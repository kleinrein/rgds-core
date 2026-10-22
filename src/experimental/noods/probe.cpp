// Original RGDS test harness, GPL-3.0-or-later (linked with NooDS).
// No downloaded games, Nintendo BIOS, firmware or desktop frontend are used.
#include "core.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
static uint64_t hash(const uint32_t* pixels, size_t count) {
    uint64_t value = 14695981039346656037ULL;
    for (size_t i = 0; i < count; ++i) {
        for (int shift = 0; shift < 32; shift += 8) {
            value ^= (pixels[i] >> shift) & 255;
            value *= 1099511628211ULL;
        }
    }
    return value;
}
static void original_rom(const std::string& path) {
    std::array<uint8_t, 8192> rom{};
    std::memcpy(rom.data(), "RGDS PROBE  ####00", 18);
    // ARM9 increments a RAM word and samples the active-low key register.
    const uint32_t arm9[] = {0xe59f0018, 0xe3a01000, 0xe2811001, 0xe5801000, 0xe59f200c,
                             0xe1d230b0, 0xe5803004, 0xeafffff9, 0x02001000, 0x04000130};
    auto word = [&](size_t offset, uint32_t value) { std::memcpy(rom.data() + offset, &value, 4); };
    std::memcpy(rom.data() + 0x200, arm9, sizeof(arm9));
    word(0x20, 0x200);
    word(0x24, 0x02000000);
    word(0x28, 0x02000000);
    word(0x2c, sizeof(arm9));
    word(0x30, 0x1000);
    word(0x34, 0x03800000);
    word(0x38, 0x03800000);
    word(0x3c, 4);
    word(0x1000, 0xeafffffe); // ARM7 loops without BIOS calls.
    word(0x80, rom.size());
    word(0x84, 0x200);
    FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "cannot create original test cartridge");
    bool okay = std::fwrite(rom.data(), 1, rom.size(), file) == rom.size();
    okay = std::fclose(file) == 0 && okay;
    require(okay, "cartridge write failed");
}
static void frames(Core& core, unsigned count, std::array<uint32_t, 256 * 192 * 2>& pixels) {
    for (unsigned frame = 0; frame < count; ++frame) {
        // runCore may also return when a CPU changes its halt state. A GPU
        // frame, rather than a frontend loop iteration, is the unit measured.
        unsigned steps = 0;
        do {
            core.runCore();
            require(++steps < 100000, "no completed frame");
        } while (!core.gpu.getFrame(pixels.data(), false));
    }
}
int main(int argc, char** argv) {
    try {
        require(argc >= 2 && argc <= 5, "usage: noods-probe PRIVATE_WORK_DIRECTORY [FRAMES [THREADS [ROM]]]");
        const std::string work = argv[1];
        const unsigned count = argc > 2 ? std::stoul(argv[2]) : 180;
        const int threads = argc > 3 ? std::stoi(argv[3]) : 1;
        require(count >= 1 && count <= 36000 && threads >= 0 && threads <= 4, "invalid frame/thread count");
        require(mkdir(work.c_str(), 0700) == 0, "work directory must be new (protects existing saves)");
        for (const char* dir : {"saves", "states", "cheats"})
            require(mkdir((work + "/" + dir).c_str(), 0700) == 0, "cannot create private save directories");
        Settings::basePath = work;
        Settings::savesFolder = Settings::statesFolder = Settings::cheatsFolder = 1;
        Settings::ndsBios9Path = Settings::ndsBios7Path = Settings::ndsFirmPath = "";
        Settings::gbaBiosPath = Settings::sdImagePath = "";
        Settings::directBoot = 1;
        Settings::dsiMode = 0;
        Settings::arm7Hle = 0;
        Settings::romInRam = 1;
        Settings::fpsLimiter = 0;
        Settings::frameskip = 0;
        Settings::threaded2D = threads != 0;
        Settings::threaded3D = threads;
        Settings::highRes3D = 0;
        Settings::screenGhost = 0;
        Settings::emulateAudio = 1;
        const bool synthetic = argc < 5;
        const std::string rom = synthetic ? work + "/RGDS-Probe.nds" : argv[4];
        if (synthetic) original_rom(rom);
        const auto start = std::chrono::steady_clock::now();
        auto core = std::unique_ptr<Core>(new Core(rom));
        core->saveStates.setPath(work + "/probe.state", false);
        if (synthetic) {
            core->memory.write<uint32_t>(0, 0x04000000, 0x10000);
            core->memory.write<uint32_t>(0, 0x04001000, 0x10000);
            core->memory.write<uint16_t>(0, 0x05000000, 0x001f);
            core->memory.write<uint16_t>(0, 0x05000400, 0x03e0);
        }
        std::array<uint32_t, 256 * 192 * 2> pixels{};
        frames(*core, 3, pixels);
        const double first_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const uint32_t first = core->memory.read<uint32_t>(0, 0x02001000);
        if (synthetic) {
            require(first != 0, "ARM9 did not execute");
            require((core->memory.read<uint32_t>(0, 0x02001004) & 0x3ff) == 0x3ff, "key release not sampled");
            core->input.pressKey(0);
            core->input.pressScreen();
            frames(*core, 2, pixels);
            require((core->memory.read<uint32_t>(0, 0x02001004) & 0x3ff) == 0x3fe, "A press not sampled");
            require((core->input.readExtKeyIn() & 0x40) == 0, "touch press missing");
            require(hash(pixels.data(), 256 * 192) != hash(pixels.data() + 256 * 192, 256 * 192),
                    "both screens identical");
        }
        require(core->saveStates.saveState(), "checkpoint save failed");
        require(core->saveStates.checkState() == STATE_SUCCESS, "checkpoint header rejected");
        const auto saved = core->memory.read<uint32_t>(0, 0x02001000);
        core->input.releaseKey(0);
        core->input.releaseScreen();
        frames(*core, 4, pixels);
        const auto advanced = core->memory.read<uint32_t>(0, 0x02001000);
        require(core->saveStates.loadState(), "checkpoint load failed");
        require(core->memory.read<uint32_t>(0, 0x02001000) == saved, "checkpoint RAM not restored");
        core->input.releaseKey(0);
        core->input.releaseScreen();
        frames(*core, 4, pixels);
        if (synthetic) {
            require(advanced > saved && core->memory.read<uint32_t>(0, 0x02001000) == advanced,
                    "deterministic checkpoint replay diverged");
            require((core->memory.read<uint32_t>(0, 0x02001004) & 0x3ff) == 0x3ff, "key release not restored");
        }
        const auto measured = std::chrono::steady_clock::now();
        frames(*core, count, pixels);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - measured).count();
        struct stat info{};
        require(stat((work + "/probe.state").c_str(), &info) == 0 && info.st_size > 8, "checkpoint missing or empty");
        std::printf("{\"core\":\"NooDS\",\"synthetic\":%s,\"passed\":true,\"threads_3d\":%d,"
                    "\"completed_frames\":%u,\"unlimited_seconds\":%.6f,\"unlimited_game_fps\":%.3f,"
                    "\"startup_to_three_frames_ms\":%.3f,\"checkpoint_bytes\":%lld,"
                    "\"top_hash\":\"%016llx\",\"bottom_hash\":\"%016llx\","
                    "\"audio_emulated\":true,\"audio_played\":false,\"physical_display\":false}\n",
                    synthetic ? "true" : "false", threads, count, seconds, count / seconds, first_ms,
                    static_cast<long long>(info.st_size),
                    static_cast<unsigned long long>(hash(pixels.data(), 256 * 192)),
                    static_cast<unsigned long long>(hash(pixels.data() + 256 * 192, 256 * 192)));
        return 0;
    } catch (const CoreError& error) {
        std::fprintf(stderr, "NooDS core rejected startup: %d\n", error);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NooDS probe failed: %s\n", error.what());
    }
    return 1;
}
