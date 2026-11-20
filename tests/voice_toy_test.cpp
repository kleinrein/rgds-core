// The sound toy's voices (src/launcher/voice_toy.hpp): what a recording becomes, how each voice changes it, the drawn
// curve, and the files, under the sanitizers (tests/test_voice_toy.py builds and runs this).
#include "launcher/voice_toy.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    failures++;
}

// A steady tone, `seconds` long between quiet on either side: a "word" the trimming can find.
std::vector<int16_t> word(double frequency, double seconds, double quiet = .3, int16_t offset = 300) {
    const int rate = voice_toy::rate;
    std::vector<int16_t> out(size_t((seconds + 2 * quiet) * rate), offset);
    for (size_t i = size_t(quiet * rate); i < size_t((quiet + seconds) * rate); i++)
        out[i] = int16_t(offset + 12000 * std::sin(2 * 3.14159265358979 * frequency * double(i) / rate));
    return out;
}

// The tone's frequency, by counting rising zero crossings.
double frequency(const std::vector<float>& pcm, int rate) {
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = 1; i < pcm.size(); i++)
        if (pcm[i - 1] < 0 && pcm[i] >= 0) {
            if (!crossings) first = i;
            last = i;
            crossings++;
        }
    return crossings > 1 ? double(crossings - 1) * rate / double(last - first) : 0;
}

double rms(const std::vector<float>& pcm, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to && i < pcm.size(); i++) sum += double(pcm[i]) * pcm[i];
    return to > from ? std::sqrt(sum / double(to - from)) : 0;
}
} // namespace

int main() {
    using voice_toy::Voice;
    const voice_toy::Bend flat{};

    // Preparing: the DC offset goes, the quiet around the word is trimmed to a little air, the level is set.
    const auto prepared = voice_toy::prepare(word(220, 1.0));
    check(!prepared.empty(), "a word is kept");
    check(prepared.size() > size_t(voice_toy::rate) && prepared.size() < size_t(voice_toy::rate * 1.25),
          "the quiet before and after is trimmed");
    float peak = 0;
    double mean = 0;
    for (float v : prepared) {
        peak = std::max(peak, std::abs(v));
        mean += v;
    }
    check(peak > .45f && peak < .6f, "the voice is brought to the same level");
    // As on the handheld: the click of X right at both ends of a take, a quiet voice between, a knock in the middle.
    {
        auto take = word(220, 1.0, .4, 0);
        for (auto& v : take) v = int16_t(v / 20); // about -36 dBFS
        for (size_t at : {size_t(voice_toy::rate / 20), take.size() - voice_toy::rate / 10})
            for (size_t i = at; i < at + 80; i++) take[i] = int16_t(i % 2 ? 30000 : -30000);
        for (size_t i = take.size() / 2; i < take.size() / 2 + 40; i++) take[i] = int16_t(i % 2 ? 28000 : -28000);
        const auto quiet = voice_toy::prepare(take);
        check(quiet.size() > size_t(voice_toy::rate) * 9 / 10 && quiet.size() < size_t(voice_toy::rate * 1.3),
              "the clicks at the ends are dropped and the quiet voice is kept");
        check(rms(quiet, 0, quiet.size()) > .2, "a quiet voice is brought up, not left under a click");
        float top = 0;
        for (float v : quiet) top = std::max(top, std::abs(v));
        check(top < .9f, "a knock in the middle is softened, not clipped");
    }
    check(std::abs(mean / prepared.size()) < .01, "the DC offset is taken out");
    check(std::abs(prepared.front()) < .001f && std::abs(prepared.back()) < .001f, "the ends are faded");
    check(voice_toy::prepare(std::vector<int16_t>(voice_toy::rate * 2, 250)).empty(), "silence is not a recording");
    check(voice_toy::prepare(word(220, .05, .02)).empty(), "a click is too short");
    check(voice_toy::prepare({}).empty(), "nothing is nothing");

    // Each voice at the output rate the handheld's sink runs at.
    const int out = 48000;
    const auto normal = voice_toy::render_all(prepared, Voice::Normal, flat, out);
    const double length = double(prepared.size()) / voice_toy::rate;
    check(std::abs(double(normal.size()) / out - length) < .02, "normal keeps the length");
    check(std::abs(frequency(normal, out) - 220) < 4, "normal keeps the pitch");

    const auto chipmunk = voice_toy::render_all(prepared, Voice::Chipmunk, flat, out);
    check(std::abs(frequency(chipmunk, out) - 220 * 1.8) < 8, "chipmunk is higher");
    check(double(chipmunk.size()) / out < length / 1.7, "chipmunk is quicker");

    const auto monster = voice_toy::render_all(prepared, Voice::Monster, flat, out);
    check(std::abs(frequency(monster, out) - 220 * .62) < 4, "monster is lower");
    check(double(monster.size()) / out > length * 1.5, "monster is slower");

    const auto echo = voice_toy::render_all(prepared, Voice::Echo, flat, out);
    check(double(echo.size()) / out > length + 1.0, "the echo rings on after the word");
    check(rms(echo, size_t((length + .1) * out), size_t((length + .35) * out)) > .02, "the echo repeats");

    const auto robot = voice_toy::render_all(prepared, Voice::Robot, flat, out);
    check(!robot.empty() && rms(robot, 0, robot.size()) > .05, "the robot speaks");
    check(rms(robot, 0, robot.size()) < rms(normal, 0, normal.size()) * 1.5, "the robot's metal does not blow up");

    // Every voice makes a sound, stays inside full scale, and ends.
    for (int v = 0; v < voice_toy::voice_count; v++) {
        const auto sound = voice_toy::render_all(prepared, Voice(v), flat, out);
        float top = 0;
        bool finite = true;
        for (float s : sound) {
            top = std::max(top, std::abs(s));
            finite = finite && std::isfinite(s);
        }
        if (!(finite && top <= 1.f && rms(sound, 0, sound.size()) > .01 && sound.size() < size_t(out) * 12))
            std::printf("voice %s: peak %.3f rms %.4f length %.2f s\n", voice_toy::voice_names[size_t(v)], double(top),
                        rms(sound, 0, sound.size()), double(sound.size()) / out);
        check(finite && top <= 1.f, "every voice stays finite and inside full scale");
        check(rms(sound, 0, sound.size()) > .01, "every voice is heard");
        check(sound.size() < size_t(out) * 12, "every voice ends");
    }
    // The pitch shifter: a pitch change that keeps the length, and a length change that keeps the pitch.
    const auto giant = voice_toy::render_all(prepared, Voice::Giant, flat, out);
    check(std::abs(frequency(giant, out) - 220 * std::exp2(-9 / 12.0)) < 8, "the giant is lower");
    const auto baby = voice_toy::render_all(prepared, Voice::Baby, flat, out);
    check(std::abs(frequency(baby, out) - 220 * 1.05 * std::exp2(6 / 12.0)) < 12, "the baby is higher");
    const auto sloth = voice_toy::render_all(prepared, Voice::Sloth, flat, out);
    check(double(sloth.size()) / out > length * 1.7, "the sloth is slow");
    check(std::abs(frequency(sloth, out) - 220) < 25, "the sloth keeps the pitch (within the shifter's seams)");
    const auto speedy = voice_toy::render_all(prepared, Voice::Speedy, flat, out);
    check(double(speedy.size()) / out < length * .6, "speedy is quick");
    check(std::abs(frequency(speedy, out) - 220) < 10, "speedy keeps the pitch");
    const auto scratch = voice_toy::render_all(prepared, Voice::Scratch, flat, out);
    check(scratch.size() > normal.size(), "the scratch plays pieces again");

    // Backwards: a word that rises plays falling.
    std::vector<int16_t> rising(size_t(voice_toy::rate) * 2, 0);
    double phase = 0;
    for (size_t i = size_t(voice_toy::rate) / 4; i < rising.size() - voice_toy::rate / 4; i++) {
        phase += (200 + 400 * double(i) / rising.size()) / voice_toy::rate;
        rising[i] = int16_t(12000 * std::sin(2 * 3.14159265358979 * phase));
    }
    const auto glide = voice_toy::prepare(rising);
    const auto backwards = voice_toy::render_all(glide, Voice::Backwards, flat, out);
    const size_t quarter = backwards.size() / 4;
    const std::vector<float> start(backwards.begin(), backwards.begin() + long(quarter)),
        end(backwards.end() - long(quarter), backwards.end());
    check(frequency(start, out) > frequency(end, out) + 100, "backwards plays the end first");

    // The curve: an octave up all the way doubles the pitch and halves the length; down halves it.
    voice_toy::Bend up{}, down{};
    up.fill(1);
    down.fill(-1);
    const auto higher = voice_toy::render_all(prepared, Voice::Normal, up, out);
    check(std::abs(frequency(higher, out) - 440) < 8, "an octave up doubles the pitch");
    check(std::abs(double(higher.size()) / out - length / 2) < .03, "an octave up halves the length");
    const auto lower = voice_toy::render_all(prepared, Voice::Normal, down, out);
    check(std::abs(frequency(lower, out) - 110) < 3, "an octave down halves the pitch");
    // Half drawn: only the second half is bent.
    voice_toy::Bend half{};
    voice_toy::paint(half, 32, 1, 63, 1);
    check(!voice_toy::bent(flat) && voice_toy::bent(half), "bent says whether a curve was drawn");
    check(std::abs(voice_toy::bend_at(half, .1)) < .001f && std::abs(voice_toy::bend_at(half, .9) - 1) < .001f,
          "the curve is read where the recording is");
    // A quick stroke leaves no gaps, either way round, and values are kept inside an octave.
    voice_toy::Bend stroke{};
    voice_toy::paint(stroke, 60, 3, 4, -3);
    check(stroke[60] == 1 && stroke[4] == -1 && std::abs(stroke[32]) < .1f, "a stroke fills every column between");

    // The player as the sink runs it: blocks into a stereo buffer, a progress for the playhead and a level.
    voice_toy::Player player;
    player.start(prepared, Voice::Normal, out);
    std::vector<float> stereo(1024 * 2, 0.f);
    player.render(stereo.data(), 1024, flat, 1, 2, 2);
    bool same = true;
    for (size_t i = 0; i < 1024; i++) same = same && stereo[i * 2] == stereo[i * 2 + 1];
    check(same, "both channels carry the voice");
    check(player.progress() > 0 && player.progress() < .05, "progress moves");
    for (int i = 0; i < 2000 && player.active(); i++) {
        std::fill(stereo.begin(), stereo.end(), 0.f);
        player.render(stereo.data(), 1024, flat, 1, 2, 2);
    }
    check(!player.active(), "the player finishes");
    check(!player.render(stereo.data(), 1024, flat, 1, 2, 2), "a finished player adds nothing");
    player.start(glide, Voice::Backwards, out);
    std::fill(stereo.begin(), stereo.end(), 0.f);
    player.render(stereo.data(), 1024, flat, 1, 2, 2);
    check(player.progress() > .95, "backwards runs its playhead from the end");
    player.stop();
    check(!player.active(), "stop stops");
    static const std::vector<float> tiny(1, .5f);
    player.start(tiny, Voice::Normal, out);
    check(!player.active(), "a single sample is nothing to play");

    // Files: a WAV written is read back the same, a slot's style survives its text.
    const auto folder = std::filesystem::temp_directory_path() / "voice_toy_test";
    std::filesystem::remove_all(folder);
    check(voice_toy::write_wav(folder / "slot-1.wav", prepared, voice_toy::rate), "the recording is written");
    int rate = 0;
    const auto read = voice_toy::read_wav(folder / "slot-1.wav", &rate);
    check(rate == voice_toy::rate && read.size() == prepared.size(), "the recording is read back");
    float worst = 0;
    for (size_t i = 0; i < read.size() && i < prepared.size(); i++)
        worst = std::max(worst, std::abs(read[i] - prepared[i]));
    check(worst < 1.f / 16000, "as it was, to 16 bits");
    check(voice_toy::read_wav(folder / "missing.wav").empty(), "a missing file is empty");
    { std::ofstream(folder / "junk.wav") << "RIFF....WAVEjunk"; }
    check(voice_toy::read_wav(folder / "junk.wav").empty(), "junk is empty");
    voice_toy::SlotStyle style;
    style.voice = Voice::Monster;
    voice_toy::paint(style.bend, 10, .5f, 20, -.25f);
    const auto back = voice_toy::parse_style(voice_toy::style_text(style));
    check(back.voice == Voice::Monster, "the voice is kept");
    bool kept = true;
    for (int i = 0; i < voice_toy::bend_points; i++) kept = kept && std::abs(back.bend[i] - style.bend[i]) < .001f;
    check(kept, "the curve is kept");
    const auto odd = voice_toy::parse_style("voice=99\nbend=nan,5,-5,x\n");
    check(odd.voice == Voice::Normal && odd.bend[0] == 0 && odd.bend[1] == 1 && odd.bend[2] == -1,
          "odd values are refused or clamped");
    std::filesystem::remove_all(folder);

    // Drawing columns.
    const auto columns = voice_toy::peaks(prepared, 96);
    check(columns.size() == 96 && *std::max_element(columns.begin(), columns.end()) > .45f, "the outline has its peaks");
    check(voice_toy::peaks({}, 10).size() == 10, "an empty outline keeps its columns");
    const auto small = voice_toy::fewer(columns, 18);
    check(small.size() == 18 &&
              *std::max_element(small.begin(), small.end()) == *std::max_element(columns.begin(), columns.end()),
          "the small outline keeps the loudest point");
    check(voice_toy::fewer({}, 4).size() == 4, "a small empty outline keeps its columns");

    if (failures) return 1;
    std::printf("voice toy: ok\n");
    return 0;
}
