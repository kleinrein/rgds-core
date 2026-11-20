"""Screenshots: the album on disk, PNG writing and the launcher's album commands; no imaging dependencies.

The album is the player's own: <player root>/screenshots/<game key>/, one folder per game (the key is
library.game_key of the ROM, or "launcher" for the menus). Each picture is <YYYYMMDD-HHMMSS>[-n].png with a
small <name>.thumb.bmp beside it for the launcher's grid; the folder's album.ini names the game. settings.ini in
screenshots/ keeps which screens a picture holds: both (upper above lower, one PNG), top or bottom.
"""
import argparse
from pathlib import Path
import shutil
import struct
import sys
import time
import zlib

from fields import read_fields, write_fields
from profiles import device_root, look_root

SCREENS = ('both', 'top', 'bottom')
LAUNCHER = 'launcher'
THUMB_WIDTH = 160
MAX_SIDE = 2560
BUDGET = 16 * 1024 * 1024


class Picture:
    """Pixels as packed RGB rows, top row first."""
    def __init__(self, width, height, rgb):
        if len(rgb) != width * height * 3:
            raise ValueError('Picture size mismatch')
        self.width, self.height, self.rgb = width, height, rgb

    def row(self, y):
        return self.rgb[y * self.width * 3:(y + 1) * self.width * 3]

    def crop(self, x, y, w, h):
        out = bytearray()
        for row in range(y, y + h):
            start = (row * self.width + x) * 3
            out += self.rgb[start:start + w * 3]
        return Picture(w, h, bytes(out))


def read_bmp(path):
    """SDL's uncompressed 24/32-bit BMP (what the bridge and the launcher capture write)."""
    path = Path(path)
    if path.stat().st_size > BUDGET:
        raise ValueError('Capture exceeds budget')
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('Invalid capture')
    offset = struct.unpack_from('<I', data, 10)[0]
    header, width, height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    if header < 40 or planes != 1 or bits not in (24, 32) or compression not in (
        0, 3
    ) or not (0 < width <= MAX_SIDE and 0 < abs(height) <= MAX_SIDE):
        raise ValueError('Unsupported capture')
    order = (2, 1, 0)  # BGR(A) in the file
    if compression == 3:
        if bits != 32 or len(data) < 66:
            raise ValueError('Unsupported capture masks')
        masks = struct.unpack_from('<III', data, 54)
        shifts = {0xff: 0, 0xff00: 1, 0xff0000: 2, 0xff000000: 3}
        if any(mask not in shifts for mask in masks):
            raise ValueError('Unsupported capture masks')
        order = tuple(shifts[mask] for mask in masks)
    h = abs(height)
    step = bits // 8
    stride = (width * step + 3) & ~3
    if offset < 14 + header or len(data) < offset + stride * h:
        raise ValueError('Truncated capture')
    rgb = bytearray(width * h * 3)
    for y in range(h):
        row = offset + (y if height < 0 else h - 1 - y) * stride
        line = data[row:row + width * step]
        target = y * width * 3
        for channel in range(3):
            rgb[target + channel:target + width * 3:3] = line[order[channel]::step]
    return Picture(width, h, bytes(rgb))


def write_bmp(path, picture):
    stride = (picture.width * 3 + 3) & ~3
    pad = b'\0' * (stride - picture.width * 3)
    body = bytearray()
    for y in range(picture.height):
        line = bytearray(picture.row(y))
        line[0::3], line[2::3] = line[2::3], line[0::3]
        body += line + pad
    head = struct.pack('<2sIHHI', b'BM', 54 + len(body), 0, 0, 54)
    head += struct.pack('<IiiHHIIiiII', 40, picture.width, -picture.height, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    atomic_write(path, head + bytes(body))


def encode_png(picture, level=6):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))

    raw = bytearray()
    for y in range(picture.height):
        raw.append(0)  # filter: none (game pictures compress well enough; the device CPU stays free)
        raw += picture.row(y)
    header = struct.pack('>IIBBBBB', picture.width, picture.height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(bytes(raw),
                                                                                        level)) + chunk(b'IEND', b'')


def decode_png(data):
    """Our own PNGs only (8-bit RGB or RGBA, not interlaced), so a cover or a PictoChat picture reads them back."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Not a PNG')
    at, idat, header = 8, bytearray(), None
    while at + 8 <= len(data):
        length, kind = struct.unpack_from('>I4s', data, at)
        payload = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', payload)
        elif kind == b'IDAT':
            idat += payload
        elif kind == b'IEND':
            break
    if not header:
        raise ValueError('PNG without a header')
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace or not (0 < width <= MAX_SIDE and 0 < height <= MAX_SIDE):
        raise ValueError('Unsupported PNG')
    step = 3 if colour == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * step
    if len(raw) < (stride + 1) * height:
        raise ValueError('Truncated PNG')
    previous = bytearray(stride)
    rgb = bytearray()
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for x in range(step, stride):
                line[x] = (line[x] + line[x - step]) & 255
        elif kind == 2:
            for x in range(stride):
                line[x] = (line[x] + previous[x]) & 255
        elif kind == 3:
            for x in range(stride):
                left = line[x - step] if x >= step else 0
                line[x] = (line[x] + (left + previous[x]) // 2) & 255
        elif kind == 4:
            for x in range(stride):
                a = line[x - step] if x >= step else 0
                b, c = previous[x], previous[x - step] if x >= step else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind:
            raise ValueError('Bad PNG filter')
        previous = line
        if step == 3:
            rgb += line
        else:
            out = bytearray(width * 3)
            for channel in range(3):
                out[channel::3] = line[channel::4]
            rgb += out
    return Picture(width, height, bytes(rgb))


def scale(picture, width, height):
    """Nearest-neighbour: a game's pixels stay sharp."""
    out = bytearray(width * height * 3)
    columns = [x * picture.width // width * 3 for x in range(width)]
    for y in range(height):
        source = picture.row(y * picture.height // height)
        target = y * width * 3
        for x, column in enumerate(columns):
            out[target + x * 3:target + x * 3 + 3] = source[column:column + 3]
    return Picture(width, height, bytes(out))


def stack(top, bottom):
    """Upper screen above the lower one, the narrower centred on black."""
    width = max(top.width, bottom.width)
    out = bytearray()
    for picture in (top, bottom):
        left = (width - picture.width) // 2
        for y in range(picture.height):
            out += b'\0' * (left * 3) + picture.row(y) + b'\0' * ((width - picture.width - left) * 3)
    return Picture(width, top.height + bottom.height, bytes(out))


def split_combined(picture):
    """One window across both panels (side by side or stacked) into (top, bottom)."""
    if picture.width >= picture.height * 2:
        half = picture.width // 2
        return picture.crop(0, 0, half, picture.height), picture.crop(half, 0, half, picture.height)
    if picture.height >= picture.width * 3 // 2:
        half = picture.height // 2
        return picture.crop(0, 0, picture.width, half), picture.crop(0, half, picture.width, half)
    return picture, None


def atomic_write(path, data):
    path = Path(path)
    pending = path.with_name('.' + path.name + '.pending')
    pending.write_bytes(data)
    pending.replace(path)


def title_for(rom):
    """A readable name from the file (the launcher shows its own library title when it knows the game)."""
    import re
    stem = Path(str(rom)).name
    for _ in range(2):
        stem = stem.rsplit('.', 1)[0] if '.' in stem else stem
    return re.sub(r'\s*[\(\[][^)\]]*[\)\]]', '', stem).strip() or stem


def album_root(data):
    return look_root(data) / 'screenshots'


def screens(data):
    value = read_fields(album_root(data) / 'settings.ini').get('screens', 'both')
    return value if value in SCREENS else 'both'


def set_screens(data, value):
    if value not in SCREENS:
        raise ValueError('Unknown screens choice')
    root = album_root(data)
    root.mkdir(parents=True, exist_ok=True)
    fields = read_fields(root / 'settings.ini')
    fields['screens'] = value
    write_fields(root / 'settings.ini', fields)


def compose(top, bottom, choice):
    """The picture to keep: both screens stacked, or the one asked for when it exists."""
    if choice == 'top' and top: return top
    if choice == 'bottom' and bottom: return bottom
    if top and bottom: return stack(top, bottom)
    return top or bottom


def thumbnail(picture):
    height = max(1, round(picture.height * THUMB_WIDTH / picture.width))
    return scale(picture, THUMB_WIDTH, min(height, THUMB_WIDTH * 2))


def save(data, top=None, bottom=None, combined=None, key=LAUNCHER, title='', system='', rom='', choice=None, now=None):
    """Save one screenshot from capture files; returns the PNG's path. Missing panels are skipped."""
    pictures = {}
    for name, path in (('top', top), ('bottom', bottom), ('combined', combined)):
        if path and Path(path).is_file():
            pictures[name] = read_bmp(path)
    if 'combined' in pictures:
        pictures['top'], pictures['bottom'] = split_combined(pictures.pop('combined'))
    picture = compose(pictures.get('top'), pictures.get('bottom'), choice or screens(data))
    if picture is None:
        raise ValueError('Nothing was captured')
    if not key or '/' in key or key.startswith('.'):
        raise ValueError('Bad album key')
    folder = album_root(data) / key
    folder.mkdir(parents=True, exist_ok=True)
    info = read_fields(folder / 'album.ini')
    fresh = {k: v for k, v in (('title', title), ('system', system), ('rom', str(rom) if rom else '')) if v}
    if any(info.get(k) != v for k, v in fresh.items()):
        info.update(fresh)
        write_fields(folder / 'album.ini', info)
    stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime(now))
    name, n = stamp, 1
    while (folder / (name + '.png')).exists():
        n += 1
        name = f'{stamp}-{n}'
    target = folder / (name + '.png')
    write_bmp(folder / (name + '.thumb.bmp'), thumbnail(picture))
    atomic_write(target, encode_png(picture))
    return target


def entries(data):
    """Every picture, newest first: (path, game key, title, system, date YYYYMMDD)."""
    root = album_root(data)
    result = []
    if not root.is_dir():
        return result
    for folder in root.iterdir():
        if not folder.is_dir() or folder.name.startswith('.'):
            continue
        info = read_fields(folder / 'album.ini')
        for path in folder.glob('*.png'):
            if not path.name.startswith('.'):
                result.append((path, folder.name, info.get('title', ''), info.get('system', ''), path.name[:8]))
    result.sort(key=lambda item: item[0].name, reverse=True)
    return result


def write_index(data, destination=None):
    """album.tsv for the launcher: path, key, title, system, date per line, newest first."""
    root = album_root(data)
    root.mkdir(parents=True, exist_ok=True)
    lines = [
        '\t'.join((str(path), key, title.replace('\t', ' '), system, date)) + '\n'
        for path, key, title, system, date in entries(data)
    ]
    target = Path(destination) if destination else root / 'album.tsv'
    atomic_write(target, ''.join(lines).encode('utf-8'))
    return target


def inside_album(data, path):
    path = Path(path).resolve()
    root = album_root(data).resolve()
    if root not in path.parents or path.suffix != '.png':
        raise ValueError('Not an album picture')
    return path


def delete(data, path):
    path = inside_album(data, path)
    path.unlink(missing_ok=True)
    path.with_name(path.stem + '.thumb.bmp').unlink(missing_ok=True)
    folder = path.parent
    if not any(folder.glob('*.png')):
        for leftover in folder.iterdir():
            leftover.unlink(missing_ok=True)
        folder.rmdir()


def view(data, path, top_out, bottom_out):
    """A picture as the two panels' BMPs for the launcher's viewer: a stacked picture splits back into its screens."""
    picture = decode_png(inside_album(data, path).read_bytes())
    top, bottom = split_combined(picture) if picture.height >= picture.width * 3 // 2 else (picture, None)
    write_bmp(top_out, fit(top, 640, 480))
    if bottom is not None:
        write_bmp(bottom_out, fit(bottom, 640, 480))
    else:
        Path(bottom_out).unlink(missing_ok=True)


def fit(picture, width, height):
    if picture.width <= width and picture.height <= height:
        return picture
    ratio = min(width / picture.width, height / picture.height)
    return scale(picture, max(1, int(picture.width * ratio)), max(1, int(picture.height * ratio)))


def cover(data, path):
    """Make the picture's upper screen the game's box art: <data>/library/<key>/box.bmp, the file the launcher draws.
    The picture it replaces is kept once as box.before-screenshot.bmp."""
    path = inside_album(data, path)
    key = path.parent.name
    if key == LAUNCHER:
        raise ValueError('A menu picture has no game')
    picture = decode_png(path.read_bytes())
    top, _ = split_combined(picture) if picture.height >= picture.width * 3 // 2 else (picture, None)
    folder = device_root(data) / 'library' / key
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / 'box.bmp'
    backup = folder / 'box.before-screenshot.bmp'
    if target.is_file() and not backup.exists():
        shutil.copy2(target, backup)
    write_bmp(target, fit(top, 640, 480))
    return target


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=('save', 'index', 'delete', 'view', 'cover', 'screens'))
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--top')
    parser.add_argument('--bottom')
    parser.add_argument('--combined')
    parser.add_argument('--key', default=LAUNCHER)
    parser.add_argument('--title', default='')
    parser.add_argument('--system', default='')
    parser.add_argument('--rom', default='')
    parser.add_argument('--path')
    parser.add_argument('--value')
    parser.add_argument('--screens', choices=SCREENS)
    parser.add_argument('--out')
    args = parser.parse_args(argv)
    try:
        if args.command == 'save':
            print(
                save(
                    args.data, args.top, args.bottom, args.combined, args.key, args.title, args.system, args.rom,
                    args.screens
                )
            )
            write_index(args.data)
        elif args.command == 'index':
            print(write_index(args.data, args.out))
        elif args.command == 'delete':
            delete(args.data, args.path)
            write_index(args.data)
        elif args.command == 'view':
            view(args.data, args.path, args.top, args.bottom)
        elif args.command == 'cover':
            print(cover(args.data, args.path))
        else:
            if args.value: set_screens(args.data, args.value)
            print(screens(args.data))
    except (OSError, ValueError, zlib.error) as error:
        print('Screenshot: ' + str(error), file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
