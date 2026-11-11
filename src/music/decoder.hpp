#pragma once
// One open music file, through FFmpeg: what it is (tags, length, format, picture) and its sound as interleaved
// stereo floats at the rate the output wants. Written against the API FFmpeg 6.1 (the OS image) and 8.0 (a
// developer's Mac) share.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include "music/tracks.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rgds::music {
inline uint64_t fnv1a(const void* data, size_t size, uint64_t value = 14695981039346656037ull) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; i++) {
        value ^= bytes[i];
        value *= 1099511628211ull;
    }
    return value;
}
inline std::string hex64(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// A decoded picture, cropped to its middle square and written as `size` x `size` 24-bit BMP.
inline bool write_square_bmp(const AVFrame* frame, int size, const std::string& path) {
    const int side = std::min(frame->width, frame->height);
    if (side <= 0) return false;
    // Crop by pointing at the middle of each plane; chroma offsets are rounded to whole chroma samples.
    const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    if (!layout || (layout->flags & AV_PIX_FMT_FLAG_HWACCEL)) return false;
    const int shift_x = layout->log2_chroma_w, shift_y = layout->log2_chroma_h;
    const int left = ((frame->width - side) / 2) >> shift_x << shift_x,
              top = ((frame->height - side) / 2) >> shift_y << shift_y;
    const uint8_t* planes[4] = {};
    int strides[4] = {};
    const bool palette = layout->flags & AV_PIX_FMT_FLAG_PAL;
    for (int p = 0; p < 4 && frame->data[p]; p++) {
        strides[p] = frame->linesize[p];
        if (palette && p == 1) {
            planes[p] = frame->data[p];
            continue;
        }
        const bool chroma = (p == 1 || p == 2) && !(layout->flags & AV_PIX_FMT_FLAG_RGB) && layout->nb_components > 2;
        const int x = chroma ? left >> shift_x : left, y = chroma ? top >> shift_y : top;
        int step = 1;
        for (int c = 0; c < layout->nb_components; c++)
            if (layout->comp[c].plane == p) step = std::max(step, layout->comp[c].step);
        planes[p] = frame->data[p] + y * frame->linesize[p] + x * step;
    }
    SwsContext* scale = sws_getContext(side, side, AVPixelFormat(frame->format), size, size, AV_PIX_FMT_BGR24,
                                       SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!scale) return false;
    const int row = (size * 3 + 3) & ~3;
    std::vector<uint8_t> pixels(size_t(row) * size_t(size));
    // BMP rows run bottom up: scale straight into them with a negative stride.
    uint8_t* target[4] = {pixels.data() + size_t(row) * size_t(size - 1), nullptr, nullptr, nullptr};
    int target_stride[4] = {-row, 0, 0, 0};
    const int done = sws_scale(scale, planes, strides, 0, side, target, target_stride);
    sws_freeContext(scale);
    if (done != size) return false;
    const uint32_t bytes = uint32_t(pixels.size()), offset = 54;
    uint8_t header[54] = {'B', 'M'};
    const auto put32 = [&](int at, uint32_t v) {
        for (int i = 0; i < 4; i++) header[at + i] = uint8_t(v >> (8 * i));
    };
    put32(2, offset + bytes);
    put32(10, offset);
    put32(14, 40);
    put32(18, uint32_t(size));
    put32(22, uint32_t(size));
    header[26] = 1;
    header[28] = 24;
    put32(34, bytes);
    const std::string temporary = path + ".part";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool ok = std::fwrite(header, 1, sizeof header, file) == sizeof header &&
                    std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    std::fclose(file);
    if (!ok || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

// Decodes one compressed picture (a tag's cover, or the first frame of an image file's stream) and writes it at
// each of `sizes` as <stem>-<size>.bmp.
inline bool write_pictures(const AVCodecParameters* parameters, const AVPacket* packet, const std::string& stem,
                           std::initializer_list<int> sizes) {
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return false;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) return false;
    bool ok = false;
    AVFrame* frame = av_frame_alloc();
    if (frame && avcodec_parameters_to_context(context, parameters) >= 0 &&
        avcodec_open2(context, codec, nullptr) >= 0 && avcodec_send_packet(context, packet) >= 0) {
        avcodec_send_packet(context, nullptr);
        if (avcodec_receive_frame(context, frame) >= 0 && frame->width > 0 && frame->width <= 8192 &&
            frame->height <= 8192) {
            ok = true;
            for (int size : sizes) ok = write_square_bmp(frame, size, stem + "-" + std::to_string(size) + ".bmp") && ok;
        }
    }
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return ok;
}

// A picture file beside the music (cover.jpg and friends).
inline bool write_pictures_from_file(const std::string& image, const std::string& stem,
                                     std::initializer_list<int> sizes) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, image.c_str(), nullptr, nullptr) < 0) return false;
    bool ok = false;
    if (avformat_find_stream_info(format, nullptr) >= 0) {
        const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        AVPacket* packet = av_packet_alloc();
        if (stream >= 0 && packet) {
            while (av_read_frame(format, packet) >= 0) {
                if (packet->stream_index == stream) {
                    ok = write_pictures(format->streams[stream]->codecpar, packet, stem, sizes);
                    break;
                }
                av_packet_unref(packet);
            }
        }
        av_packet_free(&packet);
    }
    avformat_close_input(&format);
    return ok;
}

class Decoder {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int stream = -1, out_rate = 48000;
    // What the resampler was built for: a file may change its mind mid-stream (chained Ogg, some radio rips).
    int in_rate = 0, in_format = -1, in_channels = 0;
    bool draining = false, finished = false;
    int64_t frames_out = 0;     // frames handed out since the last seek, at out_rate
    int64_t origin_ms = 0;      // where in the track frames_out started counting
    int64_t skip_until_ms = -1; // after a seek: drop what comes before the place asked for
    bool origin_known = true;

    static std::string tag(AVDictionary* a, AVDictionary* b, const char* key) {
        for (AVDictionary* d : {a, b})
            if (const AVDictionaryEntry* e = d ? av_dict_get(d, key, nullptr, 0) : nullptr)
                if (e->value && *e->value) return e->value;
        return {};
    }
    static std::string codec_label(const AVFormatContext* f, const AVCodecParameters* p) {
        const std::string container = f->iformat && f->iformat->name ? f->iformat->name : "";
        if (container.find("libopenmpt") != std::string::npos || container.find("libmodplug") != std::string::npos)
            return "Tracker";
        if (container.find("libgme") != std::string::npos) return "Chiptune";
        switch (p->codec_id) {
        case AV_CODEC_ID_MP3: return "MP3";
        case AV_CODEC_ID_MP2: return "MP2";
        case AV_CODEC_ID_FLAC: return "FLAC";
        case AV_CODEC_ID_AAC: return "AAC";
        case AV_CODEC_ID_ALAC: return "ALAC";
        case AV_CODEC_ID_VORBIS: return "Vorbis";
        case AV_CODEC_ID_OPUS: return "Opus";
        case AV_CODEC_ID_WMAV1:
        case AV_CODEC_ID_WMAV2:
        case AV_CODEC_ID_WMAPRO:
        case AV_CODEC_ID_WMALOSSLESS: return "WMA";
        case AV_CODEC_ID_APE: return "APE";
        case AV_CODEC_ID_WAVPACK: return "WavPack";
        case AV_CODEC_ID_MUSEPACK7:
        case AV_CODEC_ID_MUSEPACK8: return "Musepack";
        case AV_CODEC_ID_TTA: return "TTA";
        case AV_CODEC_ID_AC3: return "AC-3";
        case AV_CODEC_ID_EAC3: return "E-AC-3";
        case AV_CODEC_ID_DTS: return "DTS";
        case AV_CODEC_ID_DSD_LSBF:
        case AV_CODEC_ID_DSD_MSBF:
        case AV_CODEC_ID_DSD_LSBF_PLANAR:
        case AV_CODEC_ID_DSD_MSBF_PLANAR: return "DSD";
        default: break;
        }
        const std::string name = avcodec_get_name(p->codec_id);
        if (name.rfind("pcm_", 0) == 0) return container.find("aiff") != std::string::npos ? "AIFF" : "WAV";
        if (name.rfind("adpcm_", 0) == 0) return "ADPCM";
        std::string upper = name;
        for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
        return upper;
    }
    bool prepare_resampler() {
        const int channels = frame->ch_layout.nb_channels;
        if (resampler && in_rate == frame->sample_rate && in_format == frame->format && in_channels == channels)
            return true;
        swr_free(&resampler);
        AVChannelLayout input{}, output{};
        // A file that names no layout (plain WAV, raw streams) gets the usual one for its channel count.
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&input, channels);
        else av_channel_layout_copy(&input, &frame->ch_layout);
        av_channel_layout_default(&output, 2);
        const int made = swr_alloc_set_opts2(&resampler, &output, AV_SAMPLE_FMT_FLT, out_rate, &input,
                                             AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&input);
        if (made < 0 || swr_init(resampler) < 0) {
            swr_free(&resampler);
            return false;
        }
        in_rate = frame->sample_rate;
        in_format = frame->format;
        in_channels = channels;
        return true;
    }
    void convert(std::vector<float>& out) {
        if (frame->nb_samples <= 0 || frame->ch_layout.nb_channels <= 0 || !prepare_resampler()) return;
        const double base = av_q2d(format->streams[stream]->time_base);
        const int64_t stamp = frame->best_effort_timestamp;
        const int64_t start =
            format->streams[stream]->start_time == AV_NOPTS_VALUE ? 0 : format->streams[stream]->start_time;
        const int64_t at_ms = stamp == AV_NOPTS_VALUE ? -1 : int64_t(double(stamp - start) * base * 1000);
        if (!origin_known && at_ms >= 0) {
            origin_ms = std::max<int64_t>(0, at_ms);
            origin_known = true;
        }
        const int room = swr_get_out_samples(resampler, frame->nb_samples);
        if (room <= 0) return;
        const size_t before = out.size();
        out.resize(before + size_t(room) * 2);
        uint8_t* target = reinterpret_cast<uint8_t*>(out.data() + before);
        const int made =
            swr_convert(resampler, &target, room, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        out.resize(before + size_t(std::max(0, made)) * 2);
        if (made <= 0) return;
        // A seek lands on the packet before the place asked for: drop up to it, so 1:30 is 1:30.
        if (skip_until_ms >= 0) {
            const int64_t drop = at_ms < 0 ? 0 : (skip_until_ms - at_ms) * out_rate / 1000;
            if (drop >= made) {
                out.resize(before);
                return;
            }
            if (drop > 0) {
                out.erase(out.begin() + long(before), out.begin() + long(before + size_t(drop) * 2));
                origin_ms = skip_until_ms;
            }
            skip_until_ms = -1;
            frames_out = 0;
        }
        frames_out += long(out.size() - before) / 2;
    }

public:
    TrackInfo info;
    std::string error;
    // The cover inside the file, still compressed; empty when it has none.
    const AVPacket* picture() const {
        if (!format) return nullptr;
        for (unsigned i = 0; i < format->nb_streams; i++)
            if (format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
                return format->streams[i]->attached_pic.size > 0 ? &format->streams[i]->attached_pic : nullptr;
        return nullptr;
    }
    const AVCodecParameters* picture_parameters() const {
        for (unsigned i = 0; format && i < format->nb_streams; i++)
            if (format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) return format->streams[i]->codecpar;
        return nullptr;
    }
    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    ~Decoder() { close(); }
    bool is_open() const { return format != nullptr; }
    void close() {
        swr_free(&resampler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        stream = -1;
        in_rate = in_channels = 0;
        in_format = -1;
        draining = finished = false;
        frames_out = origin_ms = 0;
        skip_until_ms = -1;
        origin_known = true;
    }
    // `decode` false reads the tags and the picture only: what the library scan wants.
    bool open(const std::string& path, int rate, bool decode = true) {
        close();
        error.clear();
        info = {};
        out_rate = rate;
        char text[128];
        int result = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
        if (result >= 0) result = avformat_find_stream_info(format, nullptr);
        if (result >= 0) result = stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (result < 0) {
            av_strerror(result, text, sizeof text);
            error = text;
            close();
            return false;
        }
        AVStream* audio = format->streams[stream];
        const AVCodecParameters* parameters = audio->codecpar;
        // Every other stream (a cover, a video in an .mka) is never read.
        for (unsigned i = 0; i < format->nb_streams; i++)
            if (int(i) != stream) format->streams[i]->discard = AVDISCARD_ALL;
        AVDictionary *a = format->metadata, *b = audio->metadata; // Ogg keeps its comments on the stream
        info.title = tag(a, b, "title");
        info.artist = tag(a, b, "artist");
        info.album = tag(a, b, "album");
        info.album_artist = tag(a, b, "album_artist");
        info.genre = tag(a, b, "genre");
        info.track = std::atoi(tag(a, b, "track").c_str()); // "3/12" reads as 3
        info.disc = std::atoi(tag(a, b, "disc").c_str());
        std::string date = tag(a, b, "date");
        if (date.empty()) date = tag(a, b, "year");
        if (date.empty()) date = tag(a, b, "TYER");
        info.year = std::atoi(date.c_str());
        if (info.year < 1000 || info.year > 9999) info.year = 0;
        int64_t length = format->duration != AV_NOPTS_VALUE ? format->duration / (AV_TIME_BASE / 1000) : 0;
        if (length <= 0 && audio->duration != AV_NOPTS_VALUE)
            length = int64_t(double(audio->duration) * av_q2d(audio->time_base) * 1000);
        info.duration_ms = std::max<int64_t>(0, length);
        info.sample_rate = parameters->sample_rate;
        info.channels = parameters->ch_layout.nb_channels;
        info.bitrate = int((parameters->bit_rate > 0 ? parameters->bit_rate : format->bit_rate) / 1000);
        info.bits = parameters->bits_per_raw_sample > 0 ? parameters->bits_per_raw_sample : 0;
        info.codec = codec_label(format, parameters);
        if (!info.bits && (info.codec == "WAV" || info.codec == "AIFF")) info.bits = parameters->bits_per_coded_sample;
        if (!decode) return true;
        const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
        codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!decoder) error = "No decoder for " + info.codec;
        else if (!codec || !packet || !frame || avcodec_parameters_to_context(codec, parameters) < 0)
            error = "Out of memory";
        else {
            codec->pkt_timebase = audio->time_base;
            if ((result = avcodec_open2(codec, decoder, nullptr)) < 0) {
                av_strerror(result, text, sizeof text);
                error = text;
            }
        }
        if (!error.empty()) {
            close();
            return false;
        }
        return true;
    }
    // Appends some sound to `out` (a packet's worth, or nothing while a seek is still dropping). False at the end.
    bool read(std::vector<float>& out) {
        if (!codec || finished) return false;
        for (;;) {
            const int got = avcodec_receive_frame(codec, frame);
            if (got >= 0) {
                convert(out);
                av_frame_unref(frame);
                return true;
            }
            if (got == AVERROR_EOF) break;
            if (got != AVERROR(EAGAIN)) return true; // a broken frame: carry on with the next packet
            if (draining) break;
            int read = av_read_frame(format, packet);
            while (read >= 0 && packet->stream_index != stream) {
                av_packet_unref(packet);
                read = av_read_frame(format, packet);
            }
            if (read < 0) {
                draining = true;
                avcodec_send_packet(codec, nullptr);
                continue;
            }
            avcodec_send_packet(codec, packet); // a packet it refuses is a damaged one: skipped
            av_packet_unref(packet);
        }
        // The tail still inside the resampler.
        if (resampler) {
            const int room = swr_get_out_samples(resampler, 0);
            if (room > 0) {
                const size_t before = out.size();
                out.resize(before + size_t(room) * 2);
                uint8_t* target = reinterpret_cast<uint8_t*>(out.data() + before);
                const int made = swr_convert(resampler, &target, room, nullptr, 0);
                out.resize(before + size_t(std::max(0, made)) * 2);
                frames_out += std::max(0, made);
            }
        }
        finished = true;
        return false;
    }
    bool seek(int64_t ms) {
        if (!codec) return false;
        ms = std::max<int64_t>(0, info.duration_ms > 0 ? std::min(ms, info.duration_ms) : ms);
        AVStream* audio = format->streams[stream];
        const int64_t start = audio->start_time == AV_NOPTS_VALUE ? 0 : audio->start_time;
        const int64_t target = start + int64_t(double(ms) / 1000 / av_q2d(audio->time_base));
        if (av_seek_frame(format, stream, target, AVSEEK_FLAG_BACKWARD) < 0 &&
            av_seek_frame(format, -1, ms * (AV_TIME_BASE / 1000), AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(codec);
        swr_free(&resampler);
        draining = finished = false;
        frames_out = 0;
        origin_ms = ms;
        origin_known = false; // the first frame says where the file really landed
        skip_until_ms = ms;
        return true;
    }
    // Where the next sample `read` will append sits in the track.
    int64_t position_ms() const { return origin_ms + frames_out * 1000 / out_rate; }
    int rate() const { return out_rate; }
};
} // namespace rgds::music
