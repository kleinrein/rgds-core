// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/video.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <SDL.h>
#include <ass/ass.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace media {
namespace {

constexpr size_t max_queue_bytes = 24u << 20; // packets read ahead, both queues
constexpr size_t max_pictures = 6;            // decoded pictures waiting
constexpr double max_audio_ahead = 0.6;       // seconds of decoded sound waiting
const AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;

double now_s() { return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency()); }

std::string tag(AVDictionary* d, const char* key) {
    AVDictionaryEntry* e = av_dict_get(d, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

// The text of an ASS event line, without its fields and override tags: "Hello\Nthere" -> "Hello\nthere".
std::string plain_ass(const char* dialogue) {
    std::string s = dialogue ? dialogue : "";
    // Events from FFmpeg: ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
    int commas = 0;
    size_t i = 0;
    for (; i < s.size() && commas < 8; ++i)
        if (s[i] == ',') ++commas;
    std::string out;
    bool in_tag = false;
    for (size_t j = i; j < s.size(); ++j) {
        char c = s[j];
        if (c == '{') in_tag = true;
        else if (c == '}') in_tag = false;
        else if (in_tag) continue;
        else if (c == '\\' && j + 1 < s.size() && (s[j + 1] == 'N' || s[j + 1] == 'n')) {
            out += '\n';
            ++j;
        } else if (c == '\\' && j + 1 < s.size() && s[j + 1] == 'h') {
            out += ' ';
            ++j;
        } else out += c;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\r')) out.pop_back();
    return out;
}

} // namespace

std::string clock_text(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    long t = long(seconds);
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else std::snprintf(buf, sizeof buf, "%ld:%02ld", t / 60, t % 60);
    return buf;
}

Player::Player() {}
Player::~Player() { close(); }

static void audio_callback(void* user, Uint8* out, int bytes) { static_cast<Player*>(user)->fill_audio(out, bytes); }

bool Player::open_decoder(int stream, AVCodecContext*& ctx, bool video) {
    AVStream* st = format_->streams[stream];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (video) {
        // Frame threads on three of the four cores; sound and drawing keep the fourth.
        ctx->thread_count = 3;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        // The panels are 640x480: past 720p the loop filter on frames nothing refers to costs time nobody sees.
        if (st->codecpar->height > 720) ctx->skip_loop_filter = AVDISCARD_NONREF;
        // A hardware decoder through the kernel's request API (Hantro, rkvdec), when this FFmpeg has one.
        const char* off = std::getenv("RGDS_MEDIA_SOFTWARE");
        AVHWDeviceType type = av_hwdevice_find_type_by_name("v4l2request");
        if (type != AV_HWDEVICE_TYPE_NONE && !(off && *off == '1')) {
            AVBufferRef* device = nullptr;
            if (av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) == 0) {
                ctx->hw_device_ctx = device;
                info_.hardware = true;
            }
        }
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    return true;
}

bool Player::open(const std::string& path, double start, std::string& error) {
    close();
    path_ = path;
    info_ = Info{};
    stop_ = false;
    if (avformat_open_input(&format_, path.c_str(), nullptr, nullptr) < 0) {
        error = "cannot open the file";
        return false;
    }
    format_->flags |= AVFMT_FLAG_GENPTS;
    if (avformat_find_stream_info(format_, nullptr) < 0) {
        error = "cannot read the file's streams";
        return false;
    }
    info_.container = format_->iformat ? format_->iformat->name : "";
    info_.title = tag(format_->metadata, "title");
    info_.duration = format_->duration > 0 ? format_->duration / double(AV_TIME_BASE) : 0;
    video_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_ >= 0 && (format_->streams[video_stream_]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        video_stream_ = -1; // a cover picture in a music file is not a film
    audio_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, video_stream_, nullptr, 0);
    if (video_stream_ < 0 && audio_stream_ < 0) {
        error = "no picture or sound in the file";
        return false;
    }
    for (unsigned i = 0; i < format_->nb_streams; ++i) {
        AVStream* st = format_->streams[i];
        st->discard = AVDISCARD_ALL;
        Track t;
        t.stream = int(i);
        t.title = tag(st->metadata, "title");
        t.language = tag(st->metadata, "language");
        t.codec = avcodec_get_name(st->codecpar->codec_id);
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            t.channels = st->codecpar->ch_layout.nb_channels;
            if (int(i) == audio_stream_) audio_index_ = int(info_.audio.size());
            info_.audio.push_back(t);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id);
            t.bitmap = d && (d->props & AV_CODEC_PROP_BITMAP_SUB);
            info_.subtitles.push_back(t);
        }
    }
    for (unsigned i = 0; i < format_->nb_chapters; ++i) {
        AVChapter* c = format_->chapters[i];
        Chapter ch;
        ch.start = c->start * av_q2d(c->time_base);
        ch.end = c->end * av_q2d(c->time_base);
        ch.title = tag(c->metadata, "title");
        info_.chapters.push_back(ch);
    }
    if (video_stream_ >= 0) {
        AVStream* st = format_->streams[video_stream_];
        st->discard = AVDISCARD_DEFAULT;
        if (!open_decoder(video_stream_, video_, true)) {
            error = std::string("no decoder for ") + avcodec_get_name(st->codecpar->codec_id);
            return false;
        }
        info_.w = st->codecpar->width;
        info_.h = st->codecpar->height;
        // The shape the picture is meant to have (anamorphic DVDs).
        AVRational sar = av_guess_sample_aspect_ratio(format_, st, nullptr);
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) info_.w = int(std::lround(info_.w * av_q2d(sar)));
        AVRational fr = av_guess_frame_rate(format_, st, nullptr);
        info_.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
        info_.video_codec = avcodec_get_name(st->codecpar->codec_id);
    }
    if (audio_stream_ >= 0) {
        format_->streams[audio_stream_]->discard = AVDISCARD_DEFAULT;
        if (!open_decoder(audio_stream_, audio_, false)) audio_stream_ = -1, audio_index_ = -1;
    }
    // Subtitles: the first one marked default or forced is on; otherwise off until chosen.
    ass_ = ass_library_init();
    if (ass_) {
        ass_set_extract_fonts(ass_, 1);
        for (unsigned i = 0; i < format_->nb_streams; ++i) { // fonts carried in a Matroska file
            AVStream* st = format_->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT && st->codecpar->extradata_size > 0) {
                std::string name = tag(st->metadata, "filename");
                ass_add_font(ass_, name.c_str(), reinterpret_cast<char*>(st->codecpar->extradata),
                             st->codecpar->extradata_size);
            }
        }
        ass_renderer_ = ass_renderer_init(ass_);
        if (ass_renderer_) ass_set_fonts(ass_renderer_, nullptr, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    }
    int pick = -1;
    for (size_t i = 0; i < info_.subtitles.size(); ++i) {
        int disp = format_->streams[info_.subtitles[i].stream]->disposition;
        if (disp & (AV_DISPOSITION_FORCED | AV_DISPOSITION_DEFAULT)) {
            pick = int(i);
            break;
        }
    }
    open_audio_device();
    if (start > 1 && (info_.duration <= 0 || start < info_.duration - 2)) {
        seek_target_ = start;
        seek_pending_ = true;
        seek_exact_ = true;
    }
    wall_base_ = start;
    wall_at_ = SDL_GetPerformanceCounter();
    clock_pts_ = start;
    reader_ = std::thread(&Player::read_loop, this);
    if (video_) video_thread_ = std::thread(&Player::video_loop, this);
    if (audio_) audio_thread_ = std::thread(&Player::audio_loop, this);
    if (pick >= 0) select_subtitles(pick);
    return true;
}

void Player::open_audio_device() {
    if (!audio_) return;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    SDL_AudioSpec want{}, got{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device_) {
        std::fprintf(stderr, "rgds-media: no sound device: %s\n", SDL_GetError());
        return;
    }
    device_rate_ = got.freq;
    device_samples_ = got.samples;
    eq_.configure(float(device_rate_));
    swr_alloc_set_opts2(&swr_, &stereo_layout, AV_SAMPLE_FMT_FLT, device_rate_, &audio_->ch_layout, audio_->sample_fmt,
                        audio_->sample_rate, 0, nullptr);
    if (swr_) swr_init(swr_);
}

void Player::close() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_.notify_all();
    }
    if (device_) SDL_PauseAudioDevice(device_, 1);
    if (reader_.joinable()) reader_.join();
    if (video_thread_.joinable()) video_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
    if (sub_thread_.joinable()) sub_thread_.join();
    if (device_) SDL_CloseAudioDevice(device_);
    device_ = 0;
    flush_queue(vq_);
    flush_queue(aq_);
    for (auto& p : pictures_) av_frame_free(&p.frame);
    pictures_.clear();
    av_frame_free(&shown_.frame);
    shown_ = Picture{};
    chunks_.clear();
    queued_frames_ = 0;
    if (tempo_graph_) avfilter_graph_free(&tempo_graph_);
    tempo_in_ = tempo_out_ = nullptr;
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr;
    if (swr_) swr_free(&swr_);
    if (video_) avcodec_free_context(&video_);
    if (audio_) avcodec_free_context(&audio_);
    if (subtitle_) avcodec_free_context(&subtitle_);
    if (ass_track_) ass_free_track(ass_track_);
    ass_track_ = nullptr;
    if (ass_renderer_) ass_renderer_done(ass_renderer_);
    ass_renderer_ = nullptr;
    if (ass_) ass_library_done(ass_);
    ass_ = nullptr;
    if (format_) avformat_close_input(&format_);
    lines_.clear();
    external_paths_.clear();
    bitmaps_.clear();
    bitmap_shown_ = nullptr;
    video_stream_ = audio_stream_ = subtitle_stream_ = -1;
    audio_index_ = subtitle_index_ = -1;
    paused_ = true;
    clock_valid_ = false;
    read_eof_ = false;
    seek_pending_ = false;
    seek_settling_ = false;
    shown_serial_ = ~0ull;
    shown_pts_ = -1;
    stats_ = Stats{};
}

void Player::flush_queue(PacketQueue& q) {
    for (AVPacket* p : q.items) av_packet_free(&p);
    q.items.clear();
    q.bytes = 0;
    q.eof = false;
}

// ---- Reading

void Player::read_loop() {
    AVPacket* packet = av_packet_alloc();
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (seek_pending_) {
                double target = seek_target_;
                bool exact = seek_exact_;
                seek_pending_ = false;
                lock.unlock();
                int64_t ts = int64_t(target * AV_TIME_BASE);
                avformat_seek_file(format_, -1, INT64_MIN, ts, ts, 0);
                lock.lock();
                flush_queue(vq_);
                flush_queue(aq_);
                for (auto& p : pictures_) av_frame_free(&p.frame);
                pictures_.clear();
                chunks_.clear();
                queued_frames_ = 0;
                ++serial_;
                drop_before_ = exact ? target : -1;
                clock_pts_ = target;
                clock_valid_ = false;
                wall_base_ = target;
                wall_at_ = SDL_GetPerformanceCounter();
                read_eof_ = false;
                seek_settling_ = true;
                sub_seek_to_ = target;
                ++sub_gen_; // the subtitle reader starts again from before here
                wake_.notify_all();
            }
            wake_.wait_for(lock, std::chrono::milliseconds(20),
                           [&] { return stop_ || seek_pending_ || vq_.bytes + aq_.bytes < max_queue_bytes; });
            if (stop_) break;
            if (seek_pending_ || vq_.bytes + aq_.bytes >= max_queue_bytes) continue;
            if (read_eof_) {
                wake_.wait_for(lock, std::chrono::milliseconds(50));
                continue;
            }
        }
        int r = av_read_frame(format_, packet);
        std::lock_guard<std::mutex> lock(mutex_);
        if (seek_pending_) {
            if (r >= 0) av_packet_unref(packet);
            continue;
        }
        if (r < 0) {
            read_eof_ = true;
            vq_.eof = aq_.eof = true;
            wake_.notify_all();
            continue;
        }
        if (packet->stream_index == video_stream_ || packet->stream_index == audio_stream_) {
            AVPacket* copy = av_packet_alloc();
            av_packet_move_ref(copy, packet);
            PacketQueue& q = copy->stream_index == video_stream_ ? vq_ : aq_;
            q.bytes += size_t(copy->size);
            q.items.push_back(copy);
            wake_.notify_all();
        } else {
            av_packet_unref(packet);
        }
    }
    av_packet_free(&packet);
}

// Subtitles are read by a reader of their own: the film's reader jumps to the key frame before a seek, and a line
// that began before that (still on screen) would be missed. This one starts 30 s earlier and stays 30 s ahead.
void Player::subtitle_loop(int stream) {
    AVFormatContext* f = nullptr;
    if (avformat_open_input(&f, path_.c_str(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(f, nullptr) < 0 || stream >= int(f->nb_streams)) {
        avformat_close_input(&f);
        return;
    }
    for (unsigned i = 0; i < f->nb_streams; ++i)
        f->streams[i]->discard = int(i) == stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    const double tb = av_q2d(f->streams[stream]->time_base);
    AVPacket* packet = av_packet_alloc();
    uint64_t gen = 0;
    double read_to = -1;
    bool eof = false;
    while (!stop_ && !sub_stop_) {
        if (gen != sub_gen_) {
            gen = sub_gen_;
            const double from = std::max(0.0, double(sub_seek_to_) - 30);
            int64_t ts = int64_t(from * AV_TIME_BASE);
            avformat_seek_file(f, -1, INT64_MIN, ts, ts, 0);
            std::lock_guard<std::mutex> sub(sub_mutex_);
            if (subtitle_) avcodec_flush_buffers(subtitle_);
            bitmap_shown_ = nullptr;
            bitmaps_.clear();
            if (ass_track_) ass_flush_events(ass_track_);
            lines_.clear();
            read_to = -1;
            eof = false;
        }
        if (eof || read_to > position() + 30) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            continue;
        }
        if (av_read_frame(f, packet) < 0) {
            eof = true;
            continue;
        }
        if (packet->stream_index == stream) {
            if (packet->pts != AV_NOPTS_VALUE) read_to = packet->pts * tb;
            decode_subtitle(packet, tb);
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&f);
}

void Player::stop_subtitle_reader() {
    sub_stop_ = true;
    if (sub_thread_.joinable()) sub_thread_.join();
    sub_stop_ = false;
}

void Player::decode_subtitle(AVPacket* packet, double tb) {
    std::lock_guard<std::mutex> lock(sub_mutex_);
    if (!subtitle_) return;
    AVSubtitle sub{};
    int got = 0;
    if (avcodec_decode_subtitle2(subtitle_, &sub, &got, packet) < 0 || !got) return;
    double pts = packet->pts != AV_NOPTS_VALUE ? packet->pts * tb : 0;
    double start = pts + sub.start_display_time / 1000.0;
    double end = sub.end_display_time && sub.end_display_time != UINT32_MAX ? pts + sub.end_display_time / 1000.0
                 : packet->duration > 0                                     ? pts + packet->duration * tb
                                                                            : start + 5;
    BitmapSub bmp;
    bmp.start = start;
    bmp.end = end;
    bmp.w = subtitle_->width > 0 ? subtitle_->width : info_.w;
    bmp.h = subtitle_->height > 0 ? subtitle_->height : info_.h;
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        AVSubtitleRect* r = sub.rects[i];
        if (r->type == SUBTITLE_ASS && r->ass) {
            if (ass_track_)
                ass_process_chunk(ass_track_, r->ass, int(std::strlen(r->ass)), (long long)(start * 1000),
                                  (long long)((end - start) * 1000));
            std::string text = plain_ass(r->ass);
            bool seen = false;
            for (const auto& l : lines_)
                if (l.start == start && l.text == text) seen = true;
            if (!text.empty() && !seen) lines_.push_back({start, end, text});
        } else if (r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0) {
            BitmapSub::Rect rect{r->x, r->y, r->w, r->h, {}};
            rect.px.resize(size_t(r->w) * r->h);
            const uint32_t* palette = reinterpret_cast<const uint32_t*>(r->data[1]);
            for (int y = 0; y < r->h; ++y)
                for (int x = 0; x < r->w; ++x) {
                    uint32_t argb = palette[r->data[0][y * r->linesize[0] + x]];
                    uint32_t a = argb >> 24, rr = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
                    rect.px[size_t(y) * r->w + x] = rr | (g << 8) | (b << 16) | (a << 24);
                }
            bmp.rects.push_back(std::move(rect));
        }
    }
    if (!bmp.rects.empty() || (sub.num_rects == 0 && !bitmaps_.empty())) {
        if (!bitmaps_.empty() && bitmaps_.back().end > start) bitmaps_.back().end = start; // a clearing event
        if (!bmp.rects.empty()) bitmaps_.push_back(std::move(bmp));
        while (bitmaps_.size() > 64) bitmaps_.pop_front();
    }
    while (lines_.size() > 256) lines_.erase(lines_.begin());
    avsubtitle_free(&sub);
}

// ---- Pictures

void Player::video_loop() {
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw = av_frame_alloc();
    uint64_t serial = serial_;
    while (!stop_) {
        AVPacket* packet = nullptr;
        bool flush_eof = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return stop_ || serial != serial_ ||
                       (pictures_.size() < max_pictures && (!vq_.items.empty() || vq_.eof));
            });
            if (stop_) break;
            if (serial != serial_) {
                serial = serial_;
                lock.unlock();
                avcodec_flush_buffers(video_);
                continue;
            }
            if (!vq_.items.empty()) {
                packet = vq_.items.front();
                vq_.items.pop_front();
                vq_.bytes -= size_t(packet->size);
                wake_.notify_all();
            } else {
                flush_eof = vq_.eof;
                vq_.eof = false; // one flush per end
            }
        }
        auto t0 = std::chrono::steady_clock::now();
        if (packet) {
            avcodec_send_packet(video_, packet);
            av_packet_free(&packet);
        } else if (flush_eof) {
            avcodec_send_packet(video_, nullptr);
        }
        while (!stop_) {
            int r = avcodec_receive_frame(video_, frame);
            if (r < 0) {
                if (r == AVERROR_EOF) avcodec_flush_buffers(video_);
                break;
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            AVStream* st = format_->streams[video_stream_];
            int64_t ts = frame->best_effort_timestamp;
            double pts = ts == AV_NOPTS_VALUE ? 0 : ts * av_q2d(st->time_base);
            double drop = drop_before_;
            if (drop >= 0 && pts + 0.5 / std::max(1.0, info_.fps) < drop) { // decoded only to reach the target
                av_frame_unref(frame);
                continue;
            }
            AVFrame* src = frame;
            if (frame->hw_frames_ctx) { // a hardware picture: into memory (a dma-buf import is phase 2)
                av_frame_unref(sw);
                if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
                    av_frame_unref(frame);
                    continue;
                }
                sw->pts = frame->pts;
                src = sw;
            }
            Picture p;
            p.pts = pts;
            p.serial = serial;
            p.w = src->width;
            p.h = src->height;
            AVFrame* keep = av_frame_alloc();
            AVPixelFormat f = AVPixelFormat(src->format);
            if (f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P) {
                av_frame_ref(keep, src);
                p.format = Picture::I420;
            } else if (f == AV_PIX_FMT_NV12) {
                av_frame_ref(keep, src);
                p.format = Picture::NV12;
            } else { // 10-bit, 4:2:2, 4:4:4, RGB: converted once here
                keep->format = AV_PIX_FMT_YUV420P;
                keep->width = src->width;
                keep->height = src->height;
                av_frame_get_buffer(keep, 32);
                sws_ = sws_getCachedContext(sws_, src->width, src->height, f, src->width, src->height,
                                            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_) {
                    av_frame_free(&keep);
                    av_frame_unref(frame);
                    continue;
                }
                sws_scale(sws_, src->data, src->linesize, 0, src->height, keep->data, keep->linesize);
                p.format = Picture::I420;
            }
            for (int i = 0; i < 3; ++i) {
                p.plane[i] = keep->data[i];
                p.pitch[i] = keep->linesize[i];
            }
            p.frame = keep;
            av_frame_unref(frame);
            std::unique_lock<std::mutex> lock(mutex_);
            if (serial != serial_) {
                av_frame_free(&p.frame);
                break;
            }
            ++stats_.decoded;
            stats_.decode_ms = ms;
            pictures_.push_back(p);
            if (drop >= 0) drop_before_ = -1;
            // Back-pressure: wait for room rather than decoding pictures nobody will see yet.
            wake_.wait(lock, [&] { return stop_ || serial != serial_ || pictures_.size() < max_pictures; });
        }
    }
    av_frame_free(&sw);
    av_frame_free(&frame);
}

const Picture* Player::picture(bool& fresh) {
    fresh = false;
    std::lock_guard<std::mutex> lock(mutex_);
    const double now = position() - picture_delay_;
    bool took = false;
    while (!pictures_.empty()) {
        Picture& next = pictures_.front();
        if (next.serial != serial_) { // from before a seek
            av_frame_free(&next.frame);
            pictures_.pop_front();
            continue;
        }
        // Show it when its time is within half a frame, or always the first after a seek (the still picture).
        const double half = 0.5 / std::max(1.0, info_.fps > 0 ? info_.fps : 30.0);
        const bool first = shown_serial_ != serial_;
        if (!first && next.pts > now + half) break;
        if (took) ++stats_.dropped; // a later one is due too: this one is skipped
        av_frame_free(&shown_.frame);
        shown_ = next;
        shown_serial_ = serial_;
        pictures_.pop_front();
        took = true;
        if (first) break;
    }
    if (took) {
        ++stats_.shown;
        fresh = true;
        shown_pts_ = shown_.pts;
        if (seek_settling_ && (paused_ || clock_valid_ || !audio_)) seek_settling_ = false;
        wake_.notify_all();
    }
    // While paused after a seek, the clock waits on the picture: sound has nothing to say yet.
    return shown_.frame ? &shown_ : nullptr;
}

// ---- Sound

bool Player::build_tempo(AVCodecContext* a) {
    if (tempo_graph_) avfilter_graph_free(&tempo_graph_);
    tempo_in_ = tempo_out_ = nullptr;
    tempo_built_ = speed_;
    if (std::fabs(speed_ - 1.0) < 0.001) return true;
    tempo_graph_ = avfilter_graph_alloc();
    char layout[64] = "stereo";
    av_channel_layout_describe(&a->ch_layout, layout, sizeof layout);
    char args[256];
    std::snprintf(args, sizeof args, "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d", a->sample_rate,
                  av_get_sample_fmt_name(a->sample_fmt), layout, a->sample_rate);
    if (avfilter_graph_create_filter(&tempo_in_, avfilter_get_by_name("abuffer"), "in", args, nullptr, tempo_graph_) <
            0 ||
        avfilter_graph_create_filter(&tempo_out_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr,
                                     tempo_graph_) < 0) {
        avfilter_graph_free(&tempo_graph_);
        return false;
    }
    AVFilterContext* tempo = nullptr;
    char t[32];
    std::snprintf(t, sizeof t, "tempo=%.3f", speed_);
    if (avfilter_graph_create_filter(&tempo, avfilter_get_by_name("atempo"), "tempo", t, nullptr, tempo_graph_) < 0 ||
        avfilter_link(tempo_in_, 0, tempo, 0) < 0 || avfilter_link(tempo, 0, tempo_out_, 0) < 0 ||
        avfilter_graph_config(tempo_graph_, nullptr) < 0) {
        avfilter_graph_free(&tempo_graph_);
        tempo_in_ = tempo_out_ = nullptr;
        return false;
    }
    return true;
}

void Player::audio_loop() {
    AVFrame* frame = av_frame_alloc();
    AVFrame* filtered = av_frame_alloc();
    uint64_t serial = serial_;
    std::vector<float> buffer;
    while (!stop_) {
        AVPacket* packet = nullptr;
        bool flush_eof = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return stop_ || serial != serial_ ||
                       (double(queued_frames_) / device_rate_ < max_audio_ahead && (!aq_.items.empty() || aq_.eof));
            });
            if (stop_) break;
            if (serial != serial_) {
                serial = serial_;
                lock.unlock();
                avcodec_flush_buffers(audio_);
                if (swr_) swr_init(swr_);
                tempo_built_ = -1;
                continue;
            }
            if (!aq_.items.empty()) {
                packet = aq_.items.front();
                aq_.items.pop_front();
                aq_.bytes -= size_t(packet->size);
                wake_.notify_all();
            } else {
                flush_eof = aq_.eof;
                aq_.eof = false;
            }
        }
        if (packet) {
            avcodec_send_packet(audio_, packet);
            av_packet_free(&packet);
        } else if (flush_eof) {
            avcodec_send_packet(audio_, nullptr);
        }
        while (!stop_ && avcodec_receive_frame(audio_, frame) >= 0) {
            AVStream* st = format_->streams[audio_stream_];
            int64_t ts = frame->best_effort_timestamp;
            double pts = ts == AV_NOPTS_VALUE ? -1 : ts * av_q2d(st->time_base);
            double speed = speed_;
            if (tempo_built_ != speed) build_tempo(audio_);
            AVFrame* use = frame;
            if (tempo_graph_) {
                if (av_buffersrc_add_frame(tempo_in_, frame) < 0) {
                    av_frame_unref(frame);
                    continue;
                }
                av_frame_unref(filtered);
                if (av_buffersink_get_frame(tempo_out_, filtered) < 0) continue; // atempo gathers input first
                use = filtered;
            }
            if (!swr_) {
                av_frame_unref(frame);
                continue;
            }
            int out_frames = swr_get_out_samples(swr_, use->nb_samples);
            buffer.resize(size_t(std::max(0, out_frames)) * 2);
            uint8_t* out = reinterpret_cast<uint8_t*>(buffer.data());
            int got =
                swr_convert(swr_, &out, out_frames, const_cast<const uint8_t**>(use->extended_data), use->nb_samples);
            av_frame_unref(frame);
            if (use == filtered) av_frame_unref(filtered);
            if (got <= 0) continue;
            Chunk c;
            c.samples.assign(buffer.begin(), buffer.begin() + size_t(got) * 2);
            c.pts = pts;
            c.serial = serial;
            double drop = drop_before_;
            std::lock_guard<std::mutex> lock(mutex_);
            if (serial != serial_) break;
            if (c.pts < 0) c.pts = chunks_.empty() ? clock_pts_ : chunks_.back().pts;
            // An exact seek: sound before the target is cut, to the sample.
            if (drop >= 0 && video_ == nullptr) drop_before_ = -1;
            if (drop >= 0 || (video_ && seek_settling_ && c.pts < seek_target_)) {
                double dur = double(got) / device_rate_ * speed;
                double cut_to = std::max(drop, double(seek_target_));
                if (c.pts + dur <= cut_to) continue;
                if (c.pts < cut_to) {
                    size_t skip = size_t((cut_to - c.pts) / speed * device_rate_);
                    skip = std::min(skip, size_t(got));
                    c.read = skip;
                    c.pts = cut_to;
                    c.samples.erase(c.samples.begin(), c.samples.begin() + skip * 2);
                    c.read = 0;
                }
            }
            queued_frames_ += c.samples.size() / 2;
            chunks_.push_back(std::move(c));
        }
    }
    av_frame_free(&filtered);
    av_frame_free(&frame);
}

void Player::set_eq(const std::array<float, rgds::music::eq_bands>& gain, bool on, float boost_db) {
    std::lock_guard<std::mutex> lock(mutex_);
    eq_.set(gain, on);
    eq_.set_boost(boost_db);
}

void Player::fill_audio(uint8_t* out, int bytes) {
    std::memset(out, 0, size_t(bytes));
    float* dst = reinterpret_cast<float*>(out);
    size_t want = size_t(bytes) / 8;
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) return;
    size_t done = 0;
    double end_pts = clock_pts_;
    bool any = false;
    const float vol = float(std::clamp(volume_, 0.0, 1.0));
    while (done < want && !chunks_.empty()) {
        Chunk& c = chunks_.front();
        if (c.serial != serial_) {
            queued_frames_ -= (c.samples.size() / 2 - c.read);
            chunks_.pop_front();
            continue;
        }
        size_t have = c.samples.size() / 2 - c.read;
        size_t n = std::min(have, want - done);
        const float* src = c.samples.data() + c.read * 2;
        for (size_t i = 0; i < n * 2; ++i) dst[done * 2 + i] = src[i] * vol;
        c.read += n;
        done += n;
        queued_frames_ -= n;
        end_pts = c.pts + double(c.read) / device_rate_ * speed_;
        any = true;
        if (c.read >= c.samples.size() / 2) chunks_.pop_front();
    }
    // The equaliser and its limiter over the whole block (silence after the end included: its filters ring out).
    eq_.process(dst, want);
    if (any) {
        // What was just handed over is heard after the device's own buffer: the clock is that far behind.
        clock_pts_ = end_pts - double(device_samples_) / device_rate_ * speed_;
        clock_at_ = SDL_GetPerformanceCounter();
        clock_valid_ = true;
    }
    wake_.notify_all();
}

double Player::wall() const {
    if (paused_) return wall_base_;
    return wall_base_ + (now_s() - double(wall_at_) / double(SDL_GetPerformanceFrequency())) * speed_;
}

double Player::position() const {
    if (!audio_ || !device_) return wall();
    if (!clock_valid_) return clock_pts_;
    if (paused_) return clock_pts_;
    double since = now_s() - double(clock_at_) / double(SDL_GetPerformanceFrequency());
    // Between callbacks the clock runs on; never further than one buffer, so a stalled device freezes it.
    since = std::min(since, 2.0 * device_samples_ / device_rate_);
    return clock_pts_ + since * speed_;
}

double Player::target_or_position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seek_pending_ ? seek_target_ : position();
}

void Player::play() {
    if (!paused_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended()) { // at the end: from the start again
        seek_target_ = 0;
        seek_pending_ = true;
        seek_exact_ = true;
    }
    wall_at_ = SDL_GetPerformanceCounter();
    paused_ = false;
    clock_at_ = SDL_GetPerformanceCounter();
    if (device_) SDL_PauseAudioDevice(device_, 0);
    wake_.notify_all();
}

void Player::pause() {
    if (paused_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    wall_base_ = wall();
    if (clock_valid_) clock_pts_ = position();
    paused_ = true;
    if (device_) SDL_PauseAudioDevice(device_, 1);
}

bool Player::ended() const {
    if (!read_eof_) return false;
    if (!pictures_.empty() || queued_frames_ > 0 || !vq_.items.empty() || !aq_.items.empty()) return false;
    return info_.duration <= 0 || position() >= info_.duration - 0.25 || (!audio_ && pictures_.empty());
}

void Player::set_speed(double speed) {
    speed = std::clamp(speed, 0.5, 2.0);
    std::lock_guard<std::mutex> lock(mutex_);
    wall_base_ = wall();
    wall_at_ = SDL_GetPerformanceCounter();
    if (clock_valid_) clock_pts_ = position(), clock_at_ = SDL_GetPerformanceCounter();
    speed_ = speed;
    // Sound already decoded at the old speed is let go: a seek to here re-decodes at the new one.
    if (audio_) {
        seek_target_ = clock_pts_;
        seek_pending_ = true;
        seek_exact_ = true;
    }
    wake_.notify_all();
}

void Player::seek(double seconds, bool exact) {
    if (info_.duration > 0) seconds = std::clamp(seconds, 0.0, std::max(0.0, info_.duration - 0.5));
    else seconds = std::max(0.0, seconds);
    std::lock_guard<std::mutex> lock(mutex_);
    seek_target_ = seconds;
    seek_pending_ = true;
    seek_exact_ = exact;
    seek_settling_ = true;
    clock_pts_ = seconds;
    clock_valid_ = false;
    wall_base_ = seconds;
    wall_at_ = SDL_GetPerformanceCounter();
    wake_.notify_all();
}

void Player::select_audio(int index) {
    if (index < 0 || index >= int(info_.audio.size()) || index == audio_index_) return;
    double at = position();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        wake_.notify_all();
    }
    if (audio_thread_.joinable()) audio_thread_.join();
    if (reader_.joinable()) reader_.join();
    if (video_thread_.joinable()) video_thread_.join();
    if (sub_thread_.joinable()) sub_thread_.join();
    stop_ = false;
    if (device_) SDL_LockAudioDevice(device_);
    if (audio_stream_ >= 0) format_->streams[audio_stream_]->discard = AVDISCARD_ALL;
    if (audio_) avcodec_free_context(&audio_);
    if (swr_) swr_free(&swr_);
    if (tempo_graph_) avfilter_graph_free(&tempo_graph_);
    tempo_built_ = -1;
    audio_stream_ = info_.audio[size_t(index)].stream;
    audio_index_ = index;
    format_->streams[audio_stream_]->discard = AVDISCARD_DEFAULT;
    bool ok = open_decoder(audio_stream_, audio_, false);
    if (ok && device_)
        swr_alloc_set_opts2(&swr_, &stereo_layout, AV_SAMPLE_FMT_FLT, device_rate_, &audio_->ch_layout,
                            audio_->sample_fmt, audio_->sample_rate, 0, nullptr);
    if (swr_) swr_init(swr_);
    if (device_) SDL_UnlockAudioDevice(device_);
    seek_target_ = at;
    seek_pending_ = true;
    seek_exact_ = true;
    reader_ = std::thread(&Player::read_loop, this);
    if (video_) video_thread_ = std::thread(&Player::video_loop, this);
    if (audio_) audio_thread_ = std::thread(&Player::audio_loop, this);
    if (subtitle_stream_ >= 0 && subtitle_) {
        sub_seek_to_ = at;
        ++sub_gen_;
        sub_thread_ = std::thread(&Player::subtitle_loop, this, subtitle_stream_);
    }
}

void Player::select_subtitles(int index) {
    stop_subtitle_reader();
    {
        std::lock_guard<std::mutex> sub(sub_mutex_);
        if (subtitle_) avcodec_free_context(&subtitle_);
        if (ass_track_) ass_free_track(ass_track_);
        ass_track_ = nullptr;
        lines_.clear();
        bitmaps_.clear();
        bitmap_shown_ = nullptr;
        subtitle_stream_ = -1;
        subtitle_index_ = -1;
        if (index < 0 || index >= int(info_.subtitles.size())) return;
        const Track& t = info_.subtitles[size_t(index)];
        subtitle_index_ = index;
        if (t.external) { // a file beside the film: read whole
            ass_track* track = nullptr;
            std::vector<Line> lines;
            if (parse_subtitle_file(external_paths_[size_t(-1 - t.stream)], track, lines)) {
                ass_track_ = track;
                lines_ = std::move(lines);
            }
            return;
        }
        subtitle_stream_ = t.stream;
        AVStream* st = format_->streams[subtitle_stream_];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (codec) {
            subtitle_ = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(subtitle_, st->codecpar);
            subtitle_->pkt_timebase = st->time_base;
            if (avcodec_open2(subtitle_, codec, nullptr) < 0) avcodec_free_context(&subtitle_);
        }
        if (ass_ && subtitle_ && !t.bitmap) {
            ass_track_ = ass_new_track(ass_);
            if (subtitle_->subtitle_header)
                ass_process_codec_private(ass_track_, reinterpret_cast<char*>(subtitle_->subtitle_header),
                                          subtitle_->subtitle_header_size);
        }
    }
    if (subtitle_stream_ >= 0 && subtitle_) {
        sub_seek_to_ = position();
        ++sub_gen_;
        sub_thread_ = std::thread(&Player::subtitle_loop, this, subtitle_stream_);
    }
}

bool Player::parse_subtitle_file(const std::string& path, ass_track*& track, std::vector<Line>& lines) {
    if (!ass_) return false;
    // FFmpeg reads SRT, VTT, SSA/ASS and the rest and gives ASS events back.
    AVFormatContext* f = nullptr;
    if (avformat_open_input(&f, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(f, nullptr) < 0 || f->nb_streams < 1 ||
        f->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        avformat_close_input(&f);
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(f->streams[0]->codecpar->codec_id);
    AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!c) {
        avformat_close_input(&f);
        return false;
    }
    avcodec_parameters_to_context(c, f->streams[0]->codecpar);
    c->pkt_timebase = f->streams[0]->time_base;
    if (avcodec_open2(c, codec, nullptr) < 0) {
        avcodec_free_context(&c);
        avformat_close_input(&f);
        return false;
    }
    track = ass_new_track(ass_);
    if (c->subtitle_header)
        ass_process_codec_private(track, reinterpret_cast<char*>(c->subtitle_header), c->subtitle_header_size);
    AVPacket* p = av_packet_alloc();
    while (av_read_frame(f, p) >= 0) {
        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(c, &sub, &got, p) >= 0 && got) {
            double pts = p->pts * av_q2d(f->streams[0]->time_base);
            double dur = p->duration > 0 ? p->duration * av_q2d(f->streams[0]->time_base) : 4;
            for (unsigned i = 0; i < sub.num_rects; ++i)
                if (sub.rects[i]->type == SUBTITLE_ASS && sub.rects[i]->ass) {
                    ass_process_chunk(track, sub.rects[i]->ass, int(std::strlen(sub.rects[i]->ass)),
                                      (long long)(pts * 1000), (long long)(dur * 1000));
                    std::string text = plain_ass(sub.rects[i]->ass);
                    if (!text.empty()) lines.push_back({pts, pts + dur, text});
                }
            avsubtitle_free(&sub);
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
    avcodec_free_context(&c);
    avformat_close_input(&f);
    return true;
}

bool Player::add_subtitle_file(const std::string& path, bool select) {
    // Read once now to know it is a subtitle file; read again whenever it is chosen.
    ass_track* track = nullptr;
    std::vector<Line> lines;
    if (!parse_subtitle_file(path, track, lines)) return false;
    if (track) ass_free_track(track);
    Track t;
    t.external = true;
    t.title = path.substr(path.find_last_of('/') + 1);
    t.codec = "file";
    t.stream = -1 - int(external_paths_.size());
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        external_paths_.push_back(path);
        info_.subtitles.push_back(t);
    }
    if (select) select_subtitles(int(info_.subtitles.size()) - 1);
    return true;
}

void Player::subtitles(Overlay& overlay, int w, int h, int font_scale_percent) {
    std::lock_guard<std::mutex> lock(sub_mutex_);
    overlay.changed = false;
    const double now = position() - picture_delay_;
    bool resized = overlay.w != w || overlay.h != h;
    if (resized) {
        overlay.w = w;
        overlay.h = h;
        overlay.pixels.assign(size_t(w) * h, 0);
        overlay.empty = true;
        overlay.changed = true;
    }
    if (ass_track_ && ass_renderer_) {
        if (ass_w_ != w || ass_h_ != h || ass_scale_ != font_scale_percent) {
            ass_set_frame_size(ass_renderer_, w, h);
            ass_set_storage_size(ass_renderer_, info_.w, info_.h);
            ass_set_font_scale(ass_renderer_, font_scale_percent / 100.0);
            ass_w_ = w;
            ass_h_ = h;
            ass_scale_ = font_scale_percent;
            resized = true;
        }
        int change = 0;
        ASS_Image* img = ass_render_frame(ass_renderer_, ass_track_, (long long)(now * 1000), &change);
        if (!change && !resized) return;
        std::fill(overlay.pixels.begin(), overlay.pixels.end(), 0u);
        overlay.empty = img == nullptr;
        for (; img; img = img->next) {
            uint32_t c = img->color; // RGBA, alpha is transparency
            uint32_t r = c >> 24, g = (c >> 16) & 255, b = (c >> 8) & 255, opacity = 255 - (c & 255);
            for (int y = 0; y < img->h; ++y) {
                int oy = img->dst_y + y;
                if (oy < 0 || oy >= h) continue;
                for (int x = 0; x < img->w; ++x) {
                    int ox = img->dst_x + x;
                    if (ox < 0 || ox >= w) continue;
                    uint32_t k = img->bitmap[y * img->stride + x] * opacity / 255;
                    if (!k) continue;
                    uint32_t& d = overlay.pixels[size_t(oy) * w + ox];
                    uint32_t da = d >> 24;
                    uint32_t oa = k + da * (255 - k) / 255;
                    if (!oa) continue;
                    auto ch = [&](uint32_t src, uint32_t dst) { return (src * k + dst * da * (255 - k) / 255) / oa; };
                    d = ch(r, d & 255) | (ch(g, (d >> 8) & 255) << 8) | (ch(b, (d >> 16) & 255) << 16) | (oa << 24);
                }
            }
        }
        overlay.changed = true;
        return;
    }
    // Picture subtitles: the one whose time it is, scaled from its own grid.
    const BitmapSub* on = nullptr;
    for (const auto& b : bitmaps_)
        if (now >= b.start && now < b.end) on = &b;
    double on_start = on ? on->start : -1;
    if (!resized && on_start == bitmap_shown_start_ && (on != nullptr) == (bitmap_shown_ != nullptr)) return;
    bitmap_shown_ = on;
    bitmap_shown_start_ = on_start;
    std::fill(overlay.pixels.begin(), overlay.pixels.end(), 0u);
    overlay.empty = on == nullptr;
    overlay.changed = true;
    if (!on || on->w <= 0 || on->h <= 0) return;
    for (const auto& r : on->rects)
        for (int y = 0; y < h; ++y) {
            int sy = int(int64_t(y) * on->h / h) - r.y;
            if (sy < 0 || sy >= r.h) continue;
            for (int x = 0; x < w; ++x) {
                int sx = int(int64_t(x) * on->w / w) - r.x;
                if (sx < 0 || sx >= r.w) continue;
                uint32_t p = r.px[size_t(sy) * r.w + sx];
                if (p >> 24) overlay.pixels[size_t(y) * w + x] = p;
            }
        }
}

std::string Player::subtitle_text() {
    std::lock_guard<std::mutex> lock(sub_mutex_);
    const double now = position() - picture_delay_;
    std::string out;
    for (const auto& l : lines_)
        if (now >= l.start && now < l.end) {
            if (!out.empty()) out += '\n';
            out += l.text;
        }
    return out;
}

Player::Stats Player::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---- Thumbnails and posters

namespace {

struct Grabber {
    AVFormatContext* f = nullptr;
    AVCodecContext* c = nullptr;
    int stream = -1;
    SwsContext* sws = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    double duration = 0;
    int dw = 0, dh = 0; // display shape
    ~Grabber() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        if (sws) sws_freeContext(sws);
        if (c) avcodec_free_context(&c);
        if (f) avformat_close_input(&f);
    }
    bool open(const std::string& path, std::string& error) {
        if (avformat_open_input(&f, path.c_str(), nullptr, nullptr) < 0) return error = "cannot open", false;
        if (avformat_find_stream_info(f, nullptr) < 0) return error = "no streams", false;
        stream = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream < 0) return error = "no picture", false;
        AVStream* st = f->streams[stream];
        for (unsigned i = 0; i < f->nb_streams; ++i)
            if (int(i) != stream) f->streams[i]->discard = AVDISCARD_ALL;
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return error = "no decoder", false;
        c = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(c, st->codecpar);
        c->pkt_timebase = st->time_base;
        c->thread_count = 2;
        c->skip_loop_filter = AVDISCARD_ALL; // a thumbnail: speed over the last bit of quality
        c->flags2 |= AV_CODEC_FLAG2_FAST;
        if (avcodec_open2(c, codec, nullptr) < 0) return error = "decoder failed", false;
        duration = f->duration > 0 ? f->duration / double(AV_TIME_BASE) : 0;
        dw = st->codecpar->width;
        dh = st->codecpar->height;
        AVRational sar = av_guess_sample_aspect_ratio(f, st, nullptr);
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) dw = int(std::lround(dw * av_q2d(sar)));
        return true;
    }
    // The first picture at or after the key frame before `t`.
    bool grab(double t) {
        if (st_attached()) {
            // A cover picture: just that.
        }
        int64_t ts = int64_t(t * AV_TIME_BASE);
        avformat_seek_file(f, -1, INT64_MIN, ts, ts, 0);
        avcodec_flush_buffers(c);
        int reads = 0;
        while (av_read_frame(f, packet) >= 0 && reads++ < 600) {
            if (packet->stream_index != stream) {
                av_packet_unref(packet);
                continue;
            }
            avcodec_send_packet(c, packet);
            av_packet_unref(packet);
            if (avcodec_receive_frame(c, frame) == 0) return true;
        }
        avcodec_send_packet(c, nullptr);
        return avcodec_receive_frame(c, frame) == 0;
    }
    bool st_attached() const { return false; }
    // The frame letterboxed into w x h RGBA.
    void scale(std::vector<uint32_t>& out, int w, int h) {
        out.assign(size_t(w) * h, 0xFF000000u);
        double s = std::min(double(w) / std::max(1, dw), double(h) / std::max(1, dh));
        int tw = std::max(2, int(dw * s)) & ~1, th = std::max(2, int(dh * s)) & ~1;
        tw = std::min(tw, w);
        th = std::min(th, h);
        sws = sws_getCachedContext(sws, frame->width, frame->height, AVPixelFormat(frame->format), tw, th,
                                   AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return;
        int ox = (w - tw) / 2, oy = (h - th) / 2;
        uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(out.data() + size_t(oy) * w + ox)};
        int pitch[1] = {w * 4};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, pitch);
    }
    // Mean brightness of the Y plane, sampled.
    double brightness() const {
        if (!frame->data[0]) return 0;
        uint64_t sum = 0, n = 0;
        for (int y = 0; y < frame->height; y += 8)
            for (int x = 0; x < frame->width; x += 8, ++n) sum += frame->data[0][y * frame->linesize[0] + x];
        return n ? double(sum) / n : 0;
    }
};

} // namespace

Thumbnails::~Thumbnails() { stop(); }

void Thumbnails::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Thumbnails::start(const std::string& path, int count, int w, int h) {
    stop();
    stop_ = false;
    path_ = path;
    count_ = count;
    w_ = w;
    h_ = h;
    done_.assign(size_t(count), {});
    fresh_.clear();
    thread_ = std::thread(&Thumbnails::run, this);
}

double Thumbnails::time_of(int i) const { return count_ ? duration_ * (i + 0.5) / count_ : 0; }

void Thumbnails::run() {
    Grabber g;
    std::string error;
    if (!g.open(path_, error)) return;
    duration_ = g.duration;
    if (duration_ <= 0) return;
    // Coarse to fine (every 8th, then every 4th, ...) so the whole strip has pictures early.
    std::vector<int> order;
    std::vector<bool> seen(size_t(count_), false);
    for (int step = 8; step >= 1; step /= 2)
        for (int i = 0; i < count_; i += step)
            if (!seen[size_t(i)]) seen[size_t(i)] = true, order.push_back(i);
    for (int i : order) {
        if (stop_) return;
        if (!g.grab(time_of(i))) continue;
        std::vector<uint32_t> px;
        g.scale(px, w_, h_);
        std::lock_guard<std::mutex> lock(mutex_);
        done_[size_t(i)] = std::move(px);
        fresh_.push_back(i);
    }
}

std::vector<int> Thumbnails::take_new() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> out;
    out.swap(fresh_);
    return out;
}

const uint32_t* Thumbnails::pixels(int i) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < 0 || i >= count_ || done_[size_t(i)].empty()) return nullptr;
    return done_[size_t(i)].data();
}

bool poster(const std::string& path, double fraction, int max_w, std::vector<uint32_t>& px, int& w, int& h,
            double& duration, std::string& error) {
    Grabber g;
    if (!g.open(path, error)) return false;
    duration = g.duration;
    w = std::min(max_w, std::max(16, g.dw));
    h = std::max(2, int(int64_t(w) * g.dh / std::max(1, g.dw)));
    // Skip fades and black openings: up to four tries further in.
    for (int attempt = 0; attempt < 5; ++attempt) {
        double t = g.duration > 0 ? g.duration * std::min(0.9, fraction + attempt * 0.07) : 0;
        if (!g.grab(t)) continue;
        if (g.brightness() > 28 || attempt == 4) {
            g.scale(px, w, h);
            return true;
        }
    }
    error = "no picture decoded";
    return false;
}

} // namespace media
