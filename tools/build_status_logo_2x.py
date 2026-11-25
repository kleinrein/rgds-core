"""The status bar's logo at 2x for panels larger than 640x480 (the RG DS Plus): assets/brand/status-{light,dark}@2x.bmp.

The 1x logo (status-*.bmp, 129x22) is a one-off adaptation of the SVG lockup with its own proportions (a smaller
mark, a slimmer pill). This finds its three parts' boxes (mark, lettering, pill) in the light 1x logo, renders each
part of lockup-*.svg on its own and places it in the doubled box, so the 2x logo keeps the 1x layout.

    uv run --no-project --with "pillow>=13" --with resvg-py python tools/build_status_logo_2x.py
"""
import io, re, struct, sys, resvg_py
from PIL import Image, ImageChops, ImageStat

def boxes(ref, theme):
    """The mark, the lettering and the pill in the 1x logo: blue before the lettering, the lettering (dark on light,
    light on dark), blue after it; each part's box from its own pixels."""
    W, H = ref.size
    px = ref.load()
    def blue(p): return p[3] > 60 and p[2] > p[0] + 50
    def letter(p):
        if p[3] <= 60 or blue(p): return False
        return (p[0] < 110 and p[2] < 130) if theme == 'light' else (p[0] > 150 and p[2] > 150 and abs(p[0] - p[2]) < 40)
    text_cols = [x for x in range(W) if any(letter(px[x, y]) for y in range(H))]
    t0, t1 = text_cols[0], text_cols[-1] + 1
    def bbox(x_from, x_to, test):
        pts = [(x, y) for x in range(x_from, x_to) for y in range(H) if test(px[x, y]) or (px[x, y][3] > 60 and test is blue)]
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        return (min(xs), min(ys), max(xs) + 1, max(ys) + 1)
    opaque = lambda p: p[3] > 60
    return [bbox(0, t0, opaque), bbox(t0, t1, letter), bbox(t1, W, opaque)]

def part(svg, which):
    body = svg[svg.index('</defs>') + 7:]
    head = svg[:svg.index('</defs>') + 7]
    groups = re.split(r'(?=<g transform="translate\(84)', body, maxsplit=1)
    mark, rest = groups[0], groups[1]
    if which == 'mark': inner = mark + '</svg>'
    else:
        text = re.search(r'<path d="[^"]*" fill="#[0-9A-Fa-f]{6}"\s*/>', rest).group(0)
        if which == 'text': inner = '<g transform="translate(84.00 10.00)">' + text + '</g></svg>'
        else: inner = rest.replace(text, '')
    return head + inner

def render(svg_text):
    img = Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_string=svg_text, zoom=12)))).convert('RGBA')
    return img.crop(img.getbbox())

def bmp(img, path):
    pixels = img.tobytes('raw', 'BGRA'); w, h = img.size
    dib = struct.pack('<IiiHHIIiiII', 108, w, -h, 1, 32, 3, len(pixels), 0, 0, 0, 0)
    dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    open(path, 'wb').write(struct.pack('<2sIHHI', b'BM', 122 + len(pixels), 0, 0, 122) + dib + pixels)

for theme in ('light', 'dark'):
    ref = Image.open(f'assets/brand/status-{theme}.bmp').convert('RGBA')
    svg = open(f'assets/brand/lockup-{theme}.svg').read()
    found = boxes(ref, theme) if theme == 'light' else light_boxes
    if theme == 'light': light_boxes = found
    print(theme, found)
    assert len(found) == 3, found
    out = Image.new('RGBA', (ref.width * 2, ref.height * 2))
    for name, (x0, y0, x1, y1) in zip(('mark', 'text', 'pill'), found):
        piece = render(part(svg, name)).resize(((x1 - x0) * 2, (y1 - y0) * 2), Image.LANCZOS)
        out.alpha_composite(piece, (x0 * 2, y0 * 2))
    back = out.resize(ref.size, Image.LANCZOS)
    print(theme, 'error vs 1x', round(sum(ImageStat.Stat(ImageChops.difference(back, ref)).mean), 2))
    bmp(out, f'assets/brand/status-{theme}@2x.bmp')
