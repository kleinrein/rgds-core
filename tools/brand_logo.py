#!/usr/bin/env python3
"""Build the RG DS Core identity: SVG masters and exact-size runtime bitmaps.

The mark is a soft DSi-style app tile with the two DS screens and a stylus
stroke; the wordmark is "RG DS" in the launcher's own rounded typeface
(Nunito, OFL, weight 900) beside a "core" pill. Glyphs are converted to
outlines with fontTools, so the SVG masters need no font at all.

Every runtime bitmap is rendered at the exact pixel size it is drawn at (no
SDL scaling), by headless Chrome from the masters, then stored as alpha
BMP V4 for SDL. Design-time requirements: fontTools, Pillow, Google Chrome.

    python3 tools/brand_logo.py            # write assets/brand/*.svg and *.bmp
"""
import argparse
import base64
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT/'assets/brand'
FONT = ROOT/'assets/fonts/source/Nunito.ttf'
CHROME = Path('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome')

INK = {'light': '#2D3944', 'dark': '#EEF3F7'}
PILL = {'light': ('#4FA6EF', '#2580D2'), 'dark': ('#5AAFF3', '#2E89DA')}

DEFS = """<defs>
<linearGradient id="tile" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4FA6EF"/><stop offset="1" stop-color="#2580D2"/></linearGradient>
<linearGradient id="shine" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".38"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
<linearGradient id="screen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFFFFF"/><stop offset="1" stop-color="#E4F1FC"/></linearGradient>
</defs>"""

# The tile on a 120-unit grid: two 8:5 screens, the lower one with a stylus.
MARK = """<rect x="4" y="4" width="112" height="112" rx="32" fill="url(#tile)"/>
<path d="M4 36a32 32 0 0 1 32-32h48a32 32 0 0 1 32 32v8q-56 14-112 0z" fill="url(#shine)"/>
<rect x="28" y="19" width="64" height="40" rx="10" fill="url(#screen)"/>
<rect x="28" y="63" width="64" height="40" rx="10" fill="url(#screen)"/>
<path d="M70 94L84 78" stroke="#2580D2" stroke-width="5" stroke-linecap="round"/>
<circle cx="69" cy="95" r="2.6" fill="#2580D2"/>"""


def text_paths(text, weight, size, x, y, tracking=0.0):
    """Outline `text` from Nunito at `weight`: (svg path data, advance width)."""
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    font = instantiateVariableFont(TTFont(FONT), {'wght': weight})
    glyphs, cmap = font.getGlyphSet(), font.getBestCmap()
    scale = size / font['head'].unitsPerEm
    pen = SVGPathPen(glyphs)
    cursor = x
    for char in text:
        name = cmap[ord(char)]
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, cursor, y)))
        cursor += glyphs[name].width * scale + tracking
    return pen.getCommands(), cursor - x - tracking


CAP, XHEIGHT = .705, .484   # Nunito's OS/2 metrics, per em


def wordmark(theme, cap=40):
    """`RG DS` and the `core` pill, sized by cap height so it can match the
    UI's text exactly (the 20 px clock has a 14 px cap); returns (svg, w, h)."""
    size = cap / CAP
    pad = round(cap * .1, 2)                    # room for round overshoot
    baseline = pad + cap
    height = baseline + pad
    rg, rg_width = text_paths('RG DS', 900, size, 0, baseline, tracking=-size * .015)
    pill_h = cap * .84
    pill_y = baseline - cap / 2 - pill_h / 2   # centred on the capitals
    core_size = pill_h * .52 / XHEIGHT
    core, core_width = text_paths('core', 800, core_size, 0, 0)
    inset = pill_h * .44
    pill_w = core_width + 2 * inset
    pill_x = rg_width + cap * .2
    top, bottom = PILL[theme]
    width = pill_x + pill_w + 1
    core_baseline = pill_y + pill_h / 2 + core_size * XHEIGHT / 2
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.2f} {height:.2f}" width="{width:.2f}" height="{height:.2f}">'
           f'<defs><linearGradient id="pill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient></defs>'
           f'<path d="{rg}" fill="{INK[theme]}"/>'
           f'<rect x="{pill_x:.2f}" y="{pill_y:.2f}" width="{pill_w:.2f}" height="{pill_h:.2f}" rx="{pill_h / 2:.2f}" fill="url(#pill)"/>'
           f'<g transform="translate({pill_x + inset:.2f} {core_baseline:.2f})"><path d="{core}" fill="#fff"/></g></svg>')
    return svg, width, height


def mark_svg(size=120):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 120" width="{size}" height="{size}">{DEFS}{MARK}</svg>'


def lockup(theme, cap=40, tile=None):
    """Tile, then the wordmark, the text centred on the tile."""
    words, words_width, words_height = wordmark(theme, cap)
    tile = tile or round(cap * 1.7)
    gap = max(4, cap * .4)
    height = max(tile, words_height)
    inner = words.split('>', 1)[1].rsplit('</svg>', 1)[0]
    width = tile + gap + words_width
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.2f} {height:.2f}" width="{width:.2f}" height="{height:.2f}">{DEFS}'
           f'<g transform="translate(0 {(height - tile) / 2:.2f}) scale({tile / 120:.5f})">{MARK}</g>'
           f'<g transform="translate({tile + gap:.2f} {(height - words_height) / 2:.2f})">{inner}</g></svg>')
    return svg, width, height


def render(svg, width, height, out_png):
    """Rasterize with headless Chrome at exactly width x height, transparent."""
    with tempfile.TemporaryDirectory() as folder:
        page = Path(folder)/'page.html'
        data = base64.b64encode(svg.encode()).decode()
        page.write_text(f'<!doctype html><html><head><style>html,body{{margin:0;background:transparent}}img{{display:block;width:{width}px;height:{height}px}}</style></head>'
                        f'<body><img src="data:image/svg+xml;base64,{data}"></body></html>')
        subprocess.run([str(CHROME), '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                        '--default-background-color=00000000', f'--window-size={width},{height}',
                        f'--screenshot={out_png}', page.as_uri()], check=True, capture_output=True)


def bmp_v4(png, out_bmp):
    """Straight-alpha 32-bit BMP V4 (SDL keeps the alpha of V4 bitfields)."""
    from PIL import Image
    image = Image.open(png).convert('RGBA')
    width, height = image.size
    pixels = image.tobytes('raw', 'BGRA')
    dib = struct.pack('<IiiHHIIiiII', 108, width, -height, 1, 32, 3, len(pixels), 0, 0, 0, 0)
    dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    Path(out_bmp).write_bytes(struct.pack('<2sIHHI', b'BM', 122 + len(pixels), 0, 0, 122) + dib + pixels)


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    if not CHROME.is_file():
        raise SystemExit('Google Chrome is needed to rasterize the masters.')
    BRAND.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='rgds-brand-'))
    try:
        (BRAND/'mark.svg').write_text(mark_svg())
        outputs = []
        for size in (24, 48, 120, 144):
            outputs.append((mark_svg(size), size, size, f'mark-{size}'))
        for theme in ('light', 'dark'):
            (BRAND/f'wordmark-{theme}.svg').write_text(wordmark(theme)[0])
            (BRAND/f'lockup-{theme}.svg').write_text(lockup(theme)[0])
            # Boot scene wordmark (40 px capitals); status bar lockup with the
            # clock's 14 px capitals beside a 22 px tile.
            words, width, height = wordmark(theme, 40)
            outputs.append((words, round(width), round(height), f'wordmark-{theme}'))
            small, width, height = lockup(theme, 14, 22)
            outputs.append((small, round(width), round(height), f'status-{theme}'))
        for svg, width, height, name in outputs:
            png = work/(name + '.png')
            render(svg, width, height, png)
            bmp_v4(png, BRAND/(name + '.bmp'))
            shutil.copy2(png, work/(name + '.keep.png'))
        # Compatibility names used by older code paths: the tile itself.
        for theme in ('light', 'dark'):
            shutil.copy2(BRAND/'mark-144.bmp', BRAND/f'mark-{theme}.bmp')
        print('\n'.join(sorted(p.name for p in BRAND.iterdir())))
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
