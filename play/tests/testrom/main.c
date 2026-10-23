// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Play test cartridge: a tiny explore-the-rooms game with a map screen,
// shaped like the games the companion screen supports (Metroid, Castlevania).
//
//   title (scene 0) --START--> rooms (scene 1) --SELECT--> map opens (2) -> map (3)
//   walking off an edge fades to black (scene 4) and enters the next room.
//
// Everything the companion reads sits at fixed addresses (profiles/gba/ZRDT.ini).
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define REG16(a) (*(volatile u16*)(a))
#define REG32(a) (*(volatile u32*)(a))
#define DISPCNT REG16(0x04000000)
#define VCOUNT REG16(0x04000006)
#define KEYINPUT REG16(0x04000130)
#define DMA3SAD REG32(0x040000D4)
#define DMA3DAD REG32(0x040000D8)
#define DMA3CNT REG32(0x040000DC)
#define VRAM 0x06000000u

#define SCENE (*(volatile u8*)0x02000000)
#define ROOM (*(volatile u16*)0x02000002)
#define PX (*(volatile u16*)0x02000004)
#define PY (*(volatile u16*)0x02000006)
#define VISITED (*(volatile u16*)0x02000008)
#define TIMER (*(volatile u16*)0x0200000A)
#define PREVKEYS (*(volatile u16*)0x0200000C)
#define FRAME (*(volatile u32*)0x02000010)
#define SRAM ((volatile u8*)0x0E000000)

// Battery save: the explored rooms and the current one, so the companion's
// save handling can be tested. The tag lets emulators recognise SRAM.
__attribute__((used, section(".rodata.keep"))) static const char sram_tag[] = "SRAM_V113";

#define K_A 0x001
#define K_B 0x002
#define K_SELECT 0x004
#define K_START 0x008
#define K_RIGHT 0x010
#define K_LEFT 0x020
#define K_UP 0x040
#define K_DOWN 0x080

#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))
#define MAP_BG RGB(5, 0, 15)
#define COLS 4
#define ROWS 3

static const u16 room_colors[COLS * ROWS] = {
    RGB(20, 6, 6),  RGB(6, 18, 8),   RGB(8, 10, 24),  RGB(22, 18, 4), RGB(18, 6, 20), RGB(4, 18, 20),
    RGB(24, 12, 4), RGB(10, 22, 14), RGB(14, 14, 26), RGB(24, 8, 14), RGB(12, 16, 6), RGB(16, 16, 16),
};

void* memset(void* d, int c, unsigned n) {
    u8* p = d;
    while (n--) *p++ = (u8)c;
    return d;
}
void* memcpy(void* d, const void* s, unsigned n) {
    u8* p = d;
    const u8* q = s;
    while (n--) *p++ = *q++;
    return d;
}

static void fill(int x, int y, int w, int h, u16 color) {
    volatile u16 value = color;
    for (int row = 0; row < h; ++row) {
        DMA3SAD = (u32)&value;
        DMA3DAD = VRAM + (u32)((y + row) * 240 + x) * 2;
        DMA3CNT = 0x81000000u | (u32)w;
    }
}

static void wait_vblank(void) {
    while (VCOUNT >= 160) {}
    while (VCOUNT < 160) {}
}

static int room_x(void) { return ROOM & 3; }
static int room_y(void) { return ROOM >> 2; }

static void draw_room(void) {
    fill(0, 0, 240, 160, room_colors[ROOM]);
    fill(PX, PY, 8, 8, RGB(31, 31, 31));
}

static void draw_title(void) {
    fill(0, 0, 240, 160, RGB(4, 8, 16));
    fill(40, 70, 160, 20, RGB(31, 31, 31));
}

static void cell_rect(int cx, int cy, int* x, int* y) {
    *x = 40 + cx * 40;
    *y = 32 + cy * 32;
}

static void draw_marker(int on) {
    int x, y;
    cell_rect(room_x(), room_y(), &x, &y);
    fill(x + 13, y + 9, 10, 10, on ? RGB(31, 31, 31) : RGB(12, 22, 31));
}

static void draw_map(void) {
    fill(0, 0, 240, 160, MAP_BG);
    fill(36, 28, 168, 2, RGB(28, 28, 31));
    fill(36, 126, 168, 2, RGB(28, 28, 31));
    fill(36, 28, 2, 100, RGB(28, 28, 31));
    fill(202, 28, 2, 100, RGB(28, 28, 31));
    for (int cy = 0; cy < ROWS; ++cy)
        for (int cx = 0; cx < COLS; ++cx) {
            int x, y;
            cell_rect(cx, cy, &x, &y);
            int seen = (VISITED >> (cy * COLS + cx)) & 1;
            fill(x, y, 36, 28, seen ? RGB(12, 22, 31) : RGB(8, 4, 20));
        }
    draw_marker(1);
}

static void save_progress(void) {
    SRAM[0] = 'R';
    SRAM[1] = 'G';
    SRAM[2] = (u8)VISITED;
    SRAM[3] = (u8)(VISITED >> 8);
    SRAM[4] = (u8)ROOM;
}

static void load_progress(void) {
    if (SRAM[0] != 'R' || SRAM[1] != 'G' || SRAM[4] >= COLS * ROWS) return;
    VISITED = (u16)(SRAM[2] | (SRAM[3] << 8));
    ROOM = SRAM[4];
}

static void step_room(int dx, int dy) {
    int rx = room_x() + dx, ry = room_y() + dy;
    if (rx < 0 || rx >= COLS || ry < 0 || ry >= ROWS) return;
    ROOM = (u16)(ry * COLS + rx);
    VISITED = (u16)(VISITED | (1u << ROOM));
    if (dx > 0) PX = 4;
    if (dx < 0) PX = 228;
    if (dy > 0) PY = 4;
    if (dy < 0) PY = 148;
    save_progress();
    fill(0, 0, 240, 160, 0);
    SCENE = 4;
    TIMER = 8;
}

int main(void) {
    DISPCNT = 0x0403; // mode 3, BG2
    SCENE = 0;
    ROOM = 5;
    VISITED = 1u << 5;
    PX = 116;
    PY = 76;
    PREVKEYS = 0;
    FRAME = 0;
    load_progress();
    draw_title();
    for (;;) {
        wait_vblank();
        FRAME = FRAME + 1;
        u16 keys = (u16)(~KEYINPUT & 0x3FF);
        u16 pressed = (u16)(keys & ~PREVKEYS);
        PREVKEYS = keys;
        switch (SCENE) {
        case 0:
            if (pressed & K_START) {
                SCENE = 1;
                draw_room();
            }
            break;
        case 1: {
            if (pressed & K_SELECT) {
                fill(0, 0, 240, 160, 0);
                SCENE = 2;
                TIMER = 10;
                break;
            }
            int x = PX, y = PY;
            if (keys & K_RIGHT) x += 2;
            if (keys & K_LEFT) x -= 2;
            if (keys & K_DOWN) y += 2;
            if (keys & K_UP) y -= 2;
            if (x > 232) {
                step_room(1, 0);
                if (SCENE == 4) break;
                x = 232;
            }
            if (x < 0) {
                step_room(-1, 0);
                if (SCENE == 4) break;
                x = 0;
            }
            if (y > 152) {
                step_room(0, 1);
                if (SCENE == 4) break;
                y = 152;
            }
            if (y < 0) {
                step_room(0, -1);
                if (SCENE == 4) break;
                y = 0;
            }
            if (x != PX || y != PY) {
                fill(PX, PY, 8, 8, room_colors[ROOM]);
                PX = (u16)x;
                PY = (u16)y;
                fill(PX, PY, 8, 8, RGB(31, 31, 31));
            }
            break;
        }
        case 2:
            if (--TIMER == 0) {
                draw_map();
                SCENE = 3;
            }
            break;
        case 3:
            if (pressed & (K_SELECT | K_B)) {
                SCENE = 1;
                draw_room();
                break;
            }
            if ((FRAME & 15) == 0) draw_marker((FRAME >> 4) & 1);
            break;
        case 4:
            if (--TIMER == 0) {
                SCENE = 1;
                draw_room();
            }
            break;
        }
    }
}
