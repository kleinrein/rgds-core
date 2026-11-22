// SPDX-License-Identifier: GPL-3.0-or-later
// A film. The upper screen is the picture and nothing else (subtitles aside); the lower screen is the remote:
//
//   the status bar
//   the title, and the chapter or the subtitle line
//   the strip: pictures of the whole film under a fixed playhead; drag it to scrub (the picture above follows),
//              flick it to travel, and it coasts and settles like the launcher's lists
//   the times and a thin bar of the whole
//   back 10 s, previous chapter, play/pause, next chapter, forward 10 s
//   chips: sound track, subtitles, speed, picture, sleep
//
// After a few seconds of playing untouched the remote dims (lights down); any touch or key brings it back.
// At the end of an episode the next one in the folder is offered with a countdown.
#include "launcher/kinetics.hpp"
#include "media/app.hpp"
#include "media/video.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <regex>

namespace media {
namespace fs = std::filesystem;
using play::kit::margin;

namespace {

constexpr int bar_h = 36;
constexpr int strip_y = 118, strip_h = 90, thumb_w = 144, thumb_h = 81;
constexpr int times_y = 222;
constexpr int transport_y = 300;
constexpr int chips_y = 402;

enum Fit { FitWhole, FitFill, FitStretch, FitOriginal };
enum Button { BBack10, BPrev, BPlay, BNext, BFwd10, BAudio, BSubs, BSpeed, BPicture, BSleep, BSkipIntro, BCount };

const double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};

class Watch : public Screen {
public:
    Watch(Context& c, const Item& item, std::vector<Item> siblings)
        : Screen(c), item_(item), siblings_(std::move(siblings)) {
        open_film();
    }
    ~Watch() override { player_.close(); }
    const char* name() const override { return "watch"; }
    bool live() const override { return true; }

    void open_film() {
        Place p = c.places.get(item_.key);
        double start = p.finished() ? 0 : p.position;
        // Back a few seconds: a resumed scene is easier to follow from just before.
        if (start > 10) start -= 3;
        std::string error;
        if (!player_.open(item_.path, start, error)) {
            error_ = error;
            return;
        }
        // Subtitles beside the film: name.srt, name.en.srt, name.ass...
        const fs::path film(item_.path);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(film.parent_path(), ec)) {
            std::string n = e.path().filename().string(), ext = e.path().extension().string();
            for (auto& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if ((ext == ".srt" || ext == ".ass" || ext == ".ssa" || ext == ".vtt") &&
                n.rfind(film.stem().string(), 0) == 0)
                // Shown at once only when the film has none of its own on.
                player_.add_subtitle_file(e.path().string(), player_.subtitle_track() < 0);
        }
        if (player_.info().duration > 0) {
            int n = std::clamp(int(player_.info().duration / 20), 12, 240);
            thumbs_.start(item_.path, n, thumb_w, thumb_h);
            thumb_tex_.resize(size_t(n));
        }
        c.sound.poll(c.now, true);
        apply_eq();
        player_.play();
        wake();
        stale_ = true;
    }

    void leave() override {
        if (!error_.empty()) return;
        Place p;
        p.position = player_.position();
        p.length = player_.duration();
        if (p.length > 0 && p.position > p.length - std::min(60.0, p.length * .03)) p.position = p.length;
        c.places.set(item_.key, p);
        c.places.save();
    }

    // ---- Input

    void wake() {
        lights_.target = 1;
        touched_at_ = c.now;
    }

    void key(Key k, bool repeat) override {
        wake();
        if (sheet_.open) {
            sheet_.key(k);
            return;
        }
        if (up_next_ >= 0) {
            if (k == KA) play_next();
            else if (k == KB) up_next_ = -1, stale_ = true;
            return;
        }
        switch (k) {
        case KA:
        case KStart:
            if (!repeat) toggle();
            break;
        case KB:
            if (!repeat) {
                leave();
                c.pop();
            }
            break;
        case KLeft: seek_by(repeat ? -30 : -10); break;
        case KRight: seek_by(repeat ? 30 : 10); break;
        case KUp: seek_by(60); break;
        case KDown: seek_by(-60); break;
        case KL: chapter(-1); break;
        case KR: chapter(1); break;
        case KX: open_sheet(BAudio); break;
        case KY: open_sheet(BSubs); break;
        case KSelect: open_sheet(BPicture); break;
        case KL2: step_speed(-1); break;
        case KR2: step_speed(1); break;
        default: break;
        }
    }

    void toggle() {
        player_.toggle();
        stale_ = true;
        bump(BPlay);
    }
    void seek_by(double s) {
        player_.seek_by(s);
        flash_ = s;
        flash_at_ = c.now;
        bump(s < 0 ? BBack10 : BFwd10);
        stale_ = true;
    }
    void bump(int b) {
        bump_ = b;
        bump_at_ = c.now;
    }
    void chapter(int d) {
        const auto& ch = player_.info().chapters;
        double now = player_.position();
        if (ch.empty()) {
            seek_by(d * 60.0);
            return;
        }
        int cur = 0;
        for (int i = 0; i < int(ch.size()); ++i)
            if (now >= ch[size_t(i)].start - .5) cur = i;
        // Previous: the start of this chapter first, unless just past it.
        int to = d < 0 ? (now - ch[size_t(cur)].start > 3 ? cur : cur - 1) : cur + 1;
        if (to < 0) to = 0;
        if (to >= int(ch.size())) return;
        player_.seek(ch[size_t(to)].start + .05, true);
        bump(d < 0 ? BPrev : BNext);
        stale_ = true;
    }
    void step_speed(int d) {
        int i = 2;
        for (int k = 0; k < 6; ++k)
            if (std::abs(speeds[k] - player_.speed()) < .01) i = k;
        i = std::clamp(i + d, 0, 5);
        player_.set_speed(speeds[i]);
        stale_ = true;
    }

    Box button_box(int b) const {
        switch (b) {
        case BBack10: return {44, transport_y - 32, 72, 64};
        case BPrev: return {150, transport_y - 32, 72, 64};
        case BPlay: return {PW / 2 - 46, transport_y - 46, 92, 92};
        case BNext: return {PW - 222, transport_y - 32, 72, 64};
        case BFwd10: return {PW - 116, transport_y - 32, 72, 64};
        case BSkipIntro: return {PW - margin - 150, 52, 150, 44};
        default: {
            const int n = 5, gap = 10, w = (PW - 2 * margin - (n - 1) * gap) / n;
            int i = b - BAudio;
            return {margin + i * (w + gap), chips_y, w, 56};
        }
        }
    }

    void touch(const TouchEvent& t) override {
        wake();
        if (sheet_.open) {
            sheet_.touch(t);
            return;
        }
        if (up_next_ >= 0) {
            if (t.kind == TouchEvent::Up) {
                if (next_box().contains(int(t.x), int(t.y))) play_next();
                else up_next_ = -1;
                stale_ = true;
            }
            return;
        }
        if (t.kind == TouchEvent::Down) {
            down_x_ = t.x, down_y_ = t.y;
            moved_ = false;
            pressed_ = -1;
            if (t.y >= strip_y - 14 && t.y < strip_y + strip_h + 34) {
                // The strip: the finger takes it.
                scrubbing_ = true;
                was_playing_ = !player_.paused();
                player_.pause();
                strip_motion_.begin(t.at, int(std::lround(scrub_px())));
                scrub_from_ = scrub_px();
                stale_ = true;
            } else {
                for (int b = 0; b < BCount; ++b)
                    if ((b != BSkipIntro || intro_end_ > 0) && button_box(b).contains(int(t.x), int(t.y))) {
                        pressed_ = b;
                        press_at_ = t.at;
                        c.ripples.start(t.x, t.y, b == BPlay ? 80 : 60, t.at);
                        stale_ = true;
                    }
            }
        } else if (t.kind == TouchEvent::Move) {
            if (std::hypot(t.x - down_x_, t.y - down_y_) > 10) moved_ = true;
            if (scrubbing_) {
                const int max = int(strip_length());
                strip_motion_.drag(t.at, scrub_from_ - (t.x - down_x_), 0, max);
                scrub_to(strip_motion_.position / pps());
            } else if (moved_ && pressed_ >= 0 && !button_box(pressed_).contains(int(t.x), int(t.y))) {
                pressed_ = -1;
                c.ripples.release(t.at);
                stale_ = true;
            }
        } else {
            c.ripples.release(t.at);
            if (scrubbing_) {
                if (!moved_) {
                    // A tap on the strip: jump to that picture.
                    double at = (scrub_px() + (t.x - PW / 2.0)) / pps();
                    player_.seek(at, true);
                    scrubbing_ = false;
                    if (was_playing_) player_.play();
                } else {
                    strip_motion_.release(t.at, 0, int(strip_length()));
                    if (!strip_motion_.active()) finish_scrub();
                }
            } else if (pressed_ >= 0) {
                press(pressed_);
            }
            pressed_ = -1;
            stale_ = true;
        }
    }

    // ---- The strip

    double pps() const { // strip pixels a second
        const double d = std::max(1.0, player_.duration());
        return thumbs_.count() > 0 ? thumbs_.count() * double(thumb_w) / d : 6.0;
    }
    double strip_length() const { return std::max(1.0, player_.duration()) * pps(); }
    double scrub_px() const { return (scrubbing_ ? scrub_time_ : player_.position()) * pps(); }
    void scrub_to(double t) {
        t = std::clamp(t, 0.0, std::max(0.0, player_.duration() - .5));
        scrub_time_ = t;
        // The picture above follows, a key frame at a time, no more than every 90 ms.
        if (c.now - scrub_seek_at_ > 90) {
            player_.seek(t, false);
            scrub_seek_at_ = c.now;
        }
        stale_ = true;
    }
    void finish_scrub() {
        scrubbing_ = false;
        player_.seek(scrub_time_, true);
        if (was_playing_) player_.play();
        stale_ = true;
    }

    // ---- Buttons and sheets

    void press(int b) {
        switch (b) {
        case BPlay: toggle(); break;
        case BBack10: seek_by(-10); break;
        case BFwd10: seek_by(10); break;
        case BPrev: chapter(-1); break;
        case BNext: chapter(1); break;
        case BSkipIntro:
            if (intro_end_ > 0) player_.seek(intro_end_, true);
            break;
        default: open_sheet(b); break;
        }
    }

    void open_sheet(int b) {
        std::vector<Sheet::Row> rows;
        const Info& in = player_.info();
        auto language = [&](const Track& t, int i) {
            std::string s = !t.title.empty()      ? t.title
                            : !t.language.empty() ? t.language
                                                  : c.tr("Track") + " " + std::to_string(i + 1);
            if (!t.title.empty() && !t.language.empty() && t.title.find(t.language) == std::string::npos)
                s += " (" + t.language + ")";
            return s;
        };
        auto upper_codec = [](std::string s) {
            for (auto& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            return s;
        };
        if (b == BAudio) {
            for (int i = 0; i < int(in.audio.size()); ++i) {
                const Track& t = in.audio[size_t(i)];
                std::string ch = t.channels == 6 ? "5.1" : t.channels == 8 ? "7.1" : t.channels == 1 ? "mono" : "";
                rows.push_back(
                    {language(t, i), upper_codec(t.codec) + (ch.empty() ? "" : " " + ch), i == player_.audio_track()});
            }
            if (rows.empty()) rows.push_back({c.tr("No sound in this film"), "", false});
            const int tracks = int(in.audio.size());
            // The equaliser for the output in use: the same curve the music player uses there.
            rows.push_back({c.tr("Equaliser"), c.tr(SoundOut::route_name(c.sound.route)) + ": " + eq_name(), false});
            sheet_.show(
                c.tr("Sound"), rows, std::max(0, player_.audio_track()),
                [this, tracks](int i) {
                    if (i < tracks) player_.select_audio(i);
                    else if (i == std::max(1, tracks)) open_eq();
                    stale_ = true;
                },
                c.animations());
        } else if (b == BSubs) {
            rows.push_back({c.tr("Off"), "", player_.subtitle_track() < 0});
            for (int i = 0; i < int(in.subtitles.size()); ++i) {
                const Track& t = in.subtitles[size_t(i)];
                rows.push_back(
                    {language(t, i), t.external ? c.tr("file") : upper_codec(t.codec), i == player_.subtitle_track()});
            }
            rows.push_back({c.tr("Show below the picture"), "", subs_below_});
            rows.push_back({c.tr("Larger"), std::to_string(sub_scale_) + "%", false});
            rows.push_back({c.tr("Smaller"), "", false});
            const int n = int(in.subtitles.size());
            sheet_.show(
                c.tr("Subtitles"), rows, player_.subtitle_track() + 1,
                [this, n](int i) {
                    if (i == 0) player_.select_subtitles(-1);
                    else if (i <= n) player_.select_subtitles(i - 1);
                    else if (i == n + 1) subs_below_ = !subs_below_;
                    else if (i == n + 2) sub_scale_ = std::min(200, sub_scale_ + 15);
                    else sub_scale_ = std::max(55, sub_scale_ - 15);
                    overlay_ = Overlay{};
                    stale_ = true;
                },
                c.animations());
        } else if (b == BSpeed) {
            int at = 2;
            for (int i = 0; i < 6; ++i) {
                char s[16];
                std::snprintf(s, sizeof s, "%g×", speeds[i]);
                bool on = std::abs(speeds[i] - player_.speed()) < .01;
                if (on) at = i;
                rows.push_back({s, i == 2 ? c.tr("Normal") : "", on});
            }
            sheet_.show(
                c.tr("Speed"), rows, at,
                [this](int i) {
                    player_.set_speed(speeds[i]);
                    stale_ = true;
                },
                c.animations());
        } else if (b == BPicture) {
            const char* names[] = {"Whole picture", "Fill the screen", "Stretch", "Original size"};
            for (int i = 0; i < 4; ++i) rows.push_back({c.tr(names[i]), "", fit_ == i});
            rows.push_back({c.tr("Swap screens"), c.tr("Stand mode"), swapped_});
            char info[96];
            std::snprintf(info, sizeof info, "%dx%d %s%s", in.w, in.h, upper_codec(in.video_codec).c_str(),
                          in.hardware ? " HW" : "");
            rows.push_back({c.tr("Picture"), info, false});
            sheet_.show(
                c.tr("Picture"), rows, fit_,
                [this](int i) {
                    if (i < 4) fit_ = i;
                    else if (i == 4) {
                        swapped_ = !swapped_;
                        reset_textures(); // textures belong to one panel's renderer
                    }
                    overlay_ = Overlay{};
                    stale_ = true;
                },
                c.animations());
        } else if (b == BSleep) {
            const int mins[] = {0, 15, 30, 45, 60, 90, -1};
            for (int m : mins)
                rows.push_back({m == 0  ? c.tr("Off")
                                : m < 0 ? c.tr("End of this film")
                                        : std::to_string(m) + " min",
                                "", sleep_minutes_ == m});
            sheet_.show(
                c.tr("Sleep timer"), rows, 0,
                [this, mins](int i) {
                    sleep_minutes_ = mins[i];
                    sleep_at_ = mins[i] > 0 ? c.now + uint32_t(mins[i]) * 60000u : 0;
                    stale_ = true;
                },
                c.animations());
        }
        stale_ = true;
    }

    void reset_textures() {
        for (Tex* t : {&video_tex_, &sub_tex_, &remote_, &times_, &bar_, &knob_, &flash_tex_, &sub_line_tex_,
                       &next_tex_, &ring_tex_})
            t->reset();
        for (auto& t : thumb_tex_) t.reset();
        overlay_ = Overlay{};
        stale_ = times_stale_ = bar_stale_ = true;
        sub_line_ = "\x01";
        next_for_ = -2;
    }

    void apply_eq() {
        const auto& e = c.sound.now();
        player_.set_eq(e.gain, e.on, c.sound.boost_db());
    }
    std::string eq_name() const {
        const auto& e = c.sound.now();
        return e.on ? c.tr(rgds::music::eq_presets[size_t(std::clamp(e.preset, 0, 11))].name) : c.tr("Off");
    }
    void open_eq() {
        std::vector<Sheet::Row> rows;
        const auto& e = c.sound.now();
        rows.push_back({c.tr("Off"), "", !e.on});
        int at = 0;
        for (int i = 0; i < int(rgds::music::eq_presets.size()); ++i) {
            bool on = e.on && e.preset == i;
            if (on) at = i + 1;
            rows.push_back({c.tr(rgds::music::eq_presets[size_t(i)].name), "", on});
        }
        sheet_.show(
            c.tr("Equaliser") + " · " + c.tr(SoundOut::route_name(c.sound.route)), rows, at,
            [this](int i) {
                c.sound.choose(i - 1);
                apply_eq();
                stale_ = true;
            },
            c.animations());
    }

    // ---- Up next

    int next_index() const {
        for (size_t i = 0; i + 1 < siblings_.size(); ++i)
            if (siblings_[i].path == item_.path) return int(i + 1);
        return -1;
    }
    Box next_box() const { return {margin, 120, PW - 2 * margin, 250}; }
    void play_next() {
        int n = up_next_;
        up_next_ = -1;
        if (n < 0) return;
        leave();
        player_.close();
        thumbs_.stop();
        for (auto& t : thumb_tex_) t.reset();
        item_ = siblings_[size_t(n)];
        error_.clear();
        open_film();
    }

    // ---- Time

    bool update(uint32_t now) override {
        const double dt = last_ ? double(now - last_) : 16;
        last_ = now;
        if (!error_.empty()) return false;
        if (scrubbing_ && strip_motion_.active()) {
            int s = 0;
            strip_motion_.step(now, s, 0, int(strip_length()));
            scrub_to(strip_motion_.position / pps());
            if (!strip_motion_.active()) finish_scrub();
        }
        // A headset or headphones in or out, or the curve changed in the music player: the curve follows.
        if (c.sound.poll(now)) apply_eq();
        // Lights down after 6 s of playing untouched.
        if (!player_.paused() && !sheet_.open && !scrubbing_ && up_next_ < 0 && now - touched_at_ > 6000)
            lights_.target = .28;
        lights_.step(dt, .0016, 1.0);
        sheet_.update(dt);
        // Chapters and the intro.
        const double pos = player_.position();
        double intro = 0;
        int chap = -1;
        const auto& ch = player_.info().chapters;
        static const std::regex intro_name(R"(intro|opening|^op$|recap|previously)", std::regex::icase);
        for (int i = 0; i < int(ch.size()); ++i) {
            if (pos >= ch[size_t(i)].start - .2) chap = i;
            if (pos >= ch[size_t(i)].start && pos < ch[size_t(i)].end - 2 &&
                std::regex_search(ch[size_t(i)].title, intro_name))
                intro = ch[size_t(i)].end;
        }
        if (chap != chapter_ || (intro > 0) != (intro_end_ > 0)) stale_ = true;
        chapter_ = chap;
        intro_end_ = intro;
        // The end: the next episode, or the screen stays on the last picture.
        if (player_.ended() && up_next_ < 0 && !player_.paused()) {
            player_.pause();
            Place p;
            p.position = p.length = player_.duration();
            c.places.set(item_.key, p);
            c.places.save();
            int n = next_index();
            if (n >= 0 && sleep_minutes_ != -1) {
                up_next_ = n;
                up_next_at_ = now;
            }
            if (sleep_minutes_ == -1) sleep_minutes_ = 0;
            stale_ = true;
        }
        if (up_next_ >= 0 && now - up_next_at_ > 10000) play_next();
        if (sleep_at_ && now > sleep_at_) {
            sleep_at_ = 0;
            sleep_minutes_ = 0;
            player_.pause();
            lights_.target = .1;
            stale_ = true;
        }
        // Saved every 10 s of playing, so a flat battery loses little.
        if (now - saved_at_ > 10000 && !player_.paused()) {
            saved_at_ = now;
            leave();
        }
        // The times and the strip move every frame while playing; the rest is redrawn on change.
        int second = int(pos);
        if (second != shown_second_) shown_second_ = second, times_stale_ = true;
        if (!thumbs_.take_new().empty()) stale_strip_ = true; // uploaded where the strip is drawn
        if (now - c.power.read_at > 30000) bar_stale_ = true;
        return true;
    }

    // ---- The picture

    void draw_picture(SDL_Renderer* r) {
        fill(r, 0, 0, PW, PH, Rgb{0, 0, 0});
        if (!error_.empty()) return;
        bool fresh = false;
        const Picture* p = player_.picture(fresh);
        if (p && (fresh || !video_tex_)) {
            Uint32 fmt = p->format == Picture::NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
            if (!video_tex_ || video_tex_.w != p->w || video_tex_.h != p->h || video_fmt_ != fmt) {
                video_tex_.reset();
                video_tex_.t = SDL_CreateTexture(r, fmt, SDL_TEXTUREACCESS_STREAMING, p->w, p->h);
                video_tex_.w = p->w, video_tex_.h = p->h;
                video_fmt_ = fmt;
                SDL_SetTextureScaleMode(video_tex_.t, SDL_ScaleModeLinear);
            }
            if (p->format == Picture::NV12)
                SDL_UpdateNVTexture(video_tex_.t, nullptr, p->plane[0], p->pitch[0], p->plane[1], p->pitch[1]);
            else
                SDL_UpdateYUVTexture(video_tex_.t, nullptr, p->plane[0], p->pitch[0], p->plane[1], p->pitch[1],
                                     p->plane[2], p->pitch[2]);
        }
        if (!video_tex_) return;
        // The shape: the film's own (anamorphic pictures are wider than their pixels).
        const Info& in = player_.info();
        double fw = in.w > 0 ? in.w : video_tex_.w, fh = in.h > 0 ? in.h : video_tex_.h;
        double s = fit_ == FitFill       ? std::max(PW / fw, PH / fh)
                   : fit_ == FitOriginal ? std::min(1.0, std::min(PW / fw, PH / fh)) * 0 + 1.0
                                         : std::min(PW / fw, PH / fh);
        double dw = fit_ == FitStretch ? PW : fw * s, dh = fit_ == FitStretch ? PH : fh * s;
        double dx = (PW - dw) / 2, dy = (PH - dh) / 2;
        draw(r, video_tex_, dx, dy, dw, dh);
        video_rect_ = {int(dx), int(dy), int(dw), int(dh)};
        // Subtitles over the picture (unless they are shown below it).
        if (player_.subtitle_track() >= 0 && !subs_below_) {
            // Drawn in the panel's pixels over the whole screen, so lines may use the black bars.
            player_.subtitles(overlay_, PW, PH, sub_scale_);
            if (overlay_.changed && !overlay_.empty)
                upload(r, sub_tex_, overlay_.pixels.data(), overlay_.w, overlay_.h, true, false);
            if (!overlay_.empty) draw(r, sub_tex_, 0, 0);
        }
        // A seek by key: a soft badge says how far, and fades.
        double since = (c.now - flash_at_) / 600.0;
        if (flash_ != 0 && since < 1) {
            if (!flash_tex_ || flash_shown_ != flash_) {
                Canvas cv(150, 64);
                std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0u);
                cv.rounded_rect(0, 0, 150, 64, 32, Rgb{0, 0, 0}, 150);
                glyph(cv, flash_ < 0 ? Glyph::Back10 : Glyph::Forward10, 36, 32, 30, Rgb{255, 255, 255},
                      Rgb{30, 30, 30});
                char t[16];
                std::snprintf(t, sizeof t, "%+d s", int(flash_));
                c.font.draw(cv, 64, c.font.centred(24, 32), t, 24, Rgb{255, 255, 255});
                upload(r, flash_tex_, cv, true, true);
                flash_shown_ = flash_;
            }
            double x = flash_ < 0 ? 40 : PW - 190;
            draw(r, flash_tex_, x, PH / 2 - 32, int(255 * (1 - ease_in_out(since))));
        }
        if (flash_ != 0 && since >= 1) flash_ = 0;
    }

    // ---- The remote

    void remote_canvas(SDL_Renderer* r) {
        Canvas cv(PW, PH);
        play::backdrop(cv, c.look);
        const Look& L = c.look;
        // Title and the line under it.
        std::string title = player_.info().title.empty() ? item_.name : player_.info().title;
        c.font.draw(cv, margin, bar_h + 36, c.font.fit(title, 24, PW - 2 * margin - (intro_end_ > 0 ? 170 : 0)), 24,
                    L.ink);
        std::string sub;
        const auto& ch = player_.info().chapters;
        if (chapter_ >= 0 && chapter_ < int(ch.size()))
            sub = ch[size_t(chapter_)].title.empty() ? c.tr("Chapter") + " " + std::to_string(chapter_ + 1)
                                                     : ch[size_t(chapter_)].title;
        if (player_.speed() != 1.0) {
            char s[24];
            std::snprintf(s, sizeof s, "%g×", player_.speed());
            sub += (sub.empty() ? "" : "  ·  ") + std::string(s);
        }
        if (sleep_at_) {
            int left = int((sleep_at_ - c.now) / 60000) + 1;
            sub += (sub.empty() ? "" : "  ·  ") + c.tr("Sleep in") + " " + std::to_string(left) + " min";
        } else if (sleep_minutes_ == -1) {
            sub += (sub.empty() ? "" : "  ·  ") + c.tr("Sleep at the end");
        }
        if (!sub.empty()) c.font.draw(cv, margin, bar_h + 62, c.font.fit(sub, 16, PW - 2 * margin), 16, L.muted);
        // Skip intro.
        if (intro_end_ > 0) {
            Box b = button_box(BSkipIntro);
            play::raised(cv, L, b.x, b.y, b.w, b.h, 14, L.accent, pressed_ == BSkipIntro ? 1 : 0);
            std::string s = c.tr("Skip intro");
            c.font.draw(cv, b.x + (b.w - c.font.measure(s, 20)) / 2, c.font.centred(20, b.y + b.h / 2 - 1), s, 20,
                        L.on_accent);
        }
        // The strip's well.
        play::sunken(cv, L, {8, strip_y - 6, PW - 16, strip_h + 12}, 14);
        // Transport.
        for (int b = BBack10; b <= BFwd10; ++b) {
            Box x = button_box(b);
            double press = pressed_ == b ? 1 : 0;
            const bool big = b == BPlay;
            if (big) {
                play::soft_round(cv, x.x, x.y + 5, x.w, x.h, x.w / 2, L.shadow(), L.dark ? 90 : 40);
                play::glossy(cv, x.x, x.y + int(2 * press), x.w, x.h, x.w / 2, L.accent, press);
                glyph(cv, player_.paused() ? Glyph::Play : Glyph::Pause, x.x + x.w / 2 + (player_.paused() ? 3 : 0),
                      x.y + x.h / 2 + int(2 * press), 40, L.on_accent, L.accent);
            } else {
                play::raised(cv, L, x.x, x.y, x.w, x.h, 18, L.panel, press);
                Glyph g = b == BBack10  ? Glyph::Back10
                          : b == BFwd10 ? Glyph::Forward10
                          : b == BPrev  ? Glyph::Previous
                                        : Glyph::Next;
                glyph(cv, g, x.x + x.w / 2, x.y + x.h / 2 + int(2 * press), 30, L.ink, L.panel);
            }
        }
        // Chips.
        const Glyph chip_glyph[] = {Glyph::Audio, Glyph::Subtitles, Glyph::Speed, Glyph::Picture, Glyph::Moon};
        const char* chip_word[] = {"Sound", "Subtitles", "Speed", "Picture", "Sleep"};
        for (int b = BAudio; b <= BSleep; ++b) {
            Box x = button_box(b);
            bool lit = (b == BSubs && player_.subtitle_track() >= 0) || (b == BSpeed && player_.speed() != 1.0) ||
                       (b == BSleep && (sleep_at_ || sleep_minutes_ == -1)) || (b == BPicture && (fit_ || swapped_));
            double press = pressed_ == b ? 1 : 0;
            play::raised(cv, L, x.x, x.y, x.w, x.h, 14, lit ? L.accent : L.panel, press);
            Rgb ink = lit ? L.on_accent : L.ink;
            glyph(cv, chip_glyph[b - BAudio], x.x + x.w / 2, x.y + 20 + int(2 * press), 22, ink,
                  lit ? L.accent : L.panel);
            std::string w = c.font.fit(c.tr(chip_word[b - BAudio]), 16, x.w - 8);
            c.font.draw(cv, x.x + (x.w - c.font.measure(w, 16)) / 2, x.y + 47 + int(2 * press), w, 16, ink);
        }
        upload(r, remote_, cv);
    }

    void times_canvas(SDL_Renderer* r) {
        Canvas cv(PW, 40);
        std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0u);
        const double pos = scrubbing_ ? scrub_time_ : player_.position(), dur = player_.duration();
        c.font.draw(cv, margin, c.font.centred(20, 14), clock_text(pos), 20, c.look.ink);
        std::string rest = dur > 0 ? "-" + clock_text(std::max(0.0, dur - pos)) : "";
        c.font.draw(cv, PW - margin - c.font.measure(rest, 20), c.font.centred(20, 14), rest, 20, c.look.muted);
        if (dur > 0) {
            std::string total = clock_text(dur);
            c.font.draw(cv, (PW - c.font.measure(total, 16)) / 2, c.font.centred(16, 14), total, 16, c.look.muted);
        }
        upload(r, times_, cv, true);
        times_stale_ = false;
    }

    void draw_strip(SDL_Renderer* r) {
        SDL_Rect clip{14, strip_y, PW - 28, strip_h};
        SDL_RenderSetClipRect(r, &clip);
        const double center = PW / 2.0, at = scrubbing_ && strip_motion_.dragging ? strip_motion_.position : scrub_px();
        const int n = thumbs_.count();
        const double ty = strip_y + (strip_h - thumb_h) / 2.0;
        if (n > 0) {
            int first = std::max(0, int((at - center) / thumb_w) - 1),
                last = std::min(n - 1, int((at + center) / thumb_w) + 1);
            for (int i = first; i <= last; ++i) {
                double x = center + i * double(thumb_w) - at;
                if (!thumb_tex_[size_t(i)])
                    if (const uint32_t* px = thumbs_.pixels(i))
                        upload(r, thumb_tex_[size_t(i)], px, thumb_w, thumb_h, false, true);
                if (thumb_tex_[size_t(i)]) draw(r, thumb_tex_[size_t(i)], x + 1, ty, thumb_w - 2, thumb_h);
                else fill(r, x + 1, ty, thumb_w - 2, thumb_h, play::mix(c.look.background, c.look.line, 90));
            }
            // Past either end: the well shows.
        } else {
            fill(r, 14, ty, PW - 28, thumb_h, play::mix(c.look.background, c.look.line, 60));
        }
        // What has been watched is the light part; ahead is dimmed a little.
        fill(r, center, strip_y, PW, strip_h, Rgb{0, 0, 0}, 70);
        SDL_RenderSetClipRect(r, nullptr);
        // The playhead: an accent line with a soft glow and a handle at each end.
        fill(r, center - 4, strip_y - 4, 8, strip_h + 8, c.look.accent, 50);
        fill(r, center - 1.5, strip_y - 4, 3, strip_h + 8, c.look.accent);
        if (!knob_) {
            Canvas k(20, 20);
            std::fill(k.data(), k.data() + 400, 0u);
            play::disc(k, 10, 10, 9, c.look.accent);
            play::disc(k, 10, 10, 4, Rgb{255, 255, 255});
            upload(r, knob_, k, true, true);
        }
        draw(r, knob_, center - 10, strip_y - 14);
        draw(r, knob_, center - 10, strip_y + strip_h - 6);
        // The whole film as a thin bar under the times.
        const double dur = std::max(1.0, player_.duration());
        const double f = std::clamp((scrubbing_ ? scrub_time_ : player_.position()) / dur, 0.0, 1.0);
        const double bx = margin, bw = PW - 2 * margin, by = times_y + 34;
        fill(r, bx, by, bw, 4, c.look.line, 160);
        fill(r, bx, by, bw * f, 4, c.look.accent);
        for (const auto& ch : player_.info().chapters) // chapter ticks
            if (ch.start > 1) fill(r, bx + bw * ch.start / dur - 1, by - 2, 2, 8, c.look.background);
    }

    void draw_remote(SDL_Renderer* r) {
        if (stale_ || !remote_) {
            remote_canvas(r);
            stale_ = false;
        }
        draw(r, remote_, 0, 0);
        if (bar_stale_ || !bar_) {
            Canvas bar(PW, bar_h);
            c.power.read(c.now);
            status_bar(bar, c.look, c.font, c.power, "");
            upload(r, bar_, bar);
            bar_stale_ = false;
        }
        draw(r, bar_, 0, 0);
        draw_strip(r);
        if (times_stale_ || !times_) times_canvas(r);
        draw(r, times_, 0, times_y);
        // A key's press on a transport button: a quick pulse of the ring.
        if (bump_ >= 0 && c.now - bump_at_ < 260) {
            Box b = button_box(bump_);
            double t = (c.now - bump_at_) / 260.0;
            double rad = b.w * (.55 + .35 * ease_out(t));
            if (c.ripples.disc && r == c.lower.r) {
                SDL_SetTextureColorMod(c.ripples.disc.t, c.look.accent.r, c.look.accent.g, c.look.accent.b);
                draw(r, c.ripples.disc, b.x + b.w / 2.0 - rad, b.y + b.h / 2.0 - rad, rad * 2, rad * 2,
                     int(90 * (1 - t)));
            }
        }
        // Subtitles below: the line in a card over the title.
        if (subs_below_ && player_.subtitle_track() >= 0) {
            std::string line = player_.subtitle_text();
            if (line != sub_line_ || !sub_line_tex_) {
                sub_line_ = line;
                Canvas cv(PW, 100);
                std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0u);
                if (!line.empty()) {
                    auto lines = wrap(c.font, line, 24, PW - 2 * margin - 24, 3);
                    int h = int(lines.size()) * 30 + 16;
                    cv.rounded_rect(margin - 8, 2, PW - 2 * margin + 16, h, 14, c.look.panel, 245);
                    for (size_t i = 0; i < lines.size(); ++i)
                        c.font.draw(cv, (PW - c.font.measure(lines[i], 24)) / 2, 30 + int(i) * 30, lines[i], 24,
                                    c.look.ink);
                }
                upload(r, sub_line_tex_, cv, true);
            }
            draw(r, sub_line_tex_, 0, bar_h + 6);
        }
        // Up next.
        if (up_next_ >= 0) draw_up_next(r);
        // Lights down: the remote dims while the film plays.
        if (lights_.value < .99) fill(r, 0, 0, PW, PH, Rgb{0, 0, 0}, int(255 * (1 - lights_.value)));
    }

    void draw_up_next(SDL_Renderer* r) {
        fill(r, 0, 0, PW, PH, Rgb{0, 0, 0}, 120);
        Box b = next_box();
        if (!next_tex_ || next_for_ != up_next_) {
            Canvas cv(b.w, b.h);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0u);
            play::raised(cv, c.look, 0, 0, b.w, b.h - 6, 20, c.look.panel);
            c.font.draw(cv, 24, 40, c.tr("Up next"), 20, c.look.accent);
            auto lines = wrap(c.font, siblings_[size_t(up_next_)].name, 28, b.w - 48, 2);
            for (size_t i = 0; i < lines.size(); ++i) c.font.draw(cv, 24, 84 + int(i) * 34, lines[i], 28, c.look.ink);
            std::string hint = c.tr("Tap or press A to play now");
            c.font.draw(cv, 24, b.h - 30, hint, 16, c.look.muted);
            upload(r, next_tex_, cv, true);
            next_for_ = up_next_;
        }
        draw(r, next_tex_, b.x, b.y);
        // The countdown: a ring that closes.
        const double left = 1 - std::min(1.0, (c.now - up_next_at_) / 10000.0);
        Canvas ring(64, 64);
        std::fill(ring.data(), ring.data() + 64 * 64, 0u);
        play::ring(ring, 32, 32, 28, 5, left, c.look.accent);
        int secs = int(std::ceil(left * 10));
        c.font.draw(ring, 32 - c.font.measure(std::to_string(secs), 24) / 2, c.font.centred(24, 32),
                    std::to_string(secs), 24, c.look.ink);
        upload(r, ring_tex_, ring, true, true);
        draw(r, ring_tex_, b.x + b.w - 88, b.y + b.h - 96);
    }

    void draw_sheet(SDL_Renderer* r) {
        if (!sheet_.visible()) return;
        sheet_.draw(r, c);
        const double v = sheet_.rise.value;
        fill(r, 0, 0, PW, PH, Rgb{0, 0, 0}, int(110 * std::clamp(v, 0.0, 1.0)));
        draw(r, sheet_.tex, 0, (1 - v) * PH * .6, int(255 * std::clamp(v * 1.5, 0.0, 1.0)));
    }

    void draw_error(SDL_Renderer* r) {
        Canvas cv(PW, PH);
        play::backdrop(cv, c.look);
        play::raised(cv, c.look, margin, 120, PW - 2 * margin, 200, 18, c.look.panel);
        c.font.draw(cv, margin + 24, 170, c.tr("This film cannot be played"), 24, c.look.ink);
        auto lines = wrap(c.font, item_.name + ": " + error_, 20, PW - 2 * margin - 48, 3);
        for (size_t i = 0; i < lines.size(); ++i)
            c.font.draw(cv, margin + 24, 210 + int(i) * 26, lines[i], 20, c.look.muted);
        c.font.draw(cv, margin + 24, 300, c.tr("B: back"), 16, c.look.muted);
        upload(r, remote_, cv);
        draw(r, remote_, 0, 0);
    }

    void draw_upper(SDL_Renderer* r) override {
        if (swapped_) {
            if (!error_.empty()) draw_error(r);
            else draw_remote(r);
        } else {
            draw_picture(r);
        }
    }
    void draw_lower(SDL_Renderer* r) override {
        if (!error_.empty()) {
            draw_error(r);
            return;
        }
        if (swapped_) draw_picture(r);
        else draw_remote(r);
        draw_sheet(r);
    }

private:
    using Look = play::Look;
    Item item_;
    std::vector<Item> siblings_;
    Player player_;
    Thumbnails thumbs_;
    std::vector<Tex> thumb_tex_;
    std::string error_;
    Tex video_tex_, sub_tex_, remote_, times_, bar_, knob_, flash_tex_, sub_line_tex_, next_tex_, ring_tex_;
    Uint32 video_fmt_ = 0;
    SDL_Rect video_rect_{};
    Overlay overlay_;
    Sheet sheet_;
    Spring lights_ = [] {
        Spring s;
        s.snap(1);
        return s;
    }();
    rgds::ScrollMotion strip_motion_;
    bool stale_ = true, times_stale_ = true, bar_stale_ = true, stale_strip_ = false;
    bool scrubbing_ = false, was_playing_ = false, moved_ = false;
    bool subs_below_ = false, swapped_ = false;
    int fit_ = FitWhole, sub_scale_ = 100;
    int pressed_ = -1, bump_ = -1, chapter_ = -1, shown_second_ = -1;
    int sleep_minutes_ = 0, up_next_ = -1, next_for_ = -2;
    double scrub_time_ = 0, scrub_from_ = 0, down_x_ = 0, down_y_ = 0, intro_end_ = 0, flash_ = 0, flash_shown_ = 0;
    std::string sub_line_ = "\x01";
    uint32_t press_at_ = 0, bump_at_ = 0, touched_at_ = 0, last_ = 0, saved_at_ = 0, scrub_seek_at_ = 0, flash_at_ = 0,
             sleep_at_ = 0, up_next_at_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_watch(Context& c, const Item& item, std::vector<Item> siblings) {
    return std::make_unique<Watch>(c, item, std::move(siblings));
}

} // namespace media
