#!/usr/bin/env python3
"""Draw the profile characters: assets/profiles/<name>.bmp (192 x 192, alpha), pictures 12-23.

Smooth, flat cartoon faces in the spirit of streaming-service avatars, drawn here so they can be reproduced exactly:
no generated artwork and no one else's characters. Every outline is a closed curve through a few control points
(Catmull-Rom; a point marked 's' stays a sharp corner, for ear tips and horns), filled with a soft light-to-shade
gradient. Each face sits in a white sticker edge with a soft shadow, so it reads on any of the 16 DS colours behind
it. The launcher falls back to the 16 x 16 pixel versions in src/launcher/profile_pictures.hpp when a file is missing.

    python3 tools/build_profile_avatars.py [--png preview.png]
"""
import argparse
import math
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'assets/profiles'
SIZE, SUPER = 192, 6
UNIT = SIZE * SUPER / 100  # drawings are in a 100 x 100 box

INK = (43, 43, 51)
WHITE = (255, 255, 255)
CHEEK = (255, 128, 156, 140)

# In picture order: profile_picture_name() 12 to 23.
NAMES = ('fox', 'panda', 'frog', 'penguin', 'owl', 'bear', 'unicorn', 'dino', 'alien', 'astronaut', 'octopus', 'bunny')


def mix(colour, other, amount):
    return tuple(round(a + (b - a) * amount) for a, b in zip(colour[:3], other))


def mirror(points):
    """The other half of a symmetric face: the same points reflected about x = 50."""
    return [(100 - p[0], *p[1:]) for p in points]


def spline(points, closed=True, steps=24):
    """Catmull-Rom through the points; a point given as (x, y, 's') is a corner the curve does not round off."""
    expanded = []
    for point in points:
        expanded += [point[:2]] * (2 if len(point) > 2 else 1)
    if closed: ring = expanded[-1:] + expanded + expanded[:2]
    else: ring = expanded[:1] + expanded + expanded[-1:]
    out = []
    for i in range(1, len(ring) - 2):
        (x0, y0), (x1, y1), (x2, y2), (x3, y3) = ring[i - 1:i + 3]
        if (x1, y1) == (x2, y2): continue  # the doubled corner: no curve between a point and itself
        for k in range(steps):
            t = k / steps
            t2, t3 = t * t, t * t * t
            out.append(
                (
                    .5 *
                    (2 * x1 + (x2 - x0) * t + (2 * x0 - 5 * x1 + 4 * x2 - x3) * t2 + (3 * x1 - x0 - 3 * x2 + x3) * t3),
                    .5 *
                    (2 * y1 + (y2 - y0) * t + (2 * y0 - 5 * y1 + 4 * y2 - y3) * t2 + (3 * y1 - y0 - 3 * y2 + y3) * t3)
                )
            )
    if not closed: out.append(expanded[-1])
    return out


class Pen:
    def __init__(self):
        self.image = Image.new('RGBA', (SIZE * SUPER,) * 2, (0, 0, 0, 0))
        self.draw = ImageDraw.Draw(self.image, 'RGBA')

    def scaled(self, points):
        return [(x * UNIT, y * UNIT) for x, y in points]

    def fill(self, outline, colour, light=.16, shade=.10):
        """A shape, lit from above: its colour runs from a little lighter at the top to a little darker below."""
        mask = Image.new('L', self.image.size, 0)
        ImageDraw.Draw(mask).polygon(self.scaled(outline), fill=255)
        top = min(y for _, y in outline) * UNIT
        bottom = max(y for _, y in outline) * UNIT
        if len(colour) > 3 or (light == 0 and shade == 0):
            layer = Image.new('RGBA', self.image.size, colour if len(colour) > 3 else (*colour, 255))
        else:
            ramp = Image.new('RGBA', (1, self.image.height))
            for y in range(self.image.height):
                t = min(1, max(0, (y - top) / max(1, bottom - top)))
                tint = mix(colour, WHITE, light *
                           (1 - 2 * t)) if t < .5 else mix(colour, (0, 0, 0), shade * (2 * t - 1))
                ramp.putpixel((0, y), (*tint, 255))
            layer = ramp.resize(self.image.size)
        if len(colour) > 3: mask = mask.point(lambda a: a * colour[3] // 255)
        self.image.paste(layer.convert('RGB'), (0, 0), mask)

    def shape(self, points, colour, **kw):
        self.fill(spline(points), colour, **kw)

    def oval(self, x0, y0, x1, y1, colour, **kw):
        cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
        self.fill(
            [(cx + rx * math.cos(a / 48 * math.tau), cy + ry * math.sin(a / 48 * math.tau)) for a in range(48)], colour,
            **kw
        )

    def circle(self, x, y, r, colour, **kw):
        self.oval(x - r, y - r, x + r, y + r, colour, **kw)

    def stroke(self, points, width, colour=INK):
        """A round-capped line along a curve through the points."""
        path = self.scaled(spline(points, closed=False, steps=12))
        w = width * UNIT
        self.draw.line(path, fill=colour, width=int(w), joint='curve')
        for x, y in (path[0], path[-1]):
            self.draw.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=colour)

    def eye(self, x, y, r, look=(0, 0), iris=INK):
        self.circle(x, y, r, WHITE, light=0, shade=.08)
        self.circle(x + look[0], y + look[1], r * .64, iris, light=.2, shade=0)
        if iris != INK: self.circle(x + look[0], y + look[1], r * .36, INK, light=0, shade=0)
        self.circle(x + look[0] - r * .24, y + look[1] - r * .26, r * .2, WHITE, light=0, shade=0)

    def dot_eye(self, x, y, r):
        self.circle(x, y, r, INK, light=.18, shade=0)
        self.circle(x - r * .3, y - r * .36, r * .34, WHITE, light=0, shade=0)
        self.circle(x + r * .32, y + r * .3, r * .14, WHITE, light=0, shade=0)

    def smile(self, x, y, w, width=2.2):
        self.stroke([(x - w, y - w * .25), (x, y + w * .35), (x + w, y - w * .25)], width)

    def cheeks(self, left, right, y, r=4.6):
        self.circle(left, y, r, CHEEK)
        self.circle(right, y, r, CHEEK)


def fox(p):
    orange, cream, inner = (244, 132, 38), (255, 246, 236), (120, 62, 30)
    ear = [(32, 28), (14, 4, 's'), (10, 24), (14, 40)]
    p.shape(ear, orange)
    p.shape(mirror(ear), orange)
    p.shape([(28, 28), (16, 11, 's'), (14, 28)], inner, light=0, shade=0)
    p.shape(mirror([(28, 28), (16, 11, 's'), (14, 28)]), inner, light=0, shade=0)
    p.shape(
        [
            (50, 22), (72, 24), (88, 38), (95, 56, 's'), (78, 70), (50, 90, 's'), (22, 70), (5, 56, 's'), (12, 38),
            (28, 24)
        ], orange
    )
    p.shape(
        [
            (5, 56, 's'), (24, 54), (40, 62), (50, 70), (60, 62), (76, 54), (95, 56, 's'), (78, 70), (50, 90, 's'),
            (22, 70)
        ],
        cream,
        light=0
    )
    p.dot_eye(35, 49, 5)
    p.dot_eye(65, 49, 5)
    p.shape([(44, 72), (50, 70), (56, 72), (50, 79)], INK, light=.2, shade=0)
    p.cheeks(25, 75, 62, 4)


def panda(p):
    p.circle(20, 22, 12, INK, light=.18, shade=0)
    p.circle(80, 22, 12, INK, light=.18, shade=0)
    p.shape([(50, 16), (78, 22), (92, 48), (86, 76), (64, 90), (36, 90), (14, 76), (8, 48), (22, 22)], WHITE, light=0)
    patch = [(26, 46), (38, 40), (46, 47), (43, 60), (31, 66), (22, 58)]
    p.shape(patch, INK, light=.18, shade=0)
    p.shape(mirror(patch), INK, light=.18, shade=0)
    p.eye(36, 51, 4.6)
    p.eye(64, 51, 4.6)
    p.shape([(43, 65), (57, 65), (53, 71), (47, 71)], INK, light=.2, shade=0)
    p.stroke([(42, 76), (46, 78), (50, 75.5)], 2)
    p.stroke([(50, 75.5), (54, 78), (58, 76)], 2)
    p.cheeks(24, 76, 70, 4.4)


def frog(p):
    green, chin = (100, 196, 80), (196, 236, 150)
    p.circle(28, 30, 16, green)
    p.circle(72, 30, 16, green)
    p.shape([(50, 30), (80, 32), (95, 56), (86, 80), (50, 90), (14, 80), (5, 56), (20, 32)], green)
    p.shape([(22, 68), (50, 64), (78, 68), (82, 80), (50, 90), (18, 80)], chin, light=0)
    p.eye(28, 30, 11, (1.5, 1.5))
    p.eye(72, 30, 11, (-1.5, 1.5))
    p.circle(45, 52, 1.4, (50, 120, 50))
    p.circle(55, 52, 1.4, (50, 120, 50))
    p.stroke([(26, 58), (38, 66), (50, 68), (62, 66), (74, 58)], 2.6)
    p.cheeks(17, 83, 60, 5)


def penguin(p):
    navy, beak = (44, 56, 92), (255, 160, 36)
    p.shape([(50, 8), (80, 16), (92, 50), (84, 84), (50, 96), (16, 84), (8, 50), (20, 16)], navy, light=.22)
    p.shape(
        [(50, 32), (62, 22), (78, 30), (83, 52), (72, 78), (50, 90), (28, 78), (17, 52), (22, 30), (38, 22)],
        WHITE,
        light=0
    )
    p.dot_eye(37, 50, 5.4)
    p.dot_eye(63, 50, 5.4)
    p.shape([(41, 58), (50, 56), (59, 58), (50, 69, 's')], beak)
    p.cheeks(26, 74, 64, 4.6)


def owl(p):
    brown, disc, amber, beak = (150, 96, 60), (238, 206, 160), (255, 176, 40), (255, 200, 60)
    p.shape(
        [
            (50, 22), (72, 24), (88, 6, 's'), (90, 40), (90, 64), (78, 86), (50, 94), (22, 86), (10, 64), (10, 40),
            (12, 6, 's'), (28, 24)
        ], brown
    )
    p.shape([(30, 64), (50, 58), (70, 64), (74, 82), (50, 92), (26, 82)], disc, light=0)
    p.circle(33, 46, 17, disc, light=.1, shade=0)
    p.circle(67, 46, 17, disc, light=.1, shade=0)
    p.eye(33, 46, 11.5, (0, 1), amber)
    p.eye(67, 46, 11.5, (0, 1), amber)
    p.shape([(44, 56), (50, 54), (56, 56), (50, 66, 's')], beak)


def bear(p):
    brown, inner, snout = (156, 102, 66), (232, 156, 144), (242, 216, 184)
    p.circle(21, 24, 13, brown)
    p.circle(79, 24, 13, brown)
    p.circle(21, 25, 7, inner, light=0)
    p.circle(79, 25, 7, inner, light=0)
    p.shape([(50, 18), (76, 22), (92, 46), (88, 74), (66, 90), (34, 90), (12, 74), (8, 46), (24, 22)], brown)
    p.shape([(50, 56), (64, 60), (68, 72), (60, 82), (50, 84), (40, 82), (32, 72), (36, 60)], snout, light=0)
    p.shape([(43, 61), (57, 61), (54, 67), (46, 67)], INK, light=.2, shade=0)
    p.stroke([(50, 67), (50, 72)], 2)
    p.stroke([(44, 73), (50, 76), (56, 73)], 2)
    p.dot_eye(34, 47, 5)
    p.dot_eye(66, 47, 5)
    p.cheeks(23, 77, 64)


def unicorn(p):
    gold, stripe, inner = (255, 204, 70), (226, 160, 30), (255, 186, 206)
    pink, lilac, sky = (255, 126, 176), (176, 132, 246), (104, 196, 250)
    # Ears, then the head, then the mane sweeping from the crown down the left side, then the horn over it all.
    ear = [(31, 28), (22, 4, 's'), (14, 26)]
    p.shape(ear, WHITE, light=0)
    p.shape(mirror(ear), WHITE, light=0)
    p.shape([(27, 25), (22, 11, 's'), (18, 25)], inner, light=0, shade=0)
    p.shape(mirror([(27, 25), (22, 11, 's'), (18, 25)]), inner, light=0, shade=0)
    p.shape(
        [(50, 18), (72, 22), (85, 40), (85, 62), (75, 82), (50, 92), (25, 82), (15, 62), (15, 40), (28, 22)],
        WHITE,
        light=0,
        shade=.08
    )
    p.shape([(40, 20), (50, 24), (36, 32), (24, 46), (19, 64), (22, 84), (10, 76), (6, 56), (11, 34), (24, 22)], lilac)
    p.shape([(48, 16), (62, 19), (65, 27), (50, 29), (34, 34), (24, 44), (17, 56), (13, 44), (19, 29), (32, 19)], pink)
    p.shape([(58, 19), (70, 23), (73, 31), (65, 33), (59, 27)], sky)
    p.shape([(43, 24), (50, 2, 's'), (57, 24)], gold)
    for y in (19, 12):
        p.stroke([(45.5 + (24 - y) * .12, y + 1.5), (54.5 - (24 - y) * .12, y - 1.5)], 1.6, stripe)
    p.stroke([(45, 73), (46.5, 74.5)], 1.6, (214, 140, 170))
    p.stroke([(58.5, 73), (57, 74.5)], 1.6, (214, 140, 170))
    p.smile(52, 80, 4.5, 2)
    p.dot_eye(41, 57, 5.8)
    p.dot_eye(65, 57, 5.8)
    p.stroke([(35.6, 53.4), (33, 51.6)], 1.6)
    p.stroke([(70.4, 53.4), (73, 51.6)], 1.6)
    p.cheeks(31, 76, 67)


def dino(p):
    green, plate, belly = (92, 192, 112), (56, 146, 86), (214, 242, 194)
    for spike in (
        [(30, 24), (35, 6, 's'), (44, 20)], [(43, 19), (51, 0, 's'), (59, 19)], [(56, 20), (65, 6, 's'), (70, 24)]
    ):
        p.shape(spike, plate)
    p.shape([(50, 16), (76, 20), (90, 44), (88, 70), (70, 88), (50, 92), (30, 88), (12, 70), (10, 44), (24, 20)], green)
    p.shape([(24, 64), (50, 58), (76, 64), (78, 80), (50, 92), (22, 80)], belly, light=0)
    p.eye(35, 44, 9.5, (1, 1))
    p.eye(65, 44, 9.5, (-1, 1))
    p.circle(45, 58, 1.5, plate)
    p.circle(55, 58, 1.5, plate)
    p.stroke([(38, 68), (50, 74), (62, 68)], 2.4)
    p.cheeks(22, 78, 62)


def alien(p):
    green, bobble = (142, 222, 120), (255, 120, 176)
    p.stroke([(38, 18), (33, 9), (26, 5)], 2.4, (96, 170, 90))
    p.stroke([(62, 18), (67, 9), (74, 5)], 2.4, (96, 170, 90))
    p.circle(25, 5, 5, bobble)
    p.circle(75, 5, 5, bobble)
    p.shape([(50, 12), (76, 16), (94, 36), (90, 58), (70, 80), (50, 92), (30, 80), (10, 58), (6, 36), (24, 16)], green)
    almond = [(16, 42), (30, 40), (44, 48), (42, 60), (30, 62), (19, 54)]
    p.shape(almond, INK, light=.2, shade=0)
    p.shape(mirror(almond), INK, light=.2, shade=0)
    p.oval(24, 44, 32, 51, WHITE, light=0, shade=0)
    p.oval(68, 44, 76, 51, WHITE, light=0, shade=0)
    p.smile(50, 73, 5)
    p.cheeks(22, 78, 66, 4)


def astronaut(p):
    shell, rim, visor, glow = (238, 242, 248), (184, 194, 210), (36, 64, 140), (150, 210, 255)
    p.shape([(4, 42), (14, 40), (14, 64), (4, 62)], rim, light=0)
    p.shape([(96, 42), (86, 40), (86, 64), (96, 62)], rim, light=0)
    p.circle(50, 51, 41, shell, light=0, shade=.12)
    p.shape([(50, 26), (74, 29), (82, 48), (74, 68), (50, 72), (26, 68), (18, 48), (26, 29)], rim, light=0)
    p.shape([(50, 29), (72, 32), (79, 48), (72, 65), (50, 69), (28, 65), (21, 48), (28, 32)], visor, light=.3)
    p.shape([(30, 36), (42, 32), (40, 38), (31, 44)], (*glow, 170))
    p.circle(62, 36, 2.2, (*glow, 200))
    p.circle(39, 53, 3.4, WHITE)
    p.circle(61, 53, 3.4, WHITE)
    p.smile(50, 61, 4.5, 2)
    p.shape([(36, 82), (64, 82), (62, 90), (38, 90)], rim, light=0)
    p.shape([(45, 84), (55, 84), (55, 88), (45, 88)], (234, 62, 62), light=0, shade=0)


def octopus(p):
    purple, spot, tip = (172, 122, 238), (204, 172, 255), (140, 92, 210)
    arms = [[(26, 58), (17, 70), (17, 81), (24, 84)], [(40, 64), (37, 76), (42, 85)]]
    for arm in arms + [mirror(a) for a in arms]:
        p.stroke(arm, 11, purple)
        p.circle(*arm[-1], 2.2, tip, light=0, shade=0)
    p.shape([(50, 8), (76, 14), (90, 36), (86, 58), (70, 68), (50, 70), (30, 68), (14, 58), (10, 36), (24, 14)], purple)
    p.circle(30, 22, 6, spot, light=0, shade=0)
    p.circle(70, 18, 3.5, spot, light=0, shade=0)
    p.eye(36, 44, 8.6, (1, 1))
    p.eye(64, 44, 8.6, (-1, 1))
    p.smile(50, 59, 5.5)
    p.cheeks(22, 78, 55)


def bunny(p):
    inner = (255, 172, 198)
    ear = [(30, 40), (22, 18), (26, 2), (36, 6), (40, 24), (40, 40)]
    p.shape(ear, WHITE, light=0)
    p.shape(mirror(ear), WHITE, light=0)
    p.shape([(33, 36), (27, 18), (29, 8), (34, 10), (36, 24), (37, 36)], inner, light=0, shade=0)
    p.shape(mirror([(33, 36), (27, 18), (29, 8), (34, 10), (36, 24), (37, 36)]), inner, light=0, shade=0)
    p.shape([(50, 32), (76, 36), (90, 58), (84, 82), (50, 94), (16, 82), (10, 58), (24, 36)], WHITE, light=0, shade=.1)
    p.dot_eye(34, 60, 5.2)
    p.dot_eye(66, 60, 5.2)
    p.shape([(45, 68), (55, 68), (50, 74, 's')], (240, 108, 140))
    p.stroke([(42, 77), (46, 79), (50, 75)], 1.8)
    p.stroke([(50, 75), (54, 79), (58, 77)], 1.8)
    p.cheeks(22, 78, 72, 5)


def sticker(art):
    """A white edge round the drawing and a soft shadow under it."""
    alpha = art.getchannel('A').point(lambda a: 255 if a > 8 else 0)
    edge = alpha.filter(ImageFilter.MaxFilter(int(3 * UNIT) | 1))
    shadow = edge.filter(ImageFilter.GaussianBlur(2 * UNIT)).point(lambda a: a * 45 // 255)
    out = Image.new('RGBA', art.size, (0, 0, 0, 0))
    out.paste((20, 20, 40, 255), (0, int(1.5 * UNIT)), shadow)
    out.paste((255, 255, 255, 255), (0, 0), edge)
    return Image.alpha_composite(out, art)


def render(draw):
    pen = Pen()
    draw(pen)
    # The drawing keeps a margin inside the box for the sticker edge.
    art = Image.new('RGBA', pen.image.size, (0, 0, 0, 0))
    inset = int(6 * UNIT)
    art.paste(pen.image.resize((pen.image.width - 2 * inset,) * 2, Image.LANCZOS), (inset, inset))
    return sticker(art).resize((SIZE, SIZE), Image.LANCZOS)


def write_bmp(path, image):
    rgba = image.tobytes('raw', 'BGRA')
    dib = struct.pack('<IiiHHIIiiII', 108, SIZE, -SIZE, 1, 32, 3, len(rgba), 0, 0, 0, 0)
    dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    Path(path).write_bytes(struct.pack('<2sIHHI', b'BM', 122 + len(rgba), 0, 0, 122) + dib + rgba)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--png', help='also write a contact sheet on the DS colours')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    drawings = (fox, panda, frog, penguin, owl, bear, unicorn, dino, alien, astronaut, octopus, bunny)
    pictures = []
    for name, draw in zip(NAMES, drawings):
        image = render(draw)
        write_bmp(OUTPUT / (name + '.bmp'), image)
        pictures.append(image)
    if args.png:
        tiles = [
            (97, 130, 150), (186, 72, 32), (251, 0, 24), (251, 138, 251), (251, 146, 0), (243, 227, 0), (170, 251, 0),
            (0, 251, 0), (0, 162, 56), (73, 219, 138), (48, 186, 243), (0, 89, 243)
        ]
        sheet = Image.new('RGBA', (6 * 220 + 20, 2 * 220 + 20), (242, 245, 248, 255))
        for i, image in enumerate(pictures):
            x, y = 20 + (i % 6) * 220, 20 + (i // 6) * 220
            tile = Image.new('RGBA', (200, 200), (0, 0, 0, 0))
            ImageDraw.Draw(tile).rounded_rectangle([0, 0, 199, 199], 18, fill=(*tiles[i], 255))
            tile.alpha_composite(image, (4, 4))
            sheet.alpha_composite(tile, (x, y))
        sheet.convert('RGB').save(args.png)


if __name__ == '__main__':
    main()
