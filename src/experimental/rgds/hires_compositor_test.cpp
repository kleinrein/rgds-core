// SPDX-License-Identifier: GPL-3.0-or-later
// Differential test: cached high-resolution composition vs native rendering.
#include "core/nds.h"
#include "core/gpu/kernels.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
using namespace ds;
static void w16(NDS& n, u32 a, u16 v) { n.bus.dma_write16(Cpu::ARM9, a, v); }
static void w32(NDS& n, u32 a, u32 v) { n.bus.dma_write32(Cpu::ARM9, a, v); }
static void render(NDS& n, const gpu::Pixel* samples) {
    auto& e = n.gpu.engine[0];
    e.set_3d_line(samples);
    e.apply_pending();
    for (int i = 0; i < 3; ++i) e.pre_draw(0, false);
    e.update_windows(0);
    e.render_sprites(0);
    e.render_line(0);
}
int main() {
    using ds::gpu::kern::rgds_filter4;
    // Coverage must not introduce black fringes, and hidden RGB must be ignored.
    assert(rgds_filter4(0x1F3F3F3F, 0, 0, 0) == 0x073F3F3F);
    assert(rgds_filter4(0x1F3F3F3F, 0x003F0000, 0x00003F00, 0x0000003F) == 0x073F3F3F);
    assert(rgds_filter4(0x1F00003F, 0x1F3F0000, 0x1F00003F, 0x1F3F0000) == 0x1F200020);
    assert(rgds_filter4(0x003F3F3F, 0, 0, 0) == 0);
    assert(rgds_filter4(0x01010203, 0x01010203, 0x01010203, 0x01010203) == 0x01010203);

    // An independent weighted resolve covers flat texels and mixed coverage.
    auto reference_filter = [](const std::array<u32, 4>& p) {
        u32 weight = 0, channel[3] = {};
        for (u32 pixel : p) {
            const u32 alpha = pixel >> 24, w = alpha ? alpha + 1 : 0;
            weight += w;
            for (u32 c = 0; c < 3; ++c) channel[c] += ((pixel >> (8 * c)) & 63) * w;
        }
        const u32 coverage = (weight + 2) / 4;
        if (coverage < 2) return 0u;
        u32 result = (coverage - 1) << 24;
        for (u32 c = 0; c < 3; ++c) result |= ((channel[c] + weight / 2) / weight) << (8 * c);
        return result;
    };
    u32 random = 12345;
    for (u32 i = 0; i < 262144; ++i) {
        std::array<u32, 4> pixels;
        for (auto& pixel : pixels) {
            random = random * 1664525u + 1013904223u;
            pixel = random & 0x1F3F3F3F;
        }
        if ((i & 3) == 0) pixels.fill(pixels[0]);
        if (rgds_filter4(pixels[0], pixels[1], pixels[2], pixels[3]) != reference_filter(pixels)) {
            std::fprintf(stderr, "FAIL: filtered coverage case %u\n", i);
            return 1;
        }
    }
    std::puts("hires filter: 262144 independent weighted-reference cases passed");

    setenv("DS_VERIFY_HIRES", "1", 1);
    NDS n;
    n.io.powcnt1 = 0x820F;
    n.gpu.set_powcnt(0x820F);
    w16(n, 0x04000240, 0x8281); // A: BG, B: OBJ
    w16(n, 0x05000000, 0x7C00);
    w16(n, 0x05000002, 0x001F);
    w16(n, 0x05000202, 0x7FFF);
    for (u32 a = 0; a < 2048; a += 2) w16(n, 0x06000000 + a, 1);
    for (u32 a = 0; a < 32; a += 4) {
        w32(n, 0x06004020 + a, 0x00110011);
        w32(n, 0x06400020 + a, 0x01111110);
    }
    for (u32 i = 2; i < 128; ++i) w16(n, 0x07000000 + i * 8, 0x0200);
    w16(n, 0x04000040, (64 << 8) | 192);
    w16(n, 0x04000044, 192);
    std::array<gpu::Pixel, 256> native, subpixel, got, expected, saved;
    for (u32 x = 0; x < 256; ++x) {
        native[x] = (x % 3 ? 31u << 24 : 0) | 0x001C220Cu;
        subpixel[x] = ((x % 32) << 24) | ((x % 64) << 16) | 0x1822;
    }
    unsigned cases = 0;
    for (u32 coverage = 0; coverage < 3; ++coverage) {
        for (u32 x = 0; x < 256; ++x)
            subpixel[x] = (subpixel[x] & 0xFFFFFF) | ((coverage == 0 ? x % 32 : coverage == 1 ? 31 : 0) << 24);
        for (u32 p3 = 0; p3 < 4; ++p3)
            for (u32 pb = 0; pb < 4; ++pb)
                for (u32 po = 0; po < 4; ++po)
                    for (u32 effect = 0; effect < 4; ++effect)
                        for (u32 window = 0; window < 4; ++window)
                            for (u32 targets = 0; targets < 4; ++targets) {
                                w32(n, 0x04000000, 0x00011318 | (window ? 1u << 13 : 0));
                                w16(n, 0x04000008, p3);
                                w16(n, 0x0400000A, 4 | pb);
                                w16(n, 0x07000000, 0);
                                w16(n, 0x07000002, 68);
                                w16(n, 0x07000004, 1 | po << 10);
                                w16(n, 0x07000008, 0x0400);
                                w16(n, 0x0700000A, 188);
                                w16(n, 0x0700000C, 1 | po << 10);
                                w16(n, 0x04000048, window == 2 ? 0x3E : 0x3F);
                                w16(n, 0x0400004A, window == 3 ? 0x1F : 0x3D);
                                const u16 first[] = {0, 1, 0x12, 0x3F}, second[] = {0, 1, 0x22, 0x3F};
                                w16(n, 0x04000050, first[targets] | effect << 6 | second[targets] << 8);
                                w16(n, 0x04000052, 0x0709);
                                w16(n, 0x04000054, 11);
                                w16(n, 0x0400006C, (targets % 3) << 14 | 8);
                                render(n, native.data());
                                auto& e = n.gpu.engine[0];
                                std::memcpy(saved.data(), e.output(), sizeof saved);
                                gpu::Engine2D::EnhancedPlan plan;
                                std::array<gpu::Pixel, 256> scratch{};
                                e.prepare_3d_sublines(plan);
                                gpu::Engine2D::compose_3d_subline(plan, e.render_evy(), e.master_bright(),
                                                                  subpixel.data(), scratch.data(), got.data());
                                if (std::memcmp(saved.data(), e.output(), sizeof saved)) {
                                    std::puts("FAIL: native output was changed");
                                    return 1;
                                }
                                render(n, subpixel.data());
                                gpu::kern::active::output_line(e.output(), e.master_bright(), expected.data());
                                if (got != expected) {
                                    std::fprintf(stderr, "FAIL: composition case %u\n", cases);
                                    return 1;
                                }
                                ++cases;
                            }
    }
    std::printf("hires compositor: %u priority/window/blend/brightness cases passed\n", cases);
}
