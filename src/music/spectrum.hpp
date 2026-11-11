#pragma once
// What the visualisers are fed: the engine hands `Analyser::run` the last stretch of what the speakers are playing
// and gets back sixty-four bands, the waveform, the levels and a beat. Pure arithmetic, no allocation after the
// constructor.
#include "music/shared.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace rgds::music {
class Analyser {
public:
    static constexpr int size = 2048; // 43 ms at 48 kHz: 23 Hz a bin, enough to tell a kick from a bass line

private:
    std::array<float, size> window;
    std::array<std::complex<float>, size> bins;
    std::array<uint16_t, size> reversed;
    std::array<std::complex<float>, size / 2> twiddle;
    std::array<float, viz_bands> smooth{};
    float rate = 48000, bass_average = 0, beat = 0;
    std::array<float, viz_bands + 1> edges{}; // in bins

    void transform() {
        for (int i = 0; i < size; i++)
            if (reversed[size_t(i)] > i) std::swap(bins[size_t(i)], bins[reversed[size_t(i)]]);
        for (int span = 2; span <= size; span <<= 1) {
            const int half = span / 2, stride = size / span;
            for (int start = 0; start < size; start += span)
                for (int k = 0; k < half; k++) {
                    const auto t = twiddle[size_t(k * stride)] * bins[size_t(start + k + half)];
                    bins[size_t(start + k + half)] = bins[size_t(start + k)] - t;
                    bins[size_t(start + k)] += t;
                }
        }
    }

public:
    explicit Analyser(float sample_rate = 48000) {
        int bits = 0;
        while ((1 << bits) < size) bits++;
        for (int i = 0; i < size; i++) {
            window[size_t(i)] = .5f - .5f * std::cos(2 * 3.14159265f * float(i) / float(size - 1));
            int r = 0;
            for (int b = 0; b < bits; b++)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reversed[size_t(i)] = uint16_t(r);
        }
        for (int k = 0; k < size / 2; k++)
            twiddle[size_t(k)] = std::polar(1.f, -2 * 3.14159265f * float(k) / float(size));
        configure(sample_rate);
    }
    void configure(float sample_rate) {
        rate = sample_rate;
        // 40 Hz to 16 kHz in equal steps of pitch.
        for (int i = 0; i <= viz_bands; i++)
            edges[size_t(i)] = 40 * std::pow(400.f, float(i) / float(viz_bands)) * float(size) / rate;
    }
    // `stereo` is `size` interleaved frames, the newest last. `seconds` is the time since the last call.
    void run(const float* stereo, float seconds, Viz& out) {
        float peak[2] = {}, square[2] = {};
        for (int i = 0; i < size; i++) {
            const float l = stereo[i * 2], r = stereo[i * 2 + 1];
            bins[size_t(i)] = (l + r) * .5f * window[size_t(i)];
            if (i >= size - viz_wave * 2) {
                peak[0] = std::max(peak[0], std::fabs(l));
                peak[1] = std::max(peak[1], std::fabs(r));
                square[0] += l * l;
                square[1] += r * r;
            }
        }
        // The waveform is the last 1024 frames, every second one: wide enough to show a bass note's shape.
        for (int i = 0; i < viz_wave; i++) {
            const int at = size - viz_wave * 2 + i * 2;
            out.wave[0][i] = std::clamp(stereo[at * 2], -1.f, 1.f);
            out.wave[1][i] = std::clamp(stereo[at * 2 + 1], -1.f, 1.f);
        }
        transform();
        float bass = 0;
        for (int b = 0; b < viz_bands; b++) {
            // A band narrower than a bin reads between its two neighbours; a wider one takes its loudest bin, which
            // is what the eye expects of a spectrum (an average would sink the treble).
            const float from = edges[size_t(b)], to = edges[size_t(b) + 1];
            float level = 0;
            if (to - from < 1) {
                const float centre = (from + to) / 2;
                const int low = std::clamp(int(centre), 1, size / 2 - 2);
                const float mix = std::clamp(centre - float(low), 0.f, 1.f);
                level = std::abs(bins[size_t(low)]) * (1 - mix) + std::abs(bins[size_t(low) + 1]) * mix;
            } else {
                for (int k = std::max(1, int(from)); k < std::min(size / 2, int(to) + 1); k++)
                    level = std::max(level, std::abs(bins[size_t(k)]));
            }
            // A full-scale sine reads size/4 through the window. Music slopes about 4.5 dB an octave down from the
            // bass, so the top is lifted to match what it sounds like; 60 dB of range fills the bar.
            const float octave = float(b) / float(viz_bands) * 8.64f;
            const float db = 20 * std::log10(std::max(1e-9f, level / (float(size) / 4))) + octave * 4.5f;
            const float value = std::clamp((db + 66) / 60, 0.f, 1.f);
            // Up at once, down at 2.4 bars a second: the classic analyser's fall.
            smooth[size_t(b)] = std::max(value, smooth[size_t(b)] - seconds * 2.4f);
            out.bands[b] = smooth[size_t(b)];
            if (b < 8) bass += value / 8; // 40 to 85 Hz: the kick
        }
        // A beat is the bass jumping over its own one-second average.
        if (bass > bass_average * 1.35f + .08f && bass > .35f && beat < .35f) beat = 1;
        else beat = std::max(0.f, beat - seconds * 5);
        bass_average += (bass - bass_average) * std::min(1.f, seconds * 1.2f);
        out.bass = bass;
        out.beat = beat;
        for (int c = 0; c < 2; c++) {
            out.peak[c] = std::min(1.f, peak[c]);
            out.rms[c] = std::min(1.f, std::sqrt(square[c] / float(viz_wave * 2)));
        }
        out.serial.fetch_add(1, std::memory_order_release);
    }
    void rest(Viz& out, float seconds) { // nothing is playing: the bars fall to the floor
        for (int b = 0; b < viz_bands; b++)
            out.bands[b] = smooth[size_t(b)] = std::max(0.f, smooth[size_t(b)] - seconds * 2.4f);
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < viz_wave; i++) out.wave[c][i] *= .8f;
            out.peak[c] = out.rms[c] = 0;
        }
        out.bass = 0;
        out.beat = beat = std::max(0.f, beat - seconds * 5);
        out.serial.fetch_add(1, std::memory_order_release);
    }
};
} // namespace rgds::music
