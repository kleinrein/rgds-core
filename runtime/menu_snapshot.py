"""Bounded, one-shot paused-game backdrop preparation; no imaging dependencies."""
from pathlib import Path
import struct


def top_panel(source, destination):
    """Accept SDL's uncompressed 24/32-bit BMP and crop a dual-panel frame."""
    source, destination = Path(source), Path(destination)
    if source.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('Capture exceeds budget')
    data = source.read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('Invalid capture')
    offset = struct.unpack_from('<I', data, 10)[0]
    header, width, height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    # SDL writes BI_BITFIELDS with canonical BGRA masks for RGBA32 surfaces.
    if header < 40 or planes != 1 or bits not in (24, 32) or compression not in (0, 3) or not 0 < width <= 2560 or not 0 < abs(height) <= 1920:
        raise ValueError('Unsupported capture')
    if compression == 3 and (bits != 32 or len(data) < 66 or struct.unpack_from('<III', data, 54) != (0xff0000, 0xff00, 0xff)):
        raise ValueError('Unsupported capture masks')
    h = abs(height); stride = (width * bits // 8 + 3) & ~3
    if offset < 14 + header or len(data) < offset + stride * h:
        raise ValueError('Truncated capture')
    crop_w = width // 2 if width >= h * 2 else width
    crop_h = h // 2 if h >= width * 3 // 2 else h
    # Bound saved texture size without per-frame work. Nearest sampling preserves pixels.
    out_w, out_h = min(640, crop_w), min(480, crop_h)
    out_stride = (out_w * 3 + 3) & ~3
    pixels = bytearray(out_stride * out_h)
    for y in range(out_h):
        sy = y * crop_h // out_h
        row = offset + (sy if height < 0 else h - 1 - sy) * stride
        if crop_w == out_w:
            target = y * out_stride
            if bits == 24:
                pixels[target:target+out_w*3] = data[row:row+out_w*3]
            else:
                # Normal 640px captures need only three native byte slices,
                # avoiding hundreds of thousands of Python pixel iterations.
                for channel in range(3):
                    pixels[target+channel:target+out_w*3:3] = data[row+channel:row+out_w*4:4]
            continue
        for x in range(out_w):
            start = row + x * crop_w // out_w * (bits // 8)
            target = y * out_stride + x * 3
            pixels[target:target+3] = data[start:start+3]
    result = struct.pack('<2sIHHI', b'BM', 54+len(pixels), 0, 0, 54)
    result += struct.pack('<IiiHHIIiiII',40,out_w,-out_h,1,24,0,len(pixels),0,0,0,0)
    pending = destination.with_suffix('.pending')
    pending.write_bytes(result+pixels);pending.replace(destination)
