#!/usr/bin/env python3
"""Validate packaged shader preview pixels and their source provenance.

Uses only Python's standard library. Packaging does not need Pillow, a graphics
context, an emulator or a device. The generated manifest is checked against the
current owned pipeline and the exact community .dsd files being packaged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
SCENES = ('grove', 'circuit', 'starfall')
sys.path.insert(0, str(ROOT))
from runtime.shaders import SHADERS

SHADER_FILES = tuple(entry['file'] + '.dsd' for entry in SHADERS[1:])
PRESET_COUNT = len(SHADERS)
HASH = re.compile(r'[0-9a-f]{64}\Z')
ENCODING = 'RGBA lossless 5x5 tile deduplication, RGSPV001'


def _invalid(label, reason):
    raise ValueError(f'Invalid shader previews ({label}): {reason}')


def _read(path, label, limit=8 * 1024 * 1024):
    try:
        if path.is_symlink() or not path.is_file(): _invalid(label, 'missing file or symbolic link')
        if path.stat().st_size > limit: _invalid(label, 'file exceeds the format size limit')
        with path.open("rb") as source:
            data = source.read(limit + 1)
        if len(data) > limit: _invalid(label, "file exceeds the format size limit")
        return data
    except OSError as error:
        _invalid(label, str(error))


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _hash(value, label):
    if not isinstance(value, str) or not HASH.fullmatch(value): _invalid(label, 'invalid SHA-256 field')
    return value


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: _invalid('manifest', 'duplicate JSON key ' + repr(key))
        result[key] = value
    return result


def _rgba_tiles(data, label):
    if len(data) < 16 or data[:8] != b'RGSPV001': _invalid(label, 'invalid packed image header')
    w, h, side, count = struct.unpack_from('<HHHH', data, 8)
    if (w, h) != (640, 480) or side != 5 or not count:
        _invalid(label, 'packed image dimensions or tile size are incorrect')
    tile_bytes = side * side * 4
    columns, rows = w // side, h // side
    indices_at = 16 + count * tile_bytes
    if indices_at + columns * rows * 2 != len(data):
        _invalid(label, 'packed image is truncated or contains trailing data')
    pixels = bytearray(w * h * 4)
    for index in range(columns * rows):
        tile = struct.unpack_from('<H', data, indices_at + index * 2)[0]
        if tile >= count: _invalid(label, 'tile index is outside the atlas')
        x, y = (index % columns) * side, (index // columns) * side
        begin = 16 + tile * tile_bytes
        for row in range(side):
            destination = ((y + row) * w + x) * 4
            source = begin + row * side * 4
            pixels[destination:destination + side * 4] = data[source:source + side * 4]
    return pixels


def _native_png(data, label):
    """Decode the generator's noninterlaced RGBA8 PNGs without Pillow."""
    if not data.startswith(b'\x89PNG\r\n\x1a\n'): _invalid(label, 'invalid PNG signature')
    position = 8
    header = False
    ended = False
    compressed = bytearray()
    while position < len(data):
        if position + 12 > len(data): _invalid(label, 'truncated PNG chunk')
        length = struct.unpack_from('>I', data, position)[0]
        kind = data[position + 4:position + 8]
        end = position + 12 + length
        if end > len(data): _invalid(label, 'truncated PNG payload')
        payload = data[position + 8:position + 8 + length]
        crc = struct.unpack_from('>I', data, position + 8 + length)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != crc: _invalid(label, 'PNG chunk checksum mismatch')
        if kind == b'IHDR':
            if header or position != 8 or length != 13: _invalid(label, 'invalid PNG header')
            w, h, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            if (w, h, depth, color, compression, filtering, interlace) != (256, 192, 8, 6, 0, 0, 0):
                _invalid(label, 'native PNG must be 256x192 RGBA8 without interlacing')
            header = True
        elif kind == b'IDAT':
            if not header or ended: _invalid(label, 'invalid PNG data order')
            compressed.extend(payload)
        elif kind == b'IEND':
            if not header or not compressed or length or end != len(data):
                _invalid(label, 'invalid PNG end or trailing data')
            ended = True
        elif not kind[0] & 32:
            _invalid(label, 'unsupported critical PNG chunk')
        position = end
    if not header or not ended: _invalid(label, 'missing PNG header or end')
    expected = (256 * 4 + 1) * 192
    try:
        inflater = zlib.decompressobj()
        decoded = inflater.decompress(compressed, expected + 1)
    except zlib.error as error:
        _invalid(label, 'invalid PNG compression: ' + str(error))
    if len(decoded) != expected or not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
        _invalid(label, 'PNG decompressed size or stream is incorrect')
    pixels = bytearray()
    previous = bytearray(256 * 4)
    position = 0
    for _ in range(192):
        filtering = decoded[position]
        position += 1
        row = bytearray(decoded[position:position + 256 * 4])
        position += 256 * 4
        if filtering > 4: _invalid(label, 'unsupported PNG row filter')
        for x in range(len(row)):
            left = row[x - 4] if x >= 4 else 0
            above = previous[x]
            upper_left = previous[x - 4] if x >= 4 else 0
            if filtering == 1: predictor = left
            elif filtering == 2: predictor = above
            elif filtering == 3: predictor = (left + above) // 2
            elif filtering == 4:
                estimate = left + above - upper_left
                pa, pb, pc = abs(estimate - left), abs(estimate - above), abs(estimate - upper_left)
                predictor = left if pa <= pb and pa <= pc else above if pb <= pc else upper_left
            else:
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        pixels.extend(row)
        previous = row
    return pixels


def validate_shader_previews(folder, shader_folder, pipeline=ROOT / 'src/platform/game_shader.hpp'):
    """Return the verified manifest, or raise ValueError on any mismatch.

    ``folder`` is the preview asset tree, ``shader_folder`` contains the actual
    .dsd files being packaged, and ``pipeline`` is the owned native draw source.
    The function never imports optional image/graphics dependencies.
    """
    folder, shader_folder, pipeline = Path(folder), Path(shader_folder), Path(pipeline)
    raw = _read(folder / 'manifest.json', 'manifest', 1024 * 1024)
    try:
        manifest = json.loads(raw, object_pairs_hook=_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        _invalid('manifest', str(error))
    if not isinstance(manifest, dict) or type(manifest.get('schema')) is not int or manifest['schema'] != 1:
        _invalid('manifest', 'unsupported schema')
    if any(
        not isinstance(manifest.get(key), list) or len(manifest[key]) != 2
        or any(type(value) is not int for value in manifest[key]) for key in ("native_size", "preview_size")
    ) or manifest.get("native_size") != [256, 192] or manifest.get("preview_size") != [
        640, 480
    ] or manifest.get("encoding") != ENCODING:
        _invalid("manifest", "unsupported dimensions or lossless encoding")
    if _sha(_read(pipeline, 'pipeline')) != _hash(manifest.get('pipeline_sha256'), 'pipeline'):
        _invalid('pipeline', 'source changed; regenerate previews from the current GameShader')
    shaders = manifest.get('shaders')
    if not isinstance(shaders, dict) or set(shaders) != set(SHADER_FILES):
        _invalid('shaders', 'expected the exact preset file mappings')
    for name in SHADER_FILES:
        if _sha(
            _read(
                shader_folder / name if (shader_folder / name).exists() else ROOT / 'assets/shaders' / name,
                'shader ' + name
            )
        ) != _hash(shaders[name], 'shader ' + name):
            _invalid('shader ' + name, 'source changed; regenerate previews from the packaged shader files')
    renderer = manifest.get('renderer')
    if not isinstance(renderer, dict) or any(
        not isinstance(renderer.get(key), str) or not renderer[key] for key in ('vendor', 'renderer', 'version')
    ) or 'OpenGL ES' not in renderer['version']:
        _invalid('renderer', 'missing GLES rendering provenance')
    scenes = manifest.get('scenes')
    if not isinstance(scenes, list) or len(scenes) != 3 or any(not isinstance(scene, dict) for scene in scenes
                                                               ) or [scene.get('id')
                                                                     for scene in scenes] != list(SCENES):
        _invalid('scenes', 'expected grove, circuit and starfall in order')
    expected_files = {'manifest.json'}
    total = 0
    for scene in scenes:
        name = scene['id']
        source = name + '/source.png'
        expected_files.add(source)
        if _sha(_native_png(_read(folder / source, source), source)) != _hash(scene.get('native_sha256'), source):
            _invalid(source, 'native scene pixel hash mismatch')
        presets = scene.get('presets')
        if not isinstance(presets, list) or len(presets) != PRESET_COUNT or any(
            not isinstance(entry, dict) or type(entry.get('id')) is not int for entry in presets
        ) or [entry['id'] for entry in presets] != list(range(PRESET_COUNT)):
            _invalid(name, 'expected every preset ID in order')
        hashes = set()
        for preset in presets:
            relative = f'{name}/shader-{preset["id"]}.rgsp'
            expected_files.add(relative)
            data = _read(folder / relative, relative)
            if type(preset.get('bytes')) is not int or preset['bytes'] != len(data):
                _invalid(relative, 'packed byte count mismatch')
            if _sha(data) != _hash(preset.get('sha256'), relative): _invalid(relative, 'packed file hash mismatch')
            digest = _sha(_rgba_tiles(data, relative))
            if digest != _hash(preset.get('rgba_sha256'), relative): _invalid(relative, 'decoded pixel hash mismatch')
            hashes.add(digest)
            total += len(data)
        if len(hashes) != PRESET_COUNT: _invalid(name, 'presets must show distinct reference images')
    if type(manifest.get('packed_bytes')) is not int or manifest['packed_bytes'] != total:
        _invalid('manifest', 'packed byte total mismatch')
    if type(manifest.get('uncompressed_rgba_bytes')
            ) is not int or manifest['uncompressed_rgba_bytes'] != 3 * PRESET_COUNT * 640 * 480 * 4:
        _invalid('manifest', 'uncompressed byte total mismatch')
    actual_files = set()
    try:
        for path in folder.rglob('*'):
            relative = path.relative_to(folder).as_posix()
            if path.is_symlink(): _invalid(relative, 'symbolic links are not preview assets')
            if path.is_file(): actual_files.add(relative)
            elif path.is_dir() and relative not in SCENES: _invalid(relative, 'unexpected asset directory')
            elif not path.is_dir(): _invalid(relative, 'unsupported asset entry')
    except OSError as error:
        _invalid('inventory', str(error))
    if actual_files != expected_files:
        missing = sorted(expected_files - actual_files)
        extra = sorted(actual_files - expected_files)
        _invalid('inventory', f'missing={missing}, unexpected={extra}')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path, nargs='?', default=ROOT / 'assets/shader-previews')
    parser.add_argument('--shader-folder', type=Path, default=ROOT / 'vendor/engines/shaders')
    parser.add_argument('--pipeline', type=Path, default=ROOT / 'src/platform/game_shader.hpp')
    args = parser.parse_args()
    try:
        manifest = validate_shader_previews(args.folder, args.shader_folder, args.pipeline)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(
        f'Validated {3*PRESET_COUNT} exact GLES previews in {manifest["packed_bytes"]} bytes against {len(SHADER_FILES)} shader sources.'
    )
    return 0


if __name__ == '__main__': sys.exit(main())
