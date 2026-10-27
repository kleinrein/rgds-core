#!/usr/bin/env python3
"""The Ports system's rich icon: PortMaster's own app icon on the system icons' canvas.

The other systems' rich icons are drawn hardware (tools/prepare_system_icons.py). Ports has no hardware; its
picture is PortMaster's icon (the cargo ship, runtime/portmaster_files/icon.png, from PortMaster-GUI, MIT). This
crops it to what is drawn, scales it with an area filter into the same 116 px bound on a 128 x 128 canvas and writes
the same 32-bit BGRA BMP. No Pillow needed.

    python3 tools/prepare_ports_icon.py      # writes assets/systems/ports.bmp
"""
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/portmaster_files/icon.png'
TARGET = ROOT / 'assets/systems/ports.bmp'
FIT_SIZE = 116
CANVAS_SIZE = 128
sys.path.insert(0, str(ROOT / 'runtime'))


def scaled(width, height, rows, size):
    """Area-filtered RGBA (premultiplied while averaging) at size on its longer side: (w, h, pixel tuples)."""
    scale = size / max(width, height)
    out_w, out_h = max(1, round(width * scale)), max(1, round(height * scale))
    pixels = []
    for oy in range(out_h):
        y0, y1 = oy / scale, min(height, (oy + 1) / scale)
        for ox in range(out_w):
            x0, x1 = ox / scale, min(width, (ox + 1) / scale)
            total = [0.0] * 4
            area = 0.0
            for sy in range(int(y0), min(height, int(y1) + 1)):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0: continue
                row = rows[sy]
                for sx in range(int(x0), min(width, int(x1) + 1)):
                    wx = min(x1, sx + 1) - max(x0, sx)
                    if wx <= 0: continue
                    r, g, b, a = row[sx * 4:sx * 4 + 4]
                    w = wx * wy
                    alpha = a / 255
                    total[0] += r * alpha * w
                    total[1] += g * alpha * w
                    total[2] += b * alpha * w
                    total[3] += alpha * w
                    area += w
            alpha = total[3] / area if area else 0
            colour = [round(total[i] / total[3]) if total[3] else 0 for i in range(3)]
            pixels.append((*colour, round(alpha * 255)))
    return out_w, out_h, pixels


def main():
    from portmaster import _png_rgba
    width, height, rows = _png_rgba(SOURCE)
    xs = [x for x in range(width) if any(row[x * 4 + 3] > 16 for row in rows)]
    ys = [y for y in range(height) if any(rows[y][x * 4 + 3] > 16 for x in range(width))]
    left, right, top, bottom = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    rows = [row[left * 4:right * 4] for row in rows[top:bottom]]
    w, h, pixels = scaled(right - left, bottom - top, rows, FIT_SIZE)
    canvas = [(0, 0, 0, 0)] * (CANVAS_SIZE * CANVAS_SIZE)
    ox, oy = (CANVAS_SIZE - w) // 2, (CANVAS_SIZE - h) // 2
    for y in range(h):
        for x in range(w):
            canvas[(oy + y) * CANVAS_SIZE + ox + x] = pixels[y * w + x]
    data = bytearray()
    for y in range(CANVAS_SIZE):  # top-down (negative height), BGRA, as the other system icons
        for r, g, b, a in canvas[y * CANVAS_SIZE:(y + 1) * CANVAS_SIZE]:
            data += bytes((b, g, r, a))
    info = struct.pack('<IiiHHIIiiII', 108, CANVAS_SIZE, -CANVAS_SIZE, 1, 32, 3, len(data), 2835, 2835, 0, 0)
    info += struct.pack('<IIII', 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000) + b'BGRs' + bytes(48)
    TARGET.write_bytes(b'BM' + struct.pack('<IHHI', 14 + len(info) + len(data), 0, 0, 14 + len(info)) + info + data)
    print(TARGET, w, 'x', h)
    return 0


if __name__ == '__main__':
    sys.exit(main())
