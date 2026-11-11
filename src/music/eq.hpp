#pragma once
// The equaliser: ten peaking filters an octave apart, a headroom gain in front of them and a soft limiter behind.
// No allocation and no locks: the engine runs it inside the audio callback, and the launcher uses `response` to draw
// the curve the listener is shaping.
#include "music/shared.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace rgds::music {
inline constexpr std::array<float, eq_bands> eq_centres = {31.25f, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
inline constexpr float eq_range = 12; // dB either way
inline constexpr float eq_q = 1.41f;  // an octave wide: neighbours overlap into a smooth curve

struct EqPreset {
    const char* name;
    std::array<float, eq_bands> gain;
};
// The first is always Flat. Shapes are the usual ones, kept gentle: nothing beyond 6 dB.
inline constexpr std::array<EqPreset, 12> eq_presets = {{
    {"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Bass boost", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}},
    {"Treble boost", {0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
    {"Rock", {4, 3, 2, 0, -1, -1, 1, 3, 4, 4}},
    {"Pop", {-1, 0, 2, 3, 4, 3, 1, 0, -1, -1}},
    {"Jazz", {3, 2, 1, 2, -1, -1, 0, 1, 2, 3}},
    {"Classical", {3, 2, 1, 0, 0, 0, 0, 1, 2, 3}},
    {"Electronic", {5, 4, 1, 0, -2, 1, 0, 1, 4, 5}},
    {"Hip-hop", {5, 4, 2, 3, -1, -1, 1, 0, 2, 3}},
    {"Acoustic", {3, 3, 2, 1, 1, 1, 2, 3, 2, 1}},
    {"Vocal", {-3, -2, -1, 1, 3, 4, 3, 2, 0, -1}},
    // Small drivers have nothing under 200 Hz to give; the push sits where they still answer, and the top is tamed.
    {"Small speakers", {-6, -3, 2, 4, 3, 0, 0, 1, 2, 0}},
}};

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    // A peaking filter (the Audio EQ Cookbook's): `gain` dB at `centre` Hz.
    static Biquad peaking(float centre, float gain, float q, float rate) {
        Biquad f;
        centre = std::min(centre, rate * .45f);
        const float a = std::pow(10.f, gain / 40), w = 2 * 3.14159265f * centre / rate, alpha = std::sin(w) / (2 * q),
                    a0 = 1 + alpha / a;
        f.b0 = (1 + alpha * a) / a0;
        f.b1 = -2 * std::cos(w) / a0;
        f.b2 = (1 - alpha * a) / a0;
        f.a1 = f.b1;
        f.a2 = (1 - alpha / a) / a0;
        return f;
    }
    // How loud a tone of `frequency` Hz comes out, as a factor.
    float magnitude(float frequency, float rate) const {
        const float w = 2 * 3.14159265f * frequency / rate, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w),
                    s2 = std::sin(2 * w);
        const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = 1 + a1 * c1 + a2 * c2,
                    di = -(a1 * s1 + a2 * s2);
        return std::sqrt((nr * nr + ni * ni) / std::max(1e-12f, dr * dr + di * di));
    }
};

// The whole curve in dB at one frequency.
inline float eq_response(const std::array<float, eq_bands>& gain, float frequency, float rate = 48000) {
    float db = 0;
    for (int i = 0; i < eq_bands; i++)
        if (gain[size_t(i)] != 0)
            db += 20 *
                  std::log10(
                      Biquad::peaking(eq_centres[size_t(i)], gain[size_t(i)], eq_q, rate).magnitude(frequency, rate));
    return db;
}

// What to take off in front so the curve's highest point lands at full scale: neighbouring bands add up, so this is
// measured on the curve, not read off the sliders.
inline float eq_headroom(const std::array<float, eq_bands>& gain, float rate = 48000) {
    float peak = 0;
    for (int step = 0; step <= 108; step++) // 20 Hz to 20 kHz, about eleven points an octave
        peak = std::max(peak, eq_response(gain, 20 * std::pow(2.f, float(step) / 10.8f), rate));
    return -peak;
}

class Equaliser {
    struct Stage {
        Biquad f;
        float z1[2] = {}, z2[2] = {};
    };
    std::array<Stage, eq_bands> stages;
    std::array<float, eq_bands> gains{};
    float rate = 48000, preamp = 1, preamp_goal = 1, envelope = 0, boost = 1, headroom = 1;
    int active = 0; // stages with a gain; the rest are skipped
    bool on = false;

public:
    void configure(float sample_rate) {
        rate = sample_rate;
        set(gains, on);
    }
    // Takes effect from the next block. Filter memories are kept, so a slider can move while music plays.
    void set(const std::array<float, eq_bands>& gain, bool enabled) {
        gains = gain;
        on = enabled;
        active = 0;
        for (int i = 0; i < eq_bands; i++) {
            const float g = std::clamp(gain[size_t(i)], -eq_range, eq_range);
            if (!enabled || g == 0) continue;
            stages[size_t(active)].f = Biquad::peaking(eq_centres[size_t(i)], g, eq_q, rate);
            active++;
        }
        headroom = enabled ? std::pow(10.f, eq_headroom(gains, rate) / 20) : 1;
        preamp_goal = headroom * boost;
    }
    // Make-up gain in front of everything, in dB: the handheld's small speakers need it, and the limiter behind
    // keeps what it pushes past full scale from clipping.
    void set_boost(float db) {
        boost = std::pow(10.f, std::clamp(db, 0.f, 12.f) / 20);
        preamp_goal = headroom * boost;
    }
    float headroom_db() const { return 20 * std::log10(headroom); }
    // Interleaved stereo, in place.
    void process(float* samples, size_t frames) {
        if (!active && preamp == 1 && preamp_goal == 1) return limit(samples, frames);
        for (size_t i = 0; i < frames; i++) {
            preamp += (preamp_goal - preamp) * .002f; // about 10 ms: a preset change never clicks
            for (int c = 0; c < 2; c++) {
                float x = samples[i * 2 + size_t(c)] * preamp;
                for (int k = 0; k < active; k++) {
                    Stage& s = stages[size_t(k)];
                    // Transposed direct form II.
                    const float y = s.f.b0 * x + s.z1[c];
                    s.z1[c] = s.f.b1 * x - s.f.a1 * y + s.z2[c];
                    s.z2[c] = s.f.b2 * x - s.f.a2 * y;
                    x = y;
                }
                samples[i * 2 + size_t(c)] = x;
            }
        }
        if (std::fabs(preamp - preamp_goal) < 1e-5f) preamp = preamp_goal;
        // Denormals in a filter's memory after silence cost a core on some CPUs.
        for (int k = 0; k < active; k++)
            for (int c = 0; c < 2; c++) {
                if (std::fabs(stages[size_t(k)].z1[c]) < 1e-20f) stages[size_t(k)].z1[c] = 0;
                if (std::fabs(stages[size_t(k)].z2[c]) < 1e-20f) stages[size_t(k)].z2[c] = 0;
            }
        limit(samples, frames);
    }

private:
    // A peak limiter with an instant attack and a 100 ms release, both channels together so the picture between
    // the speakers stays put. It idles at gain 1: the headroom gain already keeps a mastered track under full scale,
    // and this only catches what filter overshoot or an over-hot file still pushes past it.
    void limit(float* samples, size_t frames) {
        const float release = std::exp(-1 / (rate * (boost > 1 ? .18f : .1f))), ceiling = .98f;
        for (size_t i = 0; i < frames; i++) {
            const float peak = std::max(std::fabs(samples[i * 2]), std::fabs(samples[i * 2 + 1]));
            envelope = std::max(peak, envelope * release);
            if (envelope > ceiling) {
                const float gain = ceiling / envelope;
                samples[i * 2] *= gain;
                samples[i * 2 + 1] *= gain;
            }
        }
        if (envelope < 1e-20f) envelope = 0;
    }
};
} // namespace rgds::music
