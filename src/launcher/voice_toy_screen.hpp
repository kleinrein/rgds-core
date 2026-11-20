#pragma once
// The sound toy: record your voice with the microphone, play it back as a chipmunk, a robot, a monster, backwards
// or with an echo, and draw on the lower screen to bend it. Six sounds are kept; any of them can be the sound the
// handheld plays when it starts. Like Music and PictoChat it is a level of its own over Home.
// The sound is voice_toy.hpp, mixed live by MenuAudio (a curve drawn while it plays is heard at once); recording is
// runtime/microphone_test.py --record, which streams the microphone and keeps nothing.
// Members of App in main.cpp; included once, right after the class.
#include "launcher/voice_faces.hpp"

namespace {
// The preview has no microphone: a made-up two-note "hel-lo" (a buzzy pulse through two moving formants) stands in.
std::vector<int16_t> voice_demo_hello() {
    const int rate = voice_toy::rate;
    std::vector<int16_t> out(size_t(rate) * 13 / 10, 0);
    double phase = 0, low1 = 0, band1 = 0, low2 = 0, band2 = 0;
    for (size_t i = 0; i < out.size(); i++) {
        const double t = double(i) / rate;
        // Two syllables: "he" (0.1-0.45 s) then "llo" (0.55-1.1 s), the second a little lower and longer.
        const bool first = t < .5;
        const double start = first ? .1 : .55, end = first ? .45 : 1.1;
        double envelope = 0;
        if (t > start && t < end) envelope = std::min({1.0, (t - start) / .04, (end - t) / .08});
        const double pitch = first ? 190 + 20 * (t - start) / (end - start) : 170 - 35 * (t - start) / (end - start);
        phase += pitch / rate;
        if (phase >= 1) phase -= 1;
        const double source = (phase < .08 ? 1.0 : 0.0) - .08 + .02 * std::sin(i * .37) * envelope;
        // "e" moves towards "o": the formants slide down as the word goes on.
        const double f1 = first ? 520 : 460 - 80 * (t - start), f2 = first ? 1900 : 1100 - 250 * (t - start);
        const auto bandpass = [&](double& low, double& band, double frequency, double input) {
            const double f = 2 * std::sin(3.14159265358979 * frequency / rate), q = .12;
            low += f * band;
            const double high = input - low - q * band;
            band += f * high;
            return band;
        };
        const double voice = bandpass(low1, band1, f1, source) + .6 * bandpass(low2, band2, f2, source);
        out[i] = int16_t(std::clamp(voice * envelope * 9000, -32000.0, 32000.0));
    }
    return out;
}

} // namespace

bool App::sound_toy_available() const { return preferences.sound_toy && !o.game_menu && !probe; }

fs::path App::voice_folder() const { return o.data / "sound-toy"; }

void App::voice_load() {
    if (vt.loaded) return;
    vt.loaded = true;
    const fs::path folder = voice_folder();
    for (int i = 0; i < voice_toy::slots; i++) {
        const std::string name = "slot-" + std::to_string(i + 1);
        int rate = 0;
        auto pcm = voice_toy::read_wav(folder / (name + ".wav"), &rate);
        if (rate != voice_toy::rate) pcm.clear();
        vt.clips[size_t(i)] = std::move(pcm);
        std::ifstream style(folder / (name + ".ini"));
        std::stringstream text;
        text << style.rdbuf();
        vt.styles[size_t(i)] = voice_toy::parse_style(text.str());
        vt.outlines[size_t(i)] = voice_toy::peaks(vt.clips[size_t(i)], 96);
    }
    vt.startup_slot = -1;
    std::ifstream startup(folder / "startup.ini");
    std::string line;
    while (std::getline(startup, line))
        if (line.rfind("slot=", 0) == 0) try {
                const int slot = std::stoi(line.substr(5)) - 1;
                if (slot >= 0 && slot < voice_toy::slots) vt.startup_slot = slot;
            } catch (const std::exception&) {}
    std::error_code ec;
    if (!fs::exists(folder / "startup.wav", ec)) vt.startup_slot = -1;
}

void App::voice_save_style(int slot) {
    std::error_code ec;
    fs::create_directories(voice_folder(), ec);
    const fs::path file = voice_folder() / ("slot-" + std::to_string(slot + 1) + ".ini");
    std::ofstream(file.string() + ".tmp") << voice_toy::style_text(vt.styles[size_t(slot)]);
    fs::rename(file.string() + ".tmp", file, ec);
    // The startup sound is this slot as it sounds now.
    if (slot == vt.startup_slot) voice_write_startup();
}

bool App::voice_write_startup() {
    const int slot = vt.startup_slot;
    if (slot < 0 || vt.clips[size_t(slot)].empty()) return false;
    const auto& style = vt.styles[size_t(slot)];
    auto sound = voice_toy::render_all(vt.clips[size_t(slot)], style.voice, style.bend, voice_toy::rate);
    // At most four seconds, faded out, so a long recording never holds up the start.
    const size_t limit = size_t(voice_toy::rate) * 4, fade = voice_toy::rate / 4;
    if (sound.size() > limit) {
        sound.resize(limit);
        for (size_t i = 0; i < fade; i++) sound[limit - fade + i] *= 1.f - float(i) / fade;
    }
    if (!voice_toy::write_wav(voice_folder() / "startup.wav", sound, voice_toy::rate)) return false;
    std::error_code ec;
    std::ofstream(voice_folder() / "startup.ini.tmp") << "slot=" << slot + 1 << '\n';
    fs::rename(voice_folder() / "startup.ini.tmp", voice_folder() / "startup.ini", ec);
    return !ec;
}

void App::voice_toast(const std::string& text) {
    vt.toast = text;
    vt.toast_until = s.GetTicks() + 2200;
    dirty = true;
}

void App::open_voice_toy() {
    if (vt.open || launching || probe || !sound_toy_available()) return;
    cancel_navigation();
    voice_load();
    vt.open = true;
    vt.touch = VoiceToyUi::Touch::None;
    emit_sound(MenuCue::Open);
    configure_sounds(); // no menu music under the toy
    dirty = true;
}

void App::close_voice_toy() {
    if (!vt.open) return;
    voice_record_stop(true);
    sounds.stop_toy();
    vt.open = false;
    vt.touch = VoiceToyUi::Touch::None;
    pointer_down = false;
    configure_sounds();
    emit_sound(MenuCue::Back);
    dirty = true;
}

// A game is starting or the handheld is going to sleep: the microphone lets go, the sound stops.
void App::voice_pause() {
    voice_record_stop(vt.open);
    sounds.stop_toy();
}

void App::voice_play() {
    const auto& clip = vt.clips[size_t(vt.slot)];
    if (clip.empty()) {
        voice_toast(locale.text("Press Record and say hello!"));
        emit_sound(MenuCue::Error);
        return;
    }
    const auto& style = vt.styles[size_t(vt.slot)];
    sounds.set_master(hardware.volume ? hardware.volume_percent : 100);
    sounds.play_toy(clip, style.voice, style.bend);
    vt.was_playing = true;
    dirty = true;
}

void App::voice_set_voice(int voice) {
    voice = (voice + voice_toy::voice_count) % voice_toy::voice_count;
    auto& style = vt.styles[size_t(vt.slot)];
    style.voice = voice_toy::Voice(voice);
    vt.page = voice / voice_toy::voices_per_page; // the strip follows the voice
    haptics.play(static_cast<Haptic>(MenuCue::Navigate));
    if (!vt.clips[size_t(vt.slot)].empty()) {
        voice_save_style(vt.slot);
        voice_play(); // hear it straight away, as the DSi did
    }
    dirty = true;
}

void App::voice_set_slot(int slot) {
    slot = (slot + voice_toy::slots) % voice_toy::slots;
    if (slot == vt.slot) return;
    if (vt.recording) voice_record_stop(true);
    sounds.stop_toy();
    vt.slot = slot;
    vt.page = int(vt.styles[size_t(slot)].voice) / voice_toy::voices_per_page;
    vt.erase_armed_until = 0;
    emit_sound(MenuCue::Navigate, 1);
    dirty = true;
}

// Clear: the drawing when there is one; otherwise the sound itself, on a second press within three seconds.
void App::voice_clear_bend() {
    const Uint32 now = s.GetTicks();
    auto& style = vt.styles[size_t(vt.slot)];
    dirty = true;
    if (voice_toy::bent(style.bend)) {
        style.bend.fill(0);
        sounds.bend_toy(style.bend);
        voice_save_style(vt.slot);
        voice_toast(locale.text("Drawing cleared"));
        emit_sound(MenuCue::Disable);
        return;
    }
    if (vt.clips[size_t(vt.slot)].empty()) {
        voice_toast(locale.text("Nothing to clear"));
        emit_sound(MenuCue::Error);
        return;
    }
    if (!vt.erase_armed_until || int32_t(now - vt.erase_armed_until) > 0) {
        vt.erase_armed_until = now + 3000;
        voice_toast(locale.text("Press Erase again to delete this sound"));
        emit_sound(MenuCue::Navigate);
        return;
    }
    vt.erase_armed_until = 0;
    sounds.stop_toy();
    const int slot = vt.slot;
    vt.clips[size_t(slot)].clear();
    vt.outlines[size_t(slot)].clear();
    std::error_code ec;
    const std::string name = "slot-" + std::to_string(slot + 1);
    fs::remove(voice_folder() / (name + ".wav"), ec);
    fs::remove(voice_folder() / (name + ".ini"), ec);
    if (vt.startup_slot == slot) {
        vt.startup_slot = -1;
        fs::remove(voice_folder() / "startup.wav", ec);
        fs::remove(voice_folder() / "startup.ini", ec);
    }
    voice_toast(locale.text("Sound erased"));
    emit_sound(MenuCue::Disable);
}

void App::voice_toggle_startup() {
    if (vt.clips[size_t(vt.slot)].empty()) {
        voice_toast(locale.text("Record a sound first"));
        emit_sound(MenuCue::Error);
        return;
    }
    std::error_code ec;
    if (vt.startup_slot == vt.slot) {
        vt.startup_slot = -1;
        fs::remove(voice_folder() / "startup.wav", ec);
        fs::remove(voice_folder() / "startup.ini", ec);
        voice_toast(locale.text("The handheld starts quietly again"));
        emit_sound(MenuCue::Disable);
    } else {
        vt.startup_slot = vt.slot;
        if (voice_write_startup()) {
            voice_toast(locale.text("Plays when the handheld starts"));
            emit_sound(MenuCue::Enable);
        } else {
            vt.startup_slot = -1;
            voice_toast(locale.text("Could not save the sound"));
            emit_sound(MenuCue::Error);
        }
    }
    dirty = true;
}

// ---- recording

void App::voice_record_start() {
    if (vt.recording) return;
    sounds.stop_toy();
    vt.taking.clear();
    vt.taking.reserve(voice_toy::max_samples);
    vt.line.clear();
    vt.odd.clear();
    vt.streaming = false;
    vt.recording = true;
    vt.record_at = s.GetTicks();
    configure_sounds();
    haptics.play(static_cast<Haptic>(MenuCue::Confirm));
    dirty = true;
    // The preview has no microphone: a file named by RGDS_SOUND_TOY_WAV, else the made-up "hello".
    if (o.preview) {
        vt.demo.clear();
        if (const char* named = std::getenv("RGDS_SOUND_TOY_WAV")) {
            int rate = 0;
            const auto file = voice_toy::read_wav(named, &rate);
            for (size_t i = 0; rate > 0 && i < voice_toy::max_samples; i++) {
                const size_t at = i * size_t(rate) / voice_toy::rate;
                if (at >= file.size()) break;
                vt.demo.push_back(file[at]);
            }
        }
        if (vt.demo.empty())
            for (int16_t v : voice_demo_hello()) vt.demo.push_back(v / 32768.f);
        vt.streaming = true;
        return;
    }
    int channel[2];
    if (::pipe(channel) != 0) {
        vt.recording = false;
        voice_toast(locale.text("Microphone unavailable"));
        return;
    }
    const auto helper = (o.app / "runtime/microphone_test.py").string(), data = o.data.string();
    const auto child = fork();
    if (child == 0) {
        setpgid(0, 0);
        ::close(channel[0]);
        dup2(channel[1], STDOUT_FILENO);
        ::close(channel[1]);
        execlp("python3", "python3", "-B", helper.c_str(), "--data", data.c_str(), "--record", nullptr);
        _exit(127);
    }
    ::close(channel[1]);
    if (child < 0) {
        ::close(channel[0]);
        vt.recording = false;
        voice_toast(locale.text("Microphone unavailable"));
        return;
    }
    setpgid(child, child);
    vt.worker = child;
    vt.pipe = channel[0];
    fcntl(vt.pipe, F_SETFL, fcntl(vt.pipe, F_GETFL) | O_NONBLOCK);
    fcntl(vt.pipe, F_SETFD, FD_CLOEXEC);
}

// Ends a recording. `keep` makes it this slot's sound (and plays it); otherwise it is thrown away.
void App::voice_record_stop(bool keep) {
    if (!vt.recording) return;
    vt.recording = false;
    if (vt.worker > 0) {
        kill(-vt.worker, SIGTERM);
        kill(vt.worker, SIGTERM);
        for (int i = 0; i < 30 && vt.worker > 0; i++) {
            if (waitpid(vt.worker, nullptr, WNOHANG) != 0) vt.worker = -1;
            else usleep(10000);
        }
        if (vt.worker > 0) {
            kill(vt.worker, SIGKILL);
            waitpid(vt.worker, nullptr, 0);
            vt.worker = -1;
        }
    }
    if (vt.pipe >= 0) {
        ::close(vt.pipe);
        vt.pipe = -1;
    }
    configure_sounds();
    dirty = true;
    if (!keep) {
        vt.taking.clear();
        return;
    }
    auto prepared = voice_toy::prepare(vt.taking);
    vt.taking.clear();
    vt.taking.shrink_to_fit();
    if (prepared.empty()) {
        voice_toast(locale.text("Nothing heard. Hold the handheld closer and try again."));
        emit_sound(MenuCue::Error);
        return;
    }
    const int slot = vt.slot;
    vt.clips[size_t(slot)] = std::move(prepared);
    vt.outlines[size_t(slot)] = voice_toy::peaks(vt.clips[size_t(slot)], 96);
    vt.styles[size_t(slot)].bend.fill(0); // a new word starts unbent; the voice stays as chosen
    if (!voice_toy::write_wav(voice_folder() / ("slot-" + std::to_string(slot + 1) + ".wav"), vt.clips[size_t(slot)],
                              voice_toy::rate))
        voice_toast(locale.text("Could not save the sound"));
    voice_save_style(slot);
    voice_play();
}

void App::voice_poll_record(Uint32 now) {
    if (!vt.recording) return;
    if (o.preview) {
        // The stand-in arrives as fast as a microphone would deliver it, then a quarter second of room, then it stops.
        const size_t end = std::min(voice_toy::max_samples, vt.demo.size() + voice_toy::rate / 4),
                     due = std::min(end, size_t(now - vt.record_at) * voice_toy::rate / 1000);
        while (vt.taking.size() < due)
            vt.taking.push_back(vt.taking.size() < vt.demo.size() ? int16_t(vt.demo[vt.taking.size()] * 32767.f) : 0);
        if (vt.taking.size() >= end) voice_record_stop(true);
        return;
    }
    if (vt.pipe >= 0) {
        char buffer[4096];
        ssize_t size;
        while ((size = read(vt.pipe, buffer, sizeof buffer)) > 0) {
            std::string chunk(buffer, size_t(size));
            if (!vt.streaming) {
                vt.line += chunk;
                const size_t newline = vt.line.find('\n');
                if (newline == std::string::npos) {
                    if (vt.line.size() > 64) vt.line.clear();
                    continue;
                }
                const std::string first = vt.line.substr(0, newline);
                chunk = vt.line.substr(newline + 1);
                vt.line.clear();
                if (first != "rec") {
                    voice_record_stop(false);
                    voice_toast(locale.text(first == "busy" ? "Return home before testing the physical microphone."
                                                            : "Microphone unavailable"));
                    emit_sound(MenuCue::Error);
                    return;
                }
                vt.streaming = true;
            }
            chunk = vt.odd + chunk;
            const size_t whole = chunk.size() / 2 * 2;
            for (size_t i = 0; i < whole && vt.taking.size() < voice_toy::max_samples; i += 2)
                vt.taking.push_back(int16_t(uint16_t(uint8_t(chunk[i])) | uint16_t(uint8_t(chunk[i + 1])) << 8));
            vt.odd = chunk.substr(whole);
        }
    }
    const bool ended = vt.worker > 0 && waitpid(vt.worker, nullptr, WNOHANG) != 0;
    if (ended) vt.worker = -1;
    if (vt.taking.size() >= voice_toy::max_samples || (ended && vt.streaming)) voice_record_stop(true);
    else if (ended || (!vt.streaming && now - vt.record_at > 4000)) {
        voice_record_stop(false);
        voice_toast(locale.text("Microphone unavailable"));
        emit_sound(MenuCue::Error);
    }
}

// Every turn of the main loop. True while something on the toy's screens moves.
bool App::voice_tick(Uint32 now) {
    // The startup sound: once, as Home first appears after the handheld starts.
    if (!vt.startup_played && o.boot_startup && session_initialized && library_ready && !welcome_active &&
        settings.page != Page::BootWarning) {
        vt.startup_played = true;
        // Played even with the tile hidden: the sound is the player's own choice, made on purpose. Mute is mute.
        int rate = 0;
        const auto sound = voice_toy::read_wav(voice_folder() / "startup.wav", &rate);
        if (rate == voice_toy::rate && !sound.empty() && !(hardware.volume && hardware.volume_percent == 0)) {
            sounds.set_master(hardware.volume ? hardware.volume_percent : 100);
            sounds.play_toy(sound, voice_toy::Voice::Normal, voice_toy::Bend{});
        }
    }
    if (!vt.open) {
        if (vt.recording) voice_record_stop(false);
        return false;
    }
    if (!sound_toy_available()) {
        close_voice_toy();
        return false;
    }
    voice_poll_record(now);
    const bool playing = sounds.toy_playing();
    if (vt.was_playing && !playing) {
        vt.was_playing = false;
        dirty = true;
    }
    if (!vt.toast.empty() && int32_t(now - vt.toast_until) >= 0) {
        vt.toast.clear();
        dirty = true;
    }
    const bool moving = vt.recording || playing;
    // About 30 frames a second is plenty for a playhead, a level and a talking face.
    if (moving && now - vt.drawn_at >= 33) {
        vt.drawn_at = now;
        dirty = true;
    }
    return moving;
}

// ---- drawing

// A face for each voice (voice_faces.hpp), centred on (cx, cy) in a square `size` across. `mouth` (0 to 1) opens it
// as the sound plays, in five steps, each baked once.
void App::voice_face(SDL_Renderer* r, int voice, int cx, int cy, int size, float mouth) {
    size = std::clamp(size, 8, 320);
    voice = std::clamp(voice, 0, voice_toy::voice_count - 1);
    const int level = std::clamp(int(std::lround(mouth * 4)), 0, 4);
    const auto key = std::make_tuple(r, voice, size, level);
    auto found = vt.faces.find(key);
    if (found == vt.faces.end()) {
        // Talking frames go first when the cache is full; the quiet faces on screen stay.
        if (vt.faces.size() >= 96) {
            for (auto at = vt.faces.begin(); at != vt.faces.end();) {
                if (std::get<3>(at->first) > 0 || vt.faces.size() >= 140) {
                    if (at->second) s.DestroyTexture(at->second);
                    at = vt.faces.erase(at);
                } else ++at;
            }
        }
        const auto pixels = voice_faces::paint(voice_toy::Voice(voice), size, float(level) / 4).argb();
        SDL_Texture* texture = s.CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
        if (texture && s.UpdateTexture(texture, nullptr, pixels.data(), size * 4) != 0) {
            s.DestroyTexture(texture);
            texture = nullptr;
        }
        if (texture) s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        found = vt.faces.emplace(key, texture).first;
    }
    if (!found->second) return;
    const SDL_Rect to{cx - size / 2, cy - size / 2, size, size};
    s.RenderCopy(r, found->second, nullptr, &to);
}

// A recording drawn as mirrored bars between x and x + w around the line cy.
void App::voice_outline(SDL_Renderer* r, const std::vector<float>& columns, int x, int cy, int w, int half, Color c,
                        double played) {
    if (columns.empty() || w <= 0) return;
    const int count = int(columns.size()), pitch = std::max(2, w / count);
    for (int i = 0; i < count; i++) {
        const int h = std::max(1, int(columns[size_t(i)] * half));
        const int bx = x + i * w / count;
        const Color ink = played >= 0 && double(i) / count < played ? theme.at("accent") : c;
        tint_rect(r, bx, cy - h, std::max(1, pitch - 1), h * 2, ink);
    }
}

void App::voice_top(SDL_Renderer* r, int y) {
    using namespace voice_layout;
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y + 48, 432);
    status_bar(r, y);
    const int slot = vt.slot;
    const auto& style = vt.styles[size_t(slot)];
    const bool have = !vt.clips[size_t(slot)].empty(), playing = sounds.toy_playing();
    settings_card(r, 24, y + 64, 592, 248);
    const int voice = int(style.voice);
    const float mouth = playing ? sounds.toy_level() : vt.recording ? .15f : 0.f;
    voice_face(r, voice, 132, y + 186, 204, mouth);
    const int tx = 248, tw = 352;
    label(r, voice_toy::voice_names[size_t(voice)], tx, y + 92, 36, "ink", tw);
    if (vt.recording) {
        const double seconds = double(vt.taking.size()) / voice_toy::rate;
        char clock[24];
        std::snprintf(clock, sizeof clock, "%.1f / %d s", seconds, voice_toy::max_seconds);
        fill_round(r, tx, y + 152, 20, 20, 10, Color{226, 60, 60});
        label(r, "Recording…", tx + 30, y + 148, 22, "ink", tw - 30);
        raw_label(r, clock, tx, y + 184, 18, "muted", tw);
        // How long there is left: a bar that fills to ten seconds.
        round_rect(r, tx, y + 220, tw - 8, 12, 6, "slot");
        const int filled = int((tw - 8) * std::min(1.0, seconds / voice_toy::max_seconds));
        if (filled > 0) fill_round(r, tx, y + 220, std::max(12, filled), 12, 6, Color{226, 60, 60});
        label(r, "Press X again to stop.", tx, y + 252, 16, "muted", tw);
    } else if (!have) {
        wrapped(r, "Press Record and say hello!", tx, y + 152, 22, tw, 2);
        wrapped(r, "Then pick a voice and draw on the lower screen to bend it.", tx, y + 214, 16, tw, 3);
    } else {
        char length[24];
        std::snprintf(length, sizeof length, "%.1f s", double(vt.clips[size_t(slot)].size()) / voice_toy::rate);
        const std::string name = locale.text("Sound " + std::to_string(slot + 1));
        raw_label(r, name + "  ·  " + length, tx, y + 146, 18, "muted", tw);
        voice_outline(r, vt.outlines[size_t(slot)], tx, y + 214, tw - 8, 30, theme.at("line"),
                      playing ? sounds.toy_progress() : -1);
        if (vt.startup_slot == slot) {
            const std::string chip = locale.text("Startup sound");
            const int width = label_width(chip, 14, false) + 44;
            round_rect(r, tx, y + 262, width, 28, 14, "accent");
            sparkle(r, tx + 16, y + 276, 7, theme.at("on_accent"), 255);
            raw_label_colour(r, chip, tx + 30, centred_label_y(chip, 14, y + 276), 14, theme.at("on_accent"),
                             width - 34);
        }
    }
    // The six sounds, like pages of a little sound book.
    for (int i = 0; i < voice_toy::slots; i++) {
        const int x = slot_x(i), top = y + slots_y;
        const bool selected = i == slot, filled = !vt.clips[size_t(i)].empty();
        alpha_round(r, x, top + 3, slot_w, slot_h, card_radius, shadow_colour(), preferences.dark ? 60 : 18);
        round_rect(r, x, top, slot_w, slot_h, card_radius, selected ? "accent" : "line");
        round_rect(r, x + 1, top + 1, slot_w - 2, slot_h - 2, card_radius - 1, "panel");
        const std::string number = std::to_string(i + 1);
        raw_label(r, number, x + 10, top + 6, 16, selected ? "accent" : "muted", 30);
        if (filled) {
            voice_face(r, int(vt.styles[size_t(i)].voice), x + slot_w - 24, top + 22, 36, 0);
            voice_outline(r, voice_toy::fewer(vt.outlines[size_t(i)], 18), x + 10, top + 58, slot_w - 20, 14,
                          theme.at(selected ? "accent" : "muted"), -1);
        } else {
            tint_rect(r, x + slot_w / 2 - 10, top + 52, 20, 2, theme.at("line"));
            tint_rect(r, x + slot_w / 2 - 1, top + 43, 2, 20, theme.at("line"));
        }
        if (vt.startup_slot == i) sparkle(r, x + slot_w - 8, top + 8, 8, theme.at("accent"), 255);
        if (selected) chat_brackets(r, x, top, slot_w, slot_h, theme.at("accent"), 4);
    }
    // L and R in the lower corners, as on every screen with shoulders: the sounds either side, round the ends. A, X
    // and Y are on the lower screen's buttons; Start (the startup sound) has none, so it is named between the tabs.
    const int previous = (slot + voice_toy::slots - 1) % voice_toy::slots, next = (slot + 1) % voice_toy::slots;
    shoulder_tab(r, y, false, "L", locale.text("Sound " + std::to_string(previous + 1)));
    shoulder_tab(r, y, true, "R", locale.text("Sound " + std::to_string(next + 1)));
    key_hints(r, 320, y + top_card_y + top_card_h - 18, {{"Start", "Startup sound"}}, 0, 16, 300);
}

// The top card on Home while the Sound toy tile is selected (carousel layout).
void App::voice_home_top(SDL_Renderer* r, int y) {
    voice_face(r, 0, 320, y + 150, 132, 0);
    centered(r, "Sound toy", y + 216, 2, "ink");
    const std::string note = locale.text("Record your voice and change it");
    raw_label(r, note, 320 - std::min(500, label_width(note, 18, false)) / 2, y + 262, 18, "muted", 500);
    key_hints(r, 320, y + 432, {{"A", "Open"}}, 0);
}

void App::voice_pad(SDL_Renderer* r, int y) {
    using namespace voice_layout;
    const SDL_Rect b{pad.x, y + pad.y, pad.w, pad.h};
    const int mid = b.y + b.h / 2, half = b.h / 2 - 10;
    round_rect(r, b.x, b.y, b.w, b.h, card_radius, "line");
    round_rect(r, b.x + 1, b.y + 1, b.w - 2, b.h - 2, card_radius - 1, "panel");
    // Ruled like PictoChat's paper; the middle rule (no bend) a little stronger, an octave up and down at the edges.
    const Color rule = blend(theme.at("panel"), theme.at("line"), 60);
    for (int line = b.y + 24; line < b.y + b.h - 8; line += 24) tint_rect(r, b.x + 12, line, b.w - 24, 1, rule);
    for (int x = b.x + 12; x < b.x + b.w - 12; x += 8) tint_rect(r, x, mid, 4, 2, theme.at("line"));
    const auto& clip = vt.clips[size_t(vt.slot)];
    const auto& style = vt.styles[size_t(vt.slot)];
    if (vt.recording) {
        // The word appears as it is said, left to right across ten seconds.
        const size_t count = vt.taking.size();
        const int columns = 148, shown = int(std::min<size_t>(columns, count * columns / voice_toy::max_samples));
        for (int c = 0; c < shown; c++) {
            const size_t from = voice_toy::max_samples * size_t(c) / columns,
                         to = std::min(count, voice_toy::max_samples * size_t(c + 1) / columns);
            int top = 0;
            for (size_t i = from; i < to; i++) top = std::max(top, std::abs(int(vt.taking[i])));
            const int h = std::max(1, top * half / 32768);
            tint_rect(r, b.x + 12 + c * 4, mid - h, 3, h * 2, Color{226, 60, 60});
        }
        return;
    }
    if (clip.empty()) {
        voice_face(r, int(style.voice), 320, mid - 30, 84, 0);
        centered_wrapped(r, locale.text("Press Record and say hello!"), mid + 18, 18, 520, 2, "muted");
        return;
    }
    voice_outline(r, vt.outlines[size_t(vt.slot)], b.x + 16, mid, b.w - 32, half, theme.at("line"), -1);
    label(r, "Higher", b.x + 14, b.y + 6, 14, "muted", 200);
    label(r, "Lower", b.x + 14, b.y + b.h - 24, 14, "muted", 200);
    // The curve, thick enough to see under a finger.
    if (voice_toy::bent(style.bend)) {
        const Color accent = theme.at("accent");
        s.SetRenderDrawColor(r, accent.r, accent.g, accent.b, 255);
        int px = -1, py = 0;
        for (int i = 0; i < voice_toy::bend_points; i++) {
            const int x = b.x + 16 + i * (b.w - 32) / (voice_toy::bend_points - 1),
                      yy = mid - int(std::lround(style.bend[size_t(i)] / voice_toy::bend_octaves * half));
            if (px >= 0)
                for (int t = -2; t <= 2; t++) s.RenderDrawLine(r, px, py + t, x, yy + t);
            px = x;
            py = yy;
        }
    }
    if (sounds.toy_playing()) {
        const int x = b.x + 16 + int(std::lround(sounds.toy_progress() * (b.w - 32)));
        tint_rect(r, x - 1, b.y + 6, 3, b.h - 12, theme.at("accent"));
        const float bend = voice_toy::bend_at(style.bend, sounds.toy_progress());
        const int dot = mid - int(std::lround(bend / voice_toy::bend_octaves * half));
        fill_round(r, x - 8, dot - 8, 16, 16, 8, theme.at("accent"));
    }
}

void App::voice_bottom(SDL_Renderer* r, int y) {
    using namespace voice_layout;
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    const bool held = pointer_down && vt.touch == VoiceToyUi::Touch::Button;
    back_button(r, 28, y + 20, held && vt.button == 0);
    label(r, "Sound toy", 88, y + 25, 28, "ink", startup.x - 100);
    rect(r, 24, y + 77, 592, 1, "line");
    rect(r, 24, y + 76, 44, 2, "accent");
    {
        // Startup sound: lit while this sound is the one the handheld starts with.
        const bool lit = vt.startup_slot == vt.slot, pressed = held && vt.button == 1;
        raised_pill(r, startup.x, y + startup.y, startup.w, startup.h, 12, theme.at(lit ? "accent" : "panel"),
                    pressed ? 1 : 0);
        const int sink = pressed ? 2 : 0;
        const Color ink = theme.at(lit ? "on_accent" : "accent");
        sparkle(r, startup.x + 24, y + startup.y + startup.h / 2 + sink, 9, ink, 255);
        label_colour(r, "Startup sound", startup.x + 42,
                     centred_label_y(locale.text("Startup sound"), 16, y + startup.y + startup.h / 2 + sink), 16,
                     theme.at(lit ? "on_accent" : "ink"), startup.w - 50);
    }
    voice_pad(r, y);
    // The voices: a face and a name on each.
    const int pages = (voice_toy::voice_count + voice_toy::voices_per_page - 1) / voice_toy::voices_per_page;
    for (int place = 0; place < voice_toy::voices_per_page; place++) {
        const int i = vt.page * voice_toy::voices_per_page + place;
        if (i >= voice_toy::voice_count) break;
        const int x = voice_x(place), top = y + voices_y;
        const bool chosen = int(vt.styles[size_t(vt.slot)].voice) == i, pressed = held && vt.button == 10 + place;
        raised_pill(r, x, top, voice_w, voice_h, control_radius + 4, theme.at(chosen ? "accent" : "panel"),
                    pressed ? 1 : 0);
        const int sink = pressed ? 2 : 0;
        voice_face(r, i, x + voice_w / 2, top + 29 + sink, 54, 0);
        const std::string name = locale.text(voice_toy::voice_names[size_t(i)]);
        const int width = std::min(voice_w - 8, label_width(name, 14, false));
        raw_label(r, name, x + (voice_w - width) / 2, top + 54 + sink, 14, chosen ? "on_accent" : "ink", voice_w - 8);
    }
    // The pages: an arrow either side of a dot for each, the one shown lit.
    for (int side = 0; side < 2; side++) {
        const SDL_Rect b = side ? page_right : page_left;
        const bool pressed = held && vt.button == 30 + side;
        raised_pill(r, b.x, y + b.y, b.w, b.h, b.h / 2, theme.at(pressed ? "focus" : "panel"), pressed ? 1 : 0);
        // A chevron, three pixels thick, pointing the way the page turns.
        const Color accent = theme.at("accent");
        s.SetRenderDrawColor(r, accent.r, accent.g, accent.b, 255);
        const int cx = b.x + b.w / 2 + (side ? 2 : -2), cy = y + b.y + b.h / 2 + (pressed ? 2 : 0), d = side ? -1 : 1;
        for (int w = -1; w <= 1; w++) {
            s.RenderDrawLine(r, cx + d * 4 + w, cy - 7, cx - d * 3 + w, cy);
            s.RenderDrawLine(r, cx - d * 3 + w, cy, cx + d * 4 + w, cy + 7);
        }
    }
    for (int i = 0; i < pages; i++) {
        const int dx = 320 + (i * 2 - (pages - 1)) * 14, d = i == vt.page ? 12 : 8;
        fill_round(r, dx - d / 2, y + pages_y - d / 2, d, d, d / 2, theme.at(i == vt.page ? "accent" : "line"));
    }
    rect(r, 24, y + 424, 592, 1, "line");
    Color red{214, 52, 52};
    footer_button(r, button_x[0], y + buttons_y, button_w, vt.recording ? "Stop" : "Record", "X", true,
                  held && vt.button == 20, &red);
    footer_button(r, button_x[1], y + buttons_y, button_w, sounds.toy_playing() ? "Stop" : "Play", "A", true,
                  held && vt.button == 21);
    footer_button(r, button_x[2], y + buttons_y, button_w,
                  voice_toy::bent(vt.styles[size_t(vt.slot)].bend) ? "Clear" : "Erase", "Y", false,
                  held && vt.button == 22);
    if (!vt.toast.empty()) {
        const int width = std::min(580, label_width(vt.toast, 16, false) + 36), left = 320 - width / 2;
        alpha_round(r, left, y + 252, width, 36, 18, Color{20, 24, 30}, 230);
        raw_label_colour(r, vt.toast, left + 18, centred_label_y(vt.toast, 16, y + 270), 16, Color{255, 255, 255},
                         width - 28);
    }
}

// ---- keys and touch

void App::voice_action(int code) {
    last_activity = s.GetTicks();
    dirty = true;
    switch (code) {
    case 7: close_voice_toy(); break;
    case 3:
        if (vt.recording) voice_record_stop(true);
        else close_voice_toy();
        break;
    case 2:
        if (vt.recording) voice_record_stop(true);
        else if (sounds.toy_playing()) sounds.stop_toy();
        else voice_play();
        break;
    case 11:
        if (vt.recording) voice_record_stop(true);
        else voice_record_start();
        break;
    case 12: voice_clear_bend(); break;
    case 4: voice_toggle_startup(); break;
    case 0:
    case 1:
        if (!vt.recording) voice_set_voice(int(vt.styles[size_t(vt.slot)].voice) + (code == 0 ? -1 : 1));
        break;
    case 8:
    case 9:
    case 13:
    case 14: voice_set_slot(vt.slot + (code == 8 || code == 13 ? -1 : 1)); break;
    default: break;
    }
}

int App::voice_button_at(int x, int y) {
    using namespace voice_layout;
    if (x < 84 && y < 78) return 0;
    if (point_in(startup, x, y)) return 1;
    for (int place = 0; place < voice_toy::voices_per_page; place++)
        if (vt.page * voice_toy::voices_per_page + place < voice_toy::voice_count && x >= voice_x(place) - 4 &&
            x < voice_x(place) + voice_w + 4 && y >= voices_y - 4 && y < voices_y + voice_h + 4)
            return 10 + place;
    if (y >= page_left.y - 4 && y < page_left.y + page_left.h + 4) {
        if (x < 320 - 12 * 6 && x >= page_left.x - 20) return 30;
        if (x >= 320 + 12 * 6 && x < page_right.x + page_right.w + 20) return 31;
    }
    if (y >= buttons_y - 8)
        for (int i = 0; i < 3; i++)
            if (x >= button_x[size_t(i)] && x < button_x[size_t(i)] + button_w) return 20 + i;
    return -1;
}

void App::voice_button(int button) {
    switch (button) {
    case 0: voice_action(3); break;
    case 1: voice_toggle_startup(); break;
    case 20: voice_action(11); break;
    case 21: voice_action(2); break;
    case 22: voice_clear_bend(); break;
    case 30:
    case 31: {
        const int pages = (voice_toy::voice_count + voice_toy::voices_per_page - 1) / voice_toy::voices_per_page;
        vt.page = (vt.page + (button == 30 ? pages - 1 : 1)) % pages;
        emit_sound(MenuCue::Navigate, button == 30 ? -1 : 1);
        dirty = true;
        break;
    }
    default:
        if (button >= 10 && button < 10 + voice_toy::voices_per_page && !vt.recording)
            voice_set_voice(vt.page * voice_toy::voices_per_page + button - 10);
        break;
    }
}

void App::voice_paint(int x, int y) {
    using namespace voice_layout;
    const int mid = pad.y + pad.h / 2, half = pad.h / 2 - 10;
    const int column =
        std::clamp(int(std::lround(double(x - pad.x - 16) * (voice_toy::bend_points - 1) / (pad.w - 32))), 0,
                   voice_toy::bend_points - 1);
    const float value =
        std::clamp(float(mid - y) / half * voice_toy::bend_octaves, -voice_toy::bend_octaves, voice_toy::bend_octaves);
    auto& bend = vt.styles[size_t(vt.slot)].bend;
    if (vt.last_column < 0) voice_toy::paint(bend, column, value, column, value);
    else voice_toy::paint(bend, vt.last_column, vt.last_value, column, value);
    vt.last_column = column;
    vt.last_value = value;
    vt.painted = true;
    sounds.bend_toy(bend);
    dirty = true;
}

void App::voice_press(int x, int y) {
    using namespace voice_layout;
    last_activity = s.GetTicks();
    dirty = true;
    vt.touch = VoiceToyUi::Touch::None;
    vt.button = voice_button_at(x, y);
    if (vt.button >= 0) {
        vt.touch = VoiceToyUi::Touch::Button;
        return;
    }
    if (point_in(pad, x, y) && !vt.recording) {
        if (vt.clips[size_t(vt.slot)].empty()) {
            voice_toast(locale.text("Press Record and say hello!"));
            return;
        }
        vt.touch = VoiceToyUi::Touch::Pad;
        vt.last_column = -1;
        vt.painted = false;
        // Drawing with nothing playing starts the sound, so the bend is heard as it is drawn.
        if (!sounds.toy_playing()) voice_play();
        voice_paint(x, y);
    }
}

void App::voice_drag(int x, int y) {
    if (vt.touch == VoiceToyUi::Touch::Pad) voice_paint(x, y);
    dirty = true;
}

void App::voice_release(int x, int y) {
    const auto touch = vt.touch;
    vt.touch = VoiceToyUi::Touch::None;
    dirty = true;
    if (touch == VoiceToyUi::Touch::Button) {
        if (voice_button_at(x, y) == vt.button) voice_button(vt.button);
    } else if (touch == VoiceToyUi::Touch::Pad && vt.painted) voice_save_style(vt.slot);
    vt.button = -1;
    vt.last_column = -1;
}
