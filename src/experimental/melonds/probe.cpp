// Original synthetic homebrew: exercise the CPU, input, software displays and
// checkpoint replay without a downloaded game, Nintendo BIOS or stock files.
#include "NDS.h"
#include "Args.h"
#include "Savestate.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace melonDS;
static void require(bool okay, const char* why) {
    if (!okay) throw std::runtime_error(why);
}
#include "test_cartridge.hpp"
static u32 ram_word(const NDS& nds, size_t offset) {
    u32 value;
    std::memcpy(&value, nds.MainRAM + offset, 4);
    return value;
}
static u64 frame_hash(const void* frame) {
    u64 hash = 14695981039346656037ULL;
    const u8* data = static_cast<const u8*>(frame);
    for (size_t i = 0; i < 256 * 192 * 4; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}
static void frames(NDS& nds, unsigned count) {
    std::array<s16, 4096> audio;
    for (unsigned i = 0; i < count; ++i) {
        require(nds.RunFrame() == 263, "incomplete DS frame");
        nds.SPU.ReadOutput(audio.data(), audio.size() / 2);
    }
}
static void exercise(bool jit) {
    NDSArgs args;
    if (!jit) args.JIT.reset();
    auto nds = std::make_unique<NDS>(std::move(args));
    auto rom = cartridge();
    auto cart = NDSCart::ParseROM(rom.data(), rom.size());
    require(bool(cart), "synthetic cartridge rejected");
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->SetupDirectBoot("RGDS-Probe.nds");
    nds->Start();
    // Separate backdrop colors exercise both software framebuffers.
    nds->ARM9Write32(0x04000000, 0x10000);
    nds->ARM9Write32(0x04001000, 0x10000);
    nds->ARM9Write16(0x05000000, 0x001f);
    nds->ARM9Write16(0x05000400, 0x03e0);
    frames(*nds, 3);
    const u32 first = ram_word(*nds, 0x1000);
    require(first > 0, "ARM9 did not execute");
    require((ram_word(*nds, 0x1004) & 0x3ff) == 0x3ff, "released keys not sampled");
    nds->SetKeyMask(0xffe); // active-low A pressed
    nds->TouchScreen(128, 96);
    require((nds->ARM7Read16(0x04000136) & 0x40) == 0, "pen press not visible to ARM7");
    frames(*nds, 2);
    require(ram_word(*nds, 0x1000) > first, "ARM9 stopped advancing");
    require((ram_word(*nds, 0x1004) & 0x3ff) == 0x3fe, "pressed A not sampled");
    void* top = nullptr;
    void* bottom = nullptr;
    require(nds->GPU.GetFramebuffers(&top, &bottom) && top && bottom, "missing software screens");
    const u64 top_hash = frame_hash(top), bottom_hash = frame_hash(bottom);
    require(top_hash != bottom_hash, "both screen outputs unexpectedly identical");
    Savestate checkpoint;
    require(nds->DoSavestate(&checkpoint) && !checkpoint.Error, "checkpoint save failed");
    checkpoint.Finish();
    require(!checkpoint.Error && checkpoint.Length() > 0, "checkpoint finalization failed");
    const u32 saved = ram_word(*nds, 0x1000);
    nds->ReleaseScreen();
    nds->SetKeyMask(0xfff);
    frames(*nds, 4);
    const u32 advanced = ram_word(*nds, 0x1000);
    require(advanced > saved, "no execution after checkpoint");
    // Snapshot into a fresh byte buffer, then reload and repeat the same input.
    std::vector<u8> bytes(static_cast<const u8*>(checkpoint.Buffer()),
                          static_cast<const u8*>(checkpoint.Buffer()) + checkpoint.Length());
    Savestate restore(bytes.data(), bytes.size(), false);
    require(!restore.Error && nds->DoSavestate(&restore) && !restore.Error, "checkpoint restore failed");
    require(ram_word(*nds, 0x1000) == saved, "RAM not restored");
    // melonDS intentionally leaves live input outside the checkpoint. The
    // frontend must poll/reapply current input after loading, not retain a
    // remembered press from the old checkpoint.
    nds->TouchScreen(128, 96);
    require((nds->ARM7Read16(0x04000136) & 0x40) == 0, "pen cannot be reapplied after restore");
    nds->ReleaseScreen();
    require((nds->ARM7Read16(0x04000136) & 0x40) != 0, "pen release not visible to ARM7");
    nds->SetKeyMask(0xfff);
    frames(*nds, 4);
    require(ram_word(*nds, 0x1000) == advanced, "checkpoint replay diverged");
    require((ram_word(*nds, 0x1004) & 0x3ff) == 0x3ff, "key release not sampled after restore");
    if (jit) require(!nds->JIT.JitBlocks9.empty() && !nds->JIT.JitBlocks7.empty(), "JIT did not compile both CPUs");
    std::printf("{\"mode\":\"%s\",\"frames\":13,\"counter\":%u,\"checkpoint_bytes\":%u,"
                "\"top_hash\":\"%016llx\",\"bottom_hash\":\"%016llx\",\"passed\":true}\n",
                jit ? "arm64-jit" : "interpreter", advanced, checkpoint.Length(),
                static_cast<unsigned long long>(top_hash), static_cast<unsigned long long>(bottom_hash));
    nds->Stop();
}
int main() {
    try {
        exercise(false);
        exercise(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Probe failed: %s\n", error.what());
        return 1;
    }
}
