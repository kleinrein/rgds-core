#pragma once
// The sound toy's voice changer: a recording (mono, 16 kHz) played back through one of 24 voices (a chipmunk, a
// robot, a monster, backwards, a choir, a dragon...), bent up and down by a curve drawn on the lower screen. Pure C++ with no SDL, so the
// menu's audio callback can run it in real time (`Player::render`: no allocation, no locks, no file access) and the
// tests and the startup sound can run it to the end (`render_all`).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace voice_toy {

constexpr int rate = 16000;     // what the microphone helper records
constexpr int max_seconds = 10; // a recording stops by itself here
constexpr size_t max_samples = size_t(rate) * max_seconds;
constexpr size_t min_samples = rate / 5; // shorter than 0.2 s is a click, not a word
constexpr int slots = 6;                 // recordings kept, like pages in a sound book
constexpr int bend_points = 64;          // the drawn curve, one value per column across the pad
constexpr float bend_octaves = 1.f;      // the pad's top edge is an octave up, the bottom an octave down

enum class Voice {
    Normal,
    Chipmunk,
    Robot,
    Monster,
    Backwards,
    Echo, // the first page, and the numbers saved before the rest
    Alien,
    Giant,
    Ghost,
    Underwater,
    Choir,
    EightBit, // the second
    Telephone,
    Megaphone,
    Cave,
    Jelly,
    Scratch,
    Whisper, // the third
    Sloth,
    Speedy,
    Astronaut,
    Dragon,
    Baby,
    Opera, // the fourth
    Count
};
constexpr int voice_count = int(Voice::Count);
constexpr std::array<const char*, voice_count> voice_names = {
    "Normal",  "Chipmunk",   "Robot", "Monster", "Backwards", "Echo",      "Alien", "Giant",
    "Ghost",   "Underwater", "Choir", "8-bit",   "Telephone", "Megaphone", "Cave",  "Jelly",
    "Scratch", "Whisper",    "Sloth", "Speedy",  "Astronaut", "Dragon",    "Baby",  "Opera"};
constexpr int voices_per_page = 6;

// What a voice does, as a chain read top to bottom: the recording is read at `speed` (tape: faster is higher), with
// a vibrato; pitch shifters move the pitch without changing the speed (a harmony when there are two); then ring
// modulation, filters, drive, a bit crusher, the robot's comb, an echo and a room.
struct VoiceSpec {
    double speed = 1;
    bool backwards = false, scratch = false, whisper = false, beep = false;
    float vibrato_hz = 0, vibrato_octaves = 0;
    std::array<float, 2> shift = {0, 0}; // semitones; 0 is off. Two make a chord with the dry voice.
    float dry = 1;                       // how much of the voice itself stays beside the shifted ones
    float ring_hz = 0, ring_mix = 0;
    float highpass_hz = 0, lowpass_hz = 0;
    float drive = 0;
    float crush_hz = 0, crush_levels = 0;
    bool comb = false;
    float echo_seconds = 0, echo_feedback = 0, echo_mix = 0;
    float room = 0;   // 0 none to 1 a cathedral
    float wobble = 0; // a slow random swell, bubbles under water
    float gain = 1;
};

inline VoiceSpec voice_spec(Voice voice) {
    VoiceSpec v;
    switch (voice) {
    case Voice::Chipmunk: v.speed = 1.8; break;
    case Voice::Robot:
        v.ring_hz = 50;
        v.ring_mix = .6f;
        v.comb = true;
        v.gain = 2.2f; // ring modulation takes half the level away
        break;
    case Voice::Monster:
        v.speed = .62;
        v.drive = 2.4f;
        v.lowpass_hz = 1800;
        v.gain = .6f;
        break;
    case Voice::Backwards: v.backwards = true; break;
    case Voice::Echo:
        v.echo_seconds = .25f;
        v.echo_feedback = .45f;
        v.echo_mix = .55f;
        break;
    case Voice::Alien:
        v.speed = 1.2;
        v.vibrato_hz = 9;
        v.vibrato_octaves = .08f;
        v.ring_hz = 330;
        v.ring_mix = .65f;
        v.echo_seconds = .09f;
        v.echo_feedback = .5f;
        v.echo_mix = .35f;
        v.gain = 2.2f;
        break;
    case Voice::Giant:
        v.shift = {-9, 0};
        v.dry = 0;
        v.lowpass_hz = 2600;
        v.drive = 1.2f;
        v.room = .55f;
        v.gain = 1.2f;
        break;
    case Voice::Ghost:
        v.speed = .85;
        v.vibrato_hz = .7f;
        v.vibrato_octaves = .12f;
        v.highpass_hz = 300;
        v.room = 1;
        v.echo_seconds = .33f;
        v.echo_feedback = .5f;
        v.echo_mix = .4f;
        break;
    case Voice::Underwater:
        v.vibrato_hz = 3;
        v.vibrato_octaves = .06f;
        v.lowpass_hz = 650;
        v.wobble = .7f;
        v.room = .3f;
        v.gain = 1.5f;
        break;
    case Voice::Choir:
        v.shift = {4, 7}; // a major chord on every word
        v.dry = .8f;
        v.vibrato_hz = 5;
        v.vibrato_octaves = .015f;
        v.room = .5f;
        v.gain = .75f;
        break;
    case Voice::EightBit:
        v.crush_hz = 5000;
        v.crush_levels = 12;
        v.drive = 1.5f;
        break;
    case Voice::Telephone:
        v.highpass_hz = 450;
        v.lowpass_hz = 3000;
        v.drive = 1.6f;
        v.gain = 1.3f;
        break;
    case Voice::Megaphone:
        v.highpass_hz = 650;
        v.lowpass_hz = 4200;
        v.drive = 5;
        v.echo_seconds = .12f;
        v.echo_feedback = .25f;
        v.echo_mix = .3f;
        v.gain = .9f;
        break;
    case Voice::Cave:
        v.room = 1;
        v.echo_seconds = .4f;
        v.echo_feedback = .35f;
        v.echo_mix = .35f;
        break;
    case Voice::Jelly:
        v.vibrato_hz = 6;
        v.vibrato_octaves = .3f;
        break;
    case Voice::Scratch: v.scratch = true; break;
    case Voice::Whisper:
        v.whisper = true;
        v.room = .25f;
        v.gain = 1.6f;
        break;
    case Voice::Sloth: // half speed, the pitch put back: slow but still you
        v.speed = .55;
        v.shift = {float(12 * std::log2(1 / .55)), 0};
        v.dry = 0;
        break;
    case Voice::Speedy: // twice as fast, the pitch put back
        v.speed = 2;
        v.shift = {-12, 0};
        v.dry = 0;
        break;
    case Voice::Astronaut:
        v.highpass_hz = 320;
        v.lowpass_hz = 3400;
        v.drive = 1.3f;
        v.beep = true;
        v.echo_seconds = .45f;
        v.echo_feedback = .3f;
        v.echo_mix = .3f;
        v.gain = 1.3f;
        break;
    case Voice::Dragon:
        v.shift = {-12, 0};
        v.dry = 0;
        v.ring_hz = 28;
        v.ring_mix = .35f;
        v.drive = 3;
        v.lowpass_hz = 2200;
        v.room = .4f;
        v.gain = 1.1f;
        break;
    case Voice::Baby:
        v.speed = 1.05;
        v.shift = {6, 0};
        v.dry = 0;
        v.vibrato_hz = 5;
        v.vibrato_octaves = .03f;
        break;
    case Voice::Opera:
        v.shift = {3, 0};
        v.dry = 0;
        v.vibrato_hz = 5.5f;
        v.vibrato_octaves = .05f;
        v.room = .7f;
        break;
    default: break;
    }
    return v;
}

// The drawn curve: octaves up (+) or down (-) at each column. Undrawn columns are 0, so a short squiggle only
// bends the part of the word under it.
using Bend = std::array<float, bend_points>;

inline bool bent(const Bend& bend) {
    return std::any_of(bend.begin(), bend.end(), [](float v) { return std::abs(v) > .001f; });
}

// The curve at a point of the recording (0 start, 1 end), between the columns.
inline float bend_at(const Bend& bend, double part) {
    const double at = std::clamp(part, 0.0, 1.0) * (bend_points - 1);
    const int low = std::min(int(at), bend_points - 2);
    const float fraction = float(at - low);
    return bend[size_t(low)] + (bend[size_t(low + 1)] - bend[size_t(low)]) * fraction;
}

// A stroke from one column to another sets every column between, so a quick flick leaves no gaps.
inline void paint(Bend& bend, int from, float from_value, int to, float to_value) {
    from = std::clamp(from, 0, bend_points - 1);
    to = std::clamp(to, 0, bend_points - 1);
    from_value = std::clamp(from_value, -bend_octaves, bend_octaves);
    to_value = std::clamp(to_value, -bend_octaves, bend_octaves);
    if (from == to) {
        bend[size_t(to)] = to_value;
        return;
    }
    const int step = to > from ? 1 : -1;
    for (int c = from;; c += step) {
        const float t = float(c - from) / float(to - from);
        bend[size_t(c)] = from_value + (to_value - from_value) * t;
        if (c == to) break;
    }
}

// How fast the recording is read for a voice: above 1 is higher and quicker, below lower and slower.
inline double voice_speed(Voice voice) { return voice_spec(voice).speed; }

// A fresh recording made ready to play. On the handheld the microphone sits next to the buttons, so a take starts and
// ends with the click of X: those ends are dropped. Then the DC offset and rumble go, the quiet around the word is
// trimmed against the room's own noise (not against the loudest point, which a stray click would win), and the voice
// is brought to the same level every time, a soft limit catching anything sharper. Empty when nothing was said.
inline std::vector<float> prepare(const std::vector<int16_t>& pcm) {
    const size_t head = rate * 3 / 20, tail_cut = rate / 5; // 150 ms after the press, 200 ms before the release
    if (pcm.size() < min_samples + head + tail_cut) return {};
    const std::vector<int16_t> take(pcm.begin() + long(head), pcm.end() - long(tail_cut));
    double mean = 0;
    for (int16_t v : take) mean += v;
    mean /= double(take.size());
    std::vector<float> out(take.size());
    // An 80 Hz high-pass after the offset: a handheld's microphone hears its own handling and the room's rumble.
    const float keep = 1.f / (1.f + 2 * 3.14159265f * 80.f / rate);
    float previous_in = 0, previous_out = 0;
    for (size_t i = 0; i < take.size(); i++) {
        const float in = float((take[i] - mean) / 32768.0);
        out[i] = previous_out = keep * (previous_out + in - previous_in);
        previous_in = in;
    }
    // Loudness in 10 ms windows; the room is the quietest tenth of them.
    const size_t window = rate / 100, count = out.size() / window;
    std::vector<float> loudness(count);
    for (size_t w = 0; w < count; w++) {
        double sum = 0;
        for (size_t i = w * window; i < (w + 1) * window; i++) sum += double(out[i]) * out[i];
        loudness[w] = float(std::sqrt(sum / window));
    }
    std::vector<float> sorted = loudness;
    std::sort(sorted.begin(), sorted.end());
    const float room = sorted.empty() ? 0 : sorted[sorted.size() / 10], loudest = sorted.empty() ? 0 : sorted.back();
    // Speech: well above the room (4x, 12 dB) but never more than a quarter of the loudest (a take that is nearly all
    // voice has no quiet tenth), at least -50 dBFS, and lasting: a lone 10 ms window is a click.
    const float speech = std::max({std::min(room * 4, loudest / 4), .003f, loudest / 40});
    size_t first = count, last = 0;
    for (size_t w = 0; w < count; w++) {
        const bool loud = loudness[w] >= speech, held = w + 1 < count && loudness[w + 1] >= speech;
        if (loud && held) {
            first = std::min(first, w);
            last = std::max(last, std::min(count, w + 2));
        }
    }
    if (first >= last || loudest < .004f) return {};
    const size_t margin = rate / 12; // 80 ms of air either side
    const size_t from = first * window > margin ? first * window - margin : 0,
                 to = std::min(out.size(), last * window + margin);
    out = std::vector<float>(out.begin() + long(from), out.begin() + long(to));
    if (out.size() < min_samples) return {};
    // The level from the voice's typical peak (the 99th percentile), so one click cannot set it; at most +36 dB.
    std::vector<float> magnitude(out.size());
    for (size_t i = 0; i < out.size(); i++) magnitude[i] = std::abs(out[i]);
    const size_t at = magnitude.size() * 99 / 100;
    std::nth_element(magnitude.begin(), magnitude.begin() + long(at), magnitude.end());
    const float gain = std::min(64.f, .6f / std::max(magnitude[at], .0001f));
    const size_t fade = rate / 200; // 5 ms, so the cut never clicks
    for (size_t i = 0; i < out.size(); i++) {
        float shape = 1;
        if (i < fade) shape = float(i) / fade;
        if (out.size() - i <= fade) shape = std::min(shape, float(out.size() - i - 1) / fade);
        out[i] = std::tanh(out[i] * gain * 1.2f) / 1.2f * shape; // soft: anything past ~0.6 rounds off below 0.84
    }
    return out;
}

namespace detail {
// A state-variable filter: low, band and high pass from one structure, stable at any setting below a sixth of the rate.
struct Svf {
    float low = 0, band = 0, f = 0, q = 1;
    void set(float hz, int out_rate, float damping = 1.f) {
        f = 2 * std::sin(3.14159265f * std::min(hz, out_rate / 6.f) / out_rate);
        q = damping;
    }
    float run(float in) {
        low += f * band;
        const float high = in - low - q * band;
        band += f * high;
        return high;
    }
};

// A delay-line pitch shifter: two read heads sweep through a 40 ms window half a turn apart, each faded in and out
// so their seams never click. Moves the pitch without changing the speed.
struct Shifter {
    std::vector<float> line;
    size_t at = 0;
    double phase = 0, step = 0;
    double window = 1;
    void start(float semitones, int out_rate) {
        window = out_rate * .04;
        line.assign(size_t(window) + 8, 0.f);
        at = 0;
        phase = 0;
        step = (1 - std::exp2(semitones / 12.0)) / window;
    }
    float run(float in) {
        line[at] = in;
        phase += step;
        phase -= std::floor(phase);
        float out = 0;
        for (double tap : {phase, phase + .5 >= 1 ? phase - .5 : phase + .5}) {
            const double delay = 2 + tap * window;
            double from = double(at) - delay;
            while (from < 0) from += double(line.size());
            const size_t i = size_t(from) % line.size(), j = (i + 1) % line.size();
            const float frac = float(from - std::floor(from));
            const float s = float(std::sin(3.14159265358979 * tap));
            out += (line[i] + (line[j] - line[i]) * frac) * s * s;
        }
        at = (at + 1) % line.size();
        return out;
    }
};

// A small Schroeder room: four damped combs into two all-passes.
struct Room {
    std::array<std::vector<float>, 4> combs;
    std::array<std::vector<float>, 2> passes;
    std::array<size_t, 4> comb_at{};
    std::array<size_t, 2> pass_at{};
    std::array<float, 4> damp{};
    float feedback = .8f;
    void start(float size, int out_rate) {
        static constexpr std::array<int, 4> comb_ms = {29, 31, 34, 37};
        static constexpr std::array<int, 2> pass_ms = {5, 2};
        for (size_t i = 0; i < combs.size(); i++) combs[i].assign(size_t(out_rate * comb_ms[i] / 1000), 0.f);
        for (size_t i = 0; i < passes.size(); i++) passes[i].assign(size_t(out_rate * pass_ms[i] / 1000), 0.f);
        comb_at.fill(0);
        pass_at.fill(0);
        damp.fill(0);
        feedback = .7f + .17f * std::clamp(size, 0.f, 1.f);
    }
    float run(float in) {
        float out = 0;
        for (size_t i = 0; i < combs.size(); i++) {
            float& slot = combs[i][comb_at[i]];
            damp[i] += (slot - damp[i]) * .35f;
            out += slot;
            slot = in + damp[i] * feedback;
            comb_at[i] = (comb_at[i] + 1) % combs[i].size();
        }
        out *= .25f;
        for (size_t i = 0; i < passes.size(); i++) {
            float& slot = passes[i][pass_at[i]];
            const float delayed = slot;
            slot = out + delayed * .5f;
            out = delayed - out * .5f;
            pass_at[i] = (pass_at[i] + 1) % passes[i].size();
        }
        return out;
    }
};
} // namespace detail

// Plays one recording through one voice, a block at a time, at the output's rate. `start` (UI thread, under the
// audio lock) sets it up and sizes its buffers; `render` (the audio callback) only reads and writes what is there.
class Player {
    const std::vector<float>* source = nullptr;
    Voice voice = Voice::Normal;
    VoiceSpec spec;
    int out_rate = 48000;
    double position = 0; // in source samples
    double ring_phase = 0, vibrato_phase = 0, beep_phase = 0;
    detail::Svf highpass, lowpass;
    std::array<detail::Svf, 4> analysis, synthesis; // the whisper's four bands
    std::array<float, 4> envelope{};
    std::array<detail::Shifter, 2> shifters;
    detail::Room room;
    std::vector<float> comb, delay;
    size_t comb_at = 0, delay_at = 0, tail = 0, tail_length = 0, beep_left = 0;
    float held = 0, hold_count = 0, wobble = 0, wobble_target = 0;
    // Scratch: the word is cut into short pieces, some played twice or three times, some half speed.
    double chunk_start = 0, chunk_end = 0, chunk_speed = 1;
    int repeats = 0;
    size_t fade_in = 0;
    uint32_t random = 0x2545F491;
    bool reading = false;
    float level_value = 0;
    float next_random() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return float(random & 0xffffff) / float(0xffffff);
    }
    void next_chunk(double size) {
        if (repeats > 0) {
            repeats--;
            position = chunk_start;
            fade_in = size_t(out_rate / 400);
            return;
        }
        chunk_start = position;
        chunk_end = std::min(size, position + rate * (.07 + .08 * next_random()));
        const float roll = next_random();
        repeats = roll < .35f ? 1 + int(next_random() * 2.99f) : 0;
        chunk_speed = roll > .85f ? .5 : roll > .75f ? 1.6 : 1;
    }

public:
    bool active() const { return source && (reading || tail || beep_left); }
    // How far through the recording playback is (0 to 1), for the playhead; backwards runs from 1 to 0.
    double progress() const {
        if (!source || source->empty()) return 0;
        const double part = std::clamp(position / double(source->size()), 0.0, 1.0);
        return spec.backwards ? 1 - part : part;
    }
    // About how loud the last block was (0 to 1), for the talking face.
    float level() const { return level_value; }
    Voice current() const { return voice; }
    void stop() {
        source = nullptr;
        reading = false;
        tail = 0;
        level_value = 0;
    }
    void start(const std::vector<float>& pcm, Voice chosen, int output_rate) {
        source = &pcm;
        voice = chosen;
        spec = voice_spec(chosen);
        out_rate = std::max(8000, output_rate);
        position = ring_phase = vibrato_phase = beep_phase = 0;
        held = hold_count = wobble = wobble_target = 0;
        reading = pcm.size() >= 2;
        level_value = 0;
        random = 0x2545F491;
        repeats = 0;
        chunk_start = chunk_end = 0;
        chunk_speed = 1;
        fade_in = 0;
        highpass = {};
        lowpass = {};
        if (spec.highpass_hz > 0) highpass.set(spec.highpass_hz, out_rate, 1.2f);
        if (spec.lowpass_hz > 0) lowpass.set(spec.lowpass_hz, out_rate, 1.2f);
        static constexpr std::array<float, 4> bands = {400, 1000, 2200, 4500};
        for (size_t i = 0; i < bands.size(); i++) {
            analysis[i] = {};
            synthesis[i] = {};
            analysis[i].set(bands[i], out_rate, .5f);
            synthesis[i].set(bands[i], out_rate, .5f);
        }
        envelope.fill(0);
        for (size_t i = 0; i < shifters.size(); i++)
            if (spec.shift[i] != 0) shifters[i].start(spec.shift[i], out_rate);
            else shifters[i].line.clear();
        if (spec.room > 0) room.start(spec.room, out_rate);
        // A 110 Hz comb gives the robot its metal.
        comb.assign(spec.comb ? size_t(out_rate / 110) : 1, 0.f);
        comb_at = 0;
        delay.assign(spec.echo_seconds > 0 ? size_t(out_rate * spec.echo_seconds) : 1, 0.f);
        delay_at = 0;
        tail_length = size_t(out_rate) * size_t(spec.echo_seconds > 0 || spec.room > 0 ? 2 : 0) +
                      (spec.comb ? size_t(out_rate / 20) : 0) + (spec.shift[0] != 0 ? size_t(out_rate / 20) : 0);
        beep_left = 0;
        tail = 0;
    }
    // Adds (mixes) `frames` mono samples into `out`, scaled by `gain`. False once the sound has finished.
    bool render(float* out, size_t frames, const Bend& bend, float gain, size_t stride = 1, size_t channels = 1) {
        if (!active()) return false;
        const std::vector<float>& pcm = *source;
        const double base = spec.speed * double(rate) / out_rate;
        const double size = double(pcm.size());
        const double two_pi = 2 * 3.14159265358979;
        double energy = 0;
        for (size_t i = 0; i < frames; i++) {
            float sample = 0;
            if (reading) {
                if (spec.scratch && position >= chunk_end) next_chunk(size);
                if (position >= size - 1) {
                    reading = false;
                    tail = tail_length;
                    if (spec.beep) beep_left = size_t(out_rate) * 3 / 20;
                } else {
                    const double at = spec.backwards ? size - 1 - position : position;
                    const size_t index = std::min(size_t(at), pcm.size() - 2);
                    const float fraction = float(at - double(index));
                    sample = pcm[index] + (pcm[index + 1] - pcm[index]) * fraction;
                    if (fade_in) sample *= 1.f - float(fade_in--) / float(out_rate / 400 + 1);
                    const double part = position / size;
                    double octaves = bend_at(bend, spec.backwards ? 1 - part : part);
                    if (spec.vibrato_hz > 0) {
                        octaves += spec.vibrato_octaves * std::sin(vibrato_phase);
                        vibrato_phase += two_pi * spec.vibrato_hz / out_rate;
                        if (vibrato_phase > two_pi) vibrato_phase -= two_pi;
                    }
                    position += base * (spec.scratch ? chunk_speed : 1) * std::exp2(octaves);
                }
            } else if (tail) tail--;
            else if (!beep_left) break;
            // The radio's "over" beep after the last word.
            if (beep_left) {
                beep_left--;
                beep_phase += two_pi * 1250 / out_rate;
                sample += .35f * (std::sin(beep_phase) > 0 ? 1.f : -1.f) * std::min(1.f, beep_left / 200.f);
            }
            if (spec.whisper) {
                // Four bands of the voice tell four bands of noise how loud to be: a whisper keeps the words.
                float noise_out = 0;
                for (size_t b = 0; b < analysis.size(); b++) {
                    analysis[b].run(sample);
                    const float strength = std::abs(analysis[b].band);
                    envelope[b] += (strength - envelope[b]) * (strength > envelope[b] ? .02f : .002f);
                    synthesis[b].run(next_random() * 2 - 1);
                    noise_out += synthesis[b].band * envelope[b] * 3;
                }
                sample = noise_out;
            }
            if (!shifters[0].line.empty()) {
                float shifted = shifters[0].run(sample);
                if (!shifters[1].line.empty()) shifted += shifters[1].run(sample);
                sample = sample * spec.dry + shifted;
            }
            if (spec.ring_mix > 0) {
                const float ring = float(std::sin(ring_phase));
                ring_phase += two_pi * spec.ring_hz / out_rate;
                if (ring_phase > two_pi) ring_phase -= two_pi;
                sample = sample * (1 - spec.ring_mix) + sample * ring * spec.ring_mix;
            }
            if (spec.highpass_hz > 0) sample = highpass.run(sample);
            if (spec.drive > 0) sample = std::tanh(sample * spec.drive) / std::tanh(spec.drive);
            if (spec.lowpass_hz > 0) {
                lowpass.run(sample);
                sample = lowpass.low;
            }
            if (spec.crush_hz > 0) {
                hold_count += spec.crush_hz / out_rate;
                if (hold_count >= 1) {
                    hold_count -= 1;
                    held = std::round(sample * spec.crush_levels) / spec.crush_levels;
                }
                sample = held;
            }
            if (spec.wobble > 0) {
                if (next_random() < 8.f / out_rate) wobble_target = next_random();
                wobble += (wobble_target - wobble) * (20.f / out_rate);
                sample *= 1 - spec.wobble * .6f * wobble;
            }
            if (spec.comb) {
                const float mixed = sample + comb[comb_at] * .55f;
                comb[comb_at] = mixed;
                comb_at = (comb_at + 1) % comb.size();
                sample = mixed * .55f;
            }
            if (spec.echo_seconds > 0) {
                const float repeat = delay[delay_at];
                delay[delay_at] = sample + repeat * spec.echo_feedback;
                delay_at = (delay_at + 1) % delay.size();
                sample = sample * .8f + repeat * spec.echo_mix;
            }
            if (spec.room > 0) sample += room.run(sample) * spec.room * .9f;
            // A soft limit instead of a hard clip: loud voices round off rather than crackle.
            sample = std::tanh(sample * spec.gain * gain * 1.1f) * .92f;
            energy += double(sample) * sample;
            for (size_t c = 0; c < channels; c++) out[i * stride + c] += sample;
        }
        level_value = frames ? float(std::min(1.0, std::sqrt(energy / double(frames)) * 3)) : 0;
        if (!reading && !tail && !beep_left) source = nullptr;
        return active();
    }
};

// The whole sound through one voice, for the startup sound and the tests.
inline std::vector<float> render_all(const std::vector<float>& pcm, Voice voice, const Bend& bend, int out_rate) {
    std::vector<float> out;
    Player player;
    player.start(pcm, voice, out_rate);
    std::array<float, 512> block{};
    // Never more than a minute, however the curve was drawn (two octaves down at the monster's speed is ~6x).
    const size_t limit = size_t(out_rate) * 60;
    while (player.active() && out.size() < limit) {
        block.fill(0);
        player.render(block.data(), block.size(), bend, 1);
        out.insert(out.end(), block.begin(), block.end());
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// Fewer columns from more (the loudest of each group), for the small drawings.
inline std::vector<float> fewer(const std::vector<float>& columns, int count) {
    std::vector<float> out(size_t(std::max(0, count)), 0.f);
    if (columns.empty()) return out;
    for (size_t i = 0; i < columns.size(); i++) {
        float& to = out[std::min(out.size() - 1, i * out.size() / columns.size())];
        to = std::max(to, columns[i]);
    }
    return out;
}

// One column of the waveform drawing: the loudest point in each of `columns` slices of the sound (0 to 1).
inline std::vector<float> peaks(const std::vector<float>& pcm, int columns) {
    std::vector<float> out(size_t(std::max(0, columns)), 0.f);
    if (pcm.empty() || columns <= 0) return out;
    for (int c = 0; c < columns; c++) {
        const size_t from = pcm.size() * size_t(c) / size_t(columns),
                     to = std::max(from + 1, pcm.size() * size_t(c + 1) / size_t(columns));
        float top = 0;
        for (size_t i = from; i < to && i < pcm.size(); i++) top = std::max(top, std::abs(pcm[i]));
        out[size_t(c)] = std::min(1.f, top);
    }
    return out;
}

// ---- files: mono PCM16 WAV, which is what the startup player and any computer can read

inline bool write_wav(const std::filesystem::path& path, const std::vector<float>& pcm, int sample_rate) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        const uint32_t bytes = uint32_t(pcm.size() * 2);
        auto u32 = [&](uint32_t v) {
            const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
            file.write(b, 4);
        };
        auto u16 = [&](uint16_t v) {
            const char b[2] = {char(v), char(v >> 8)};
            file.write(b, 2);
        };
        file.write("RIFF", 4);
        u32(36 + bytes);
        file.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(1);
        u32(uint32_t(sample_rate));
        u32(uint32_t(sample_rate) * 2);
        u16(2);
        u16(16);
        file.write("data", 4);
        u32(bytes);
        std::vector<char> data(pcm.size() * 2);
        for (size_t i = 0; i < pcm.size(); i++) {
            const auto v = uint16_t(int16_t(std::lround(std::clamp(pcm[i], -1.f, 1.f) * 32767.f)));
            data[i * 2] = char(v);
            data[i * 2 + 1] = char(v >> 8);
        }
        file.write(data.data(), std::streamsize(data.size()));
        if (!file) return false;
    }
    std::filesystem::rename(temporary, path, ec);
    return !ec;
}

// Reads a mono PCM16 WAV (any rate up to 48 kHz, at most a minute). Empty on anything else.
inline std::vector<float> read_wav(const std::filesystem::path& path, int* sample_rate = nullptr) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t at) {
        return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8 |
               uint32_t(uint8_t(bytes[at + 2])) << 16 | uint32_t(uint8_t(bytes[at + 3])) << 24;
    };
    auto u16 = [&](size_t at) { return uint16_t(uint8_t(bytes[at]) | uint8_t(bytes[at + 1]) << 8); };
    if (bytes.size() < 44 || std::string(bytes.data(), 4) != "RIFF" || std::string(bytes.data() + 8, 4) != "WAVE")
        return {};
    int found_rate = 0;
    bool pcm16_mono = false;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const std::string id(bytes.data() + at, 4);
        const uint32_t size = u32(at + 4);
        const size_t body = at + 8;
        if (body + size > bytes.size()) break;
        if (id == "fmt " && size >= 16) {
            pcm16_mono = u16(body) == 1 && u16(body + 2) == 1 && u16(body + 14) == 16;
            found_rate = int(u32(body + 4));
        } else if (id == "data") {
            if (!pcm16_mono || found_rate < 8000 || found_rate > 48000 || size > uint32_t(found_rate) * 120) return {};
            std::vector<float> out(size / 2);
            for (size_t i = 0; i < out.size(); i++) out[i] = float(int16_t(u16(body + i * 2))) / 32768.f;
            if (sample_rate) *sample_rate = found_rate;
            return out;
        }
        at = body + size + (size & 1);
    }
    return {};
}

// A slot's voice and curve, beside its recording: "voice=2" and "bend=" with 64 numbers.
struct SlotStyle {
    Voice voice = Voice::Normal;
    Bend bend{};
};

inline std::string style_text(const SlotStyle& style) {
    std::string text = "voice=" + std::to_string(int(style.voice)) + "\nbend=";
    char number[16];
    for (int i = 0; i < bend_points; i++) {
        std::snprintf(number, sizeof number, "%s%.3f", i ? "," : "", double(style.bend[size_t(i)]));
        text += number;
    }
    return text + "\n";
}

inline SlotStyle parse_style(const std::string& text) {
    SlotStyle style;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.rfind("voice=", 0) == 0) {
            try {
                const int v = std::stoi(line.substr(6));
                if (v >= 0 && v < voice_count) style.voice = Voice(v);
            } catch (const std::exception&) {}
        } else if (line.rfind("bend=", 0) == 0) {
            size_t from = 5;
            for (int i = 0; i < bend_points && from <= line.size(); i++) {
                const size_t comma = std::min(line.find(',', from), line.size());
                try {
                    const float v = std::stof(line.substr(from, comma - from));
                    if (std::isfinite(v)) style.bend[size_t(i)] = std::clamp(v, -bend_octaves, bend_octaves);
                } catch (const std::exception&) {}
                from = comma + 1;
            }
        }
    }
    return style;
}

} // namespace voice_toy
