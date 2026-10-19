/* RGDS raster-rules ROM: an original DS 3D scene that draws one rendering
 * rule per 64x48 screen cell, every frame the same, so the GPU raster can be
 * checked against the CPU raster (tools/raster_rules.py). No game assets,
 * no SDK: registers only, like the graphics probe.
 *
 * Geometry is axis-aligned quads on pixel coordinates: x = -4096 + px * 32,
 * y = 4096 - py * 128 / 3 (so py is a multiple of 3). Colours are flat per
 * polygon and textures are mapped one texel per pixel, so inside a polygon
 * every sample position gives the same value as the DS corner sample; only
 * edge pixels may differ between rasterizers.
 *
 * Cells (column, row) and what they test:
 *  (0,0) marker: one opaque red quad (orientation and colour decode)
 *  (1,0) translucent same id over its own alpha-0 texels: B blends only there
 *  (2,0) the same with a different id: B blends everywhere
 *  (3,0) translucent cut-out with depth write: B behind it shows in the holes
 *  (0,1) the same without depth write: B blends over A's pixels too
 *  (1,1) depth-equal decal on a surface (left) and the same without the bit (right, invisible)
 *  (2,1) shadow volume over id 0 (darkened, left) and over its own id 10 (skipped, right)
 *  (3,1) alpha test (ref 10) on a modulated A5I3 texture, then the same id over it
 *  (0,2) alpha-31 polygon with A5I3 texels: opaque pixels take a later same-id polygon
 *  (1,2) fog by depth (three fogged quads, one unfogged)
 *  (2,2) toon shading from flat vertex reds
 *  (3,2) decal blend of an A5I3 texture over the vertex colour
 *  (0,3) repeat + flip texture wrap on an opaque cut-out over a backdrop
 *  (1,3) translucent polygons over the empty rear plane (clear alpha 0)
 *  (2,3) opaque overlap order, including an opaque cut-out submitted last
 *  (3,3) A3I5 polygon of alpha 31: opaque texels take a later same-id polygon
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define REG32(a) (*(volatile u32 *)(a))
#define REG16(a) (*(volatile u16 *)(a))
#define REG8(a) (*(volatile u8 *)(a))
#define GX(cmd) REG32(0x04000400 + (cmd) * 4)
#define C_MTX_MODE 0x10
#define C_MTX_IDENTITY 0x15
#define C_COLOR 0x20
#define C_TEXCOORD 0x22
#define C_VTX_16 0x23
#define C_POLYGON_ATTR 0x29
#define C_TEXIMAGE_PARAM 0x2A
#define C_PLTT_BASE 0x2B
#define C_BEGIN_VTXS 0x40
#define C_END_VTXS 0x41
#define C_SWAP_BUFFERS 0x50
#define C_VIEWPORT 0x60

/* POLYGON_ATTR pieces. */
#define MODE_MODULATE 0
#define MODE_DECAL 1
#define MODE_TOON 2
#define MODE_SHADOW 3
#define PA_DEPTH_WRITE (1u << 11)
#define PA_DEPTH_EQUAL (1u << 14)
#define PA_FOG (1u << 15)
static u32 attr(u32 mode, u32 alpha, u32 id, u32 extra) { return 0xC0u | (mode << 4) | (alpha << 16) | (id << 24) | extra; }

/* Textures: 16x16 each, in VRAM A; palettes in VRAM E. */
#define TEX_NONE 0u
#define TEX_CHECKER (0x0000u / 8u | (1u << 20) | (1u << 23) | (1u << 26))   /* A3I5: 4-px checker of alpha 31 / 0, blue */
#define TEX_BANDS   (0x0100u / 8u | (1u << 20) | (1u << 23) | (6u << 26))   /* A5I3: 4-px bands of alpha 0, 6, 16, 31; red top half, yellow bottom */
#define TEX_STRIPES (0x0200u / 8u | (1u << 20) | (1u << 23) | (7u << 26))   /* direct colour: 4-px colour stripes, bottom half alpha 0 */
#define PAL_CHECKER 0u
#define PAL_BANDS (0x40u / 16u)
#define REPEAT_S (1u << 16)
#define REPEAT_T (1u << 17)
#define FLIP_S (1u << 18)
#define FLIP_T (1u << 19)

static int cur_tex = 0;

static void texture(u32 param, u32 pal) {
    GX(C_TEXIMAGE_PARAM) = param;
    GX(C_PLTT_BASE) = pal;
    cur_tex = param != 0;
}

static void vertex(int px, int py, int z, int tx0, int ty0, int px1) {
    if (cur_tex) {
        /* Texel s = pixel offset, reaching width + 1 at the right vertex:
         * both rasterizers interpolate a span over [xstart, xend + 1). */
        int s = (px - tx0 + (px == px1 ? 1 : 0)) * 16, t = (py - ty0) * 16;
        GX(C_TEXCOORD) = (u32)(u16)s | ((u32)(u16)t << 16);
    }
    int x = -4096 + px * 32, y = 4096 - (py / 3) * 128;
    GX(C_VTX_16) = (u32)(u16)x | ((u32)(u16)y << 16);
    GX(C_VTX_16) = (u32)(u16)z;
}

/* An axis-aligned quad covering pixels [px0, px1] x [py0, py1) at depth z (nearer is smaller). */
static void quad(int px0, int py0, int px1, int py1, int z) {
    GX(C_BEGIN_VTXS) = 1;
    vertex(px0, py0, z, px0, py0, px1);
    vertex(px1, py0, z, px0, py0, px1);
    vertex(px1, py1, z, px0, py0, px1);
    vertex(px0, py1, z, px0, py0, px1);
    GX(C_END_VTXS) = 0;
}

static void colour(u32 r, u32 g, u32 b) { GX(C_COLOR) = r | (g << 5) | (b << 10); }
static void poly(u32 a, u32 r, u32 g, u32 b, u32 tex, u32 pal) { GX(C_POLYGON_ATTR) = a; texture(tex, pal); colour(r, g, b); }

static void write_textures(void) {
    u16 buf[256];
    volatile u16 *vram = (volatile u16 *)0x06800000;   /* VRAM A as LCDC */
    volatile u16 *pal = (volatile u16 *)0x06880000;    /* VRAM E as LCDC */
    REG8(0x04000240) = 0x80;
    REG8(0x04000244) = 0x80;
    /* A3I5 checker: index 1, alpha 7 (31) or 0 in 4-px blocks. */
    for (int i = 0; i < 256; i += 2) {
        u16 w = 0;
        for (int k = 0; k < 2; ++k) {
            int x = (i + k) & 15, y = (i + k) >> 4;
            u8 a = (((x >> 2) + (y >> 2)) & 1) ? 7 : 0;
            w |= (u16)((a << 5) | 1) << (8 * k);
        }
        buf[i >> 1] = w;
    }
    for (int i = 0; i < 128; ++i) vram[i] = buf[i];
    /* A5I3 bands: alpha 0, 6, 16, 31 by column block; index 1 (top half) or 2 (bottom). */
    static const u8 band_alpha[4] = {0, 6, 16, 31};
    for (int i = 0; i < 256; i += 2) {
        u16 w = 0;
        for (int k = 0; k < 2; ++k) {
            int x = (i + k) & 15, y = (i + k) >> 4;
            w |= (u16)((band_alpha[x >> 2] << 3) | (y < 8 ? 1 : 2)) << (8 * k);
        }
        buf[i >> 1] = w;
    }
    for (int i = 0; i < 128; ++i) vram[0x80 + i] = buf[i];
    /* Direct colour stripes: colour by column block, alpha bit clear on the bottom half. */
    for (int i = 0; i < 256; ++i) {
        int x = i & 15, y = i >> 4;
        static const u16 stripe[4] = {31, 31 << 5, 31 << 10, 31 | (31 << 5)};
        vram[0x100 + i] = stripe[x >> 2] | (y < 8 ? 0x8000 : 0);
    }
    /* Palettes: checker index 1 blue; bands index 1 red, 2 yellow. */
    pal[0] = 0; pal[1] = 31 << 10;
    pal[0x20] = 0; pal[0x21] = 31; pal[0x22] = 31 | (31 << 5);
    REG8(0x04000240) = 0x83;   /* texture image slot 0 */
    REG8(0x04000244) = 0x83;   /* texture palette */
}

static void cell_backdrop(int X, int Y, u32 id, u32 r, u32 g, u32 b, int z) {
    poly(attr(MODE_MODULATE, 31, id, 0), r, g, b, TEX_NONE, 0);
    quad(X + 2, Y + 3, X + 61, Y + 45, z);
}

static void draw_opaque(void) {
    int X, Y;
    /* (0,0) marker */
    X = 0; Y = 0;
    poly(attr(MODE_MODULATE, 31, 1, 0), 31, 0, 0, TEX_NONE, 0); quad(X + 4, Y + 3, X + 59, Y + 45, 0);
    /* backdrops for the translucent cells */
    cell_backdrop(64, 0, 0, 24, 24, 24, 1000);
    cell_backdrop(128, 0, 0, 24, 24, 24, 1000);
    cell_backdrop(192, 0, 0, 24, 24, 24, 1000);
    cell_backdrop(0, 48, 0, 24, 24, 24, 1000);
    cell_backdrop(192, 48, 0, 24, 24, 24, 1000);
    cell_backdrop(0, 96, 0, 24, 24, 24, 1000);
    cell_backdrop(192, 144, 0, 24, 24, 24, 1000);
    /* (1,1) decal surfaces at z 0 */
    X = 64; Y = 48;
    poly(attr(MODE_MODULATE, 31, 0, 0), 20, 20, 20, TEX_NONE, 0); quad(X + 2, Y + 3, X + 31, Y + 45, 0);
    poly(attr(MODE_MODULATE, 31, 0, 0), 20, 20, 20, TEX_NONE, 0); quad(X + 32, Y + 3, X + 61, Y + 45, 0);
    /* (2,1) shadow backdrops: id 0 left, id 10 right */
    X = 128; Y = 48;
    poly(attr(MODE_MODULATE, 31, 0, 0), 28, 28, 20, TEX_NONE, 0); quad(X + 2, Y + 3, X + 31, Y + 45, 0);
    poly(attr(MODE_MODULATE, 31, 10, 0), 28, 28, 20, TEX_NONE, 0); quad(X + 32, Y + 3, X + 61, Y + 45, 0);
    /* (1,2) fog: three fogged depths and one unfogged */
    X = 64; Y = 96;
    poly(attr(MODE_MODULATE, 31, 0, PA_FOG), 31, 31, 31, TEX_NONE, 0); quad(X + 2, Y + 3, X + 31, Y + 24, -3000);
    poly(attr(MODE_MODULATE, 31, 0, PA_FOG), 31, 31, 31, TEX_NONE, 0); quad(X + 32, Y + 3, X + 61, Y + 24, 0);
    poly(attr(MODE_MODULATE, 31, 0, PA_FOG), 31, 31, 31, TEX_NONE, 0); quad(X + 2, Y + 24, X + 31, Y + 45, 3000);
    poly(attr(MODE_MODULATE, 31, 0, 0), 31, 31, 31, TEX_NONE, 0); quad(X + 32, Y + 24, X + 61, Y + 45, 3000);
    /* (2,2) toon: flat reds 0, 10, 20, 31 */
    X = 128; Y = 96;
    poly(attr(MODE_TOON, 31, 0, 0), 0, 0, 0, TEX_NONE, 0); quad(X + 2, Y + 3, X + 31, Y + 24, 0);
    poly(attr(MODE_TOON, 31, 0, 0), 10, 0, 0, TEX_NONE, 0); quad(X + 32, Y + 3, X + 61, Y + 24, 0);
    poly(attr(MODE_TOON, 31, 0, 0), 20, 0, 0, TEX_NONE, 0); quad(X + 2, Y + 24, X + 31, Y + 45, 0);
    poly(attr(MODE_TOON, 31, 0, 0), 31, 0, 0, TEX_NONE, 0); quad(X + 32, Y + 24, X + 61, Y + 45, 0);
    /* (0,3) wrap: a backdrop, then an opaque cut-out with repeat + flip over it */
    X = 0; Y = 144;
    cell_backdrop(X, Y, 0, 0, 20, 0, 1000);
    poly(attr(MODE_MODULATE, 31, 2, 0), 31, 31, 31, TEX_STRIPES | REPEAT_S | FLIP_S | REPEAT_T | FLIP_T, 0); quad(X + 4, Y + 3, X + 59, Y + 45, -500);
    /* (2,3) opaque overlap order: far first, then near, then a far one submitted last, then a cut-out */
    X = 128; Y = 144;
    poly(attr(MODE_MODULATE, 31, 3, 0), 0, 0, 31, TEX_NONE, 0); quad(X + 2, Y + 3, X + 40, Y + 30, 2000);
    poly(attr(MODE_MODULATE, 31, 4, 0), 0, 31, 0, TEX_NONE, 0); quad(X + 20, Y + 12, X + 61, Y + 45, -2000);
    poly(attr(MODE_MODULATE, 31, 5, 0), 31, 0, 31, TEX_NONE, 0); quad(X + 10, Y + 21, X + 50, Y + 45, 3000);
    poly(attr(MODE_MODULATE, 31, 6, 0), 31, 31, 31, TEX_CHECKER, PAL_CHECKER); quad(X + 2, Y + 3, X + 61, Y + 24, -3000);
}

static void draw_translucent(void) {
    int X, Y;
    /* (1,0) same id 5 with alpha-0 texels, (2,0) B with id 6 */
    for (int c = 0; c < 2; ++c) {
        X = 64 + 64 * c; Y = 0;
        poly(attr(MODE_MODULATE, 16, 5, 0), 31, 31, 31, TEX_CHECKER, PAL_CHECKER); quad(X + 4, Y + 6, X + 43, Y + 42, -1000);
        poly(attr(MODE_MODULATE, 16, c ? 6 : 5, 0), 0, 31, 0, TEX_NONE, 0); quad(X + 20, Y + 6, X + 59, Y + 42, -1500);
    }
    /* (3,0) depth-write cut-out, then B further away; (0,1) the same without depth write */
    for (int c = 0; c < 2; ++c) {
        X = c ? 0 : 192; Y = c ? 48 : 0;
        poly(attr(MODE_MODULATE, 16, 7, c ? 0 : PA_DEPTH_WRITE), 31, 31, 31, TEX_CHECKER, PAL_CHECKER); quad(X + 4, Y + 6, X + 43, Y + 42, -1500);
        poly(attr(MODE_MODULATE, 20, 8, 0), 31, 0, 31, TEX_NONE, 0); quad(X + 20, Y + 6, X + 59, Y + 42, -500);
    }
    /* (1,1) decals at the surface depth: with and without the depth-equal bit */
    X = 64; Y = 48;
    poly(attr(MODE_MODULATE, 16, 9, PA_DEPTH_EQUAL), 0, 31, 31, TEX_NONE, 0); quad(X + 8, Y + 9, X + 27, Y + 39, 0);
    poly(attr(MODE_MODULATE, 16, 9, 0), 0, 31, 31, TEX_NONE, 0); quad(X + 36, Y + 9, X + 55, Y + 39, 0);
    /* (2,1) shadow: mask (id 0) behind the surfaces, shadow (id 10) in front */
    X = 128; Y = 48;
    poly(attr(MODE_SHADOW, 16, 0, 0), 0, 0, 0, TEX_NONE, 0); quad(X + 16, Y + 9, X + 47, Y + 39, 1000);
    poly(attr(MODE_SHADOW, 16, 10, 0), 0, 0, 0, TEX_NONE, 0); quad(X + 16, Y + 9, X + 47, Y + 39, -1000);
    /* (3,1) alpha test on modulated bands, then the same id over it */
    X = 192; Y = 48;
    poly(attr(MODE_MODULATE, 16, 11, 0), 31, 31, 31, TEX_BANDS, PAL_BANDS); quad(X + 4, Y + 6, X + 59, Y + 42, -1000);
    poly(attr(MODE_MODULATE, 16, 11, 0), 31, 20, 0, TEX_NONE, 0); quad(X + 4, Y + 6, X + 59, Y + 42, -1500);
    /* (0,2) alpha 31 with A5I3 texels, then the same id over it */
    X = 0; Y = 96;
    poly(attr(MODE_MODULATE, 31, 12, 0), 31, 31, 31, TEX_BANDS, PAL_BANDS); quad(X + 4, Y + 6, X + 59, Y + 42, -1000);
    poly(attr(MODE_MODULATE, 16, 12, 0), 0, 0, 31, TEX_NONE, 0); quad(X + 4, Y + 6, X + 59, Y + 42, -1500);
    /* (3,2) decal of the bands over magenta (A5I3 makes it translucent; alpha stays 31) */
    X = 192; Y = 96;
    poly(attr(MODE_DECAL, 31, 13, 0), 31, 0, 31, TEX_BANDS, PAL_BANDS); quad(X + 4, Y + 6, X + 59, Y + 42, -1000);
    /* (1,3) over the empty rear plane: A, then B of another id overlapping */
    X = 64; Y = 144;
    poly(attr(MODE_MODULATE, 16, 14, 0), 31, 0, 0, TEX_NONE, 0); quad(X + 4, Y + 6, X + 43, Y + 42, -1000);
    poly(attr(MODE_MODULATE, 16, 15, 0), 0, 0, 31, TEX_NONE, 0); quad(X + 20, Y + 6, X + 59, Y + 42, -1500);
    /* (3,3) A3I5 at alpha 31: opaque texels, then the same id over it */
    X = 192; Y = 144;
    poly(attr(MODE_MODULATE, 31, 16, 0), 31, 31, 31, TEX_CHECKER, PAL_CHECKER); quad(X + 4, Y + 6, X + 59, Y + 42, -1000);
    poly(attr(MODE_MODULATE, 16, 16, 0), 31, 31, 0, TEX_NONE, 0); quad(X + 4, Y + 6, X + 59, Y + 42, -1500);
}

void main(void) {
    REG16(0x04000304) = 0x820f;
    REG32(0x04000000) = 0x10108; /* Display mode 1, BG0 enabled and 3D. */
    REG16(0x04000008) = 0;
    write_textures();
    /* DISP3DCNT: textures, alpha test, alpha blending, fog (shift 0). */
    REG16(0x04000060) = 0x008D;
    REG16(0x04000340) = 10;                 /* alpha test reference */
    REG32(0x04000350) = 0;                  /* rear plane: black, alpha 0, id 0 */
    REG16(0x04000354) = 0x7fff;
    REG32(0x04000358) = (31u << 10) | (31u << 16);   /* fog colour blue, alpha 31 */
    REG16(0x0400035c) = 0;
    for (int i = 0; i < 8; ++i) {   /* fog density ramp 0..124 */
        u32 w = 0;
        for (int k = 0; k < 4; ++k) w |= (u32)((i * 4 + k) * 4) << (8 * k);
        REG32(0x04000360 + i * 4) = w;
    }
    for (int i = 0; i < 32; ++i) REG16(0x04000380 + i * 2) = (u16)(i | ((31 - i) << 5));   /* toon: red up, green down */
    GX(C_VIEWPORT) = 0xbfff0000;
    GX(C_MTX_MODE) = 0; GX(C_MTX_IDENTITY) = 0;
    GX(C_MTX_MODE) = 1; GX(C_MTX_IDENTITY) = 0;
    GX(C_MTX_MODE) = 3; GX(C_MTX_IDENTITY) = 0;
    volatile u32 *clock = (volatile u32 *)0x02010000;
    clock[0] = 0x53444752; clock[1] = 0x4d415246;
    clock[2] = 0x4f525045; clock[3] = 0x31304542;
    u32 low = 0, high = 0;
    for (;;) {
        while (REG16(0x04000006) >= 192) {}
        while (REG16(0x04000006) < 192) {}
        if (++low == 0) ++high;
        clock[4] = low; clock[5] = high; clock[6] = ~low; clock[7] = ~high;
        draw_opaque();
        draw_translucent();
        GX(C_SWAP_BUFFERS) = 1;   /* translucent polygons in submission order, Z-buffer */
    }
}
