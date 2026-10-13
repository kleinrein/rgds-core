#!/usr/bin/env python3
"""Prepare an RG DS boot-logo image offline; never open a device for writing.

Only an unsigned FDT/FIT with an uncompressed Rockchip RSCE resource is
supported. Existing hashes are checked before modification, and image geometry,
FDT metadata and every non-logo resource remain byte-for-byte unchanged.

Format references:
https://fitspec.osfw.foundation/ (external data and hash nodes)
https://github.com/rockchip-linux/u-boot/blob/next-dev/common/image-fit.c
https://github.com/rockchip-linux/u-boot/blob/next-dev/tools/rockchip/resource_tool.c
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import zlib

BLOCK = 512
MAX_IMAGE = 128 * 1024 * 1024
LOGOS = ('logo.bmp', 'logo_kernel.bmp')


def fail(message):
    raise ValueError(message)


def bounds(start, size, limit, label):
    if start < 0 or size < 0 or start > limit or size > limit - start:
        fail(label + ' is truncated or out of bounds')


def align4(value):
    return (value + 3) & ~3


def overlap(a, b):
    return a[0] < b[0] + b[1] and b[0] < a[0] + a[1]


def ascii_string(data, label):
    if not data or data[-1:] != b'\0' or b'\0' in data[:-1]:
        fail(label + ' must be one terminated string')
    try:
        return data[:-1].decode('ascii')
    except UnicodeDecodeError:
        fail(label + ' is not ASCII')


def parse_fdt(image):
    """Return nodes and property byte positions without reconstructing the tree."""
    if len(image) > MAX_IMAGE:
        fail('Boot image exceeds 128 MiB limit')
    bounds(0, 40, len(image), 'FDT header')
    (magic, total, struct_at, strings_at, reserve_at, version, compatible,
     _cpu, strings_size, struct_size) = struct.unpack_from('>10I', image)
    if magic != 0xd00dfeed:
        fail('Boot image must begin with FDT/FIT magic')
    if version != 17 or compatible > 17 or compatible < 16:
        fail('Unsupported FDT version')
    bounds(0, total, len(image), 'FDT total size')
    if total < 40 or struct_at % 4 or struct_size % 4 or reserve_at % 8:
        fail('Invalid FDT alignment or total size')
    blocks = [(0, 40)]
    for start, size, label in ((struct_at, struct_size, 'FDT structure'),
                               (strings_at, strings_size, 'FDT strings')):
        bounds(start, size, total, label)
        if not size or any(overlap((start, size), other) for other in blocks):
            fail('FDT sections overlap or are empty')
        blocks.append((start, size))
    position = reserve_at
    while True:
        bounds(position, 16, total, 'FDT reservation map')
        if any(overlap((position, 16), other) for other in blocks):
            fail('FDT reservation map overlaps another section')
        address, size = struct.unpack_from('>QQ', image, position)
        position += 16
        if not address and not size:
            break
    strings = image[strings_at:strings_at + strings_size]
    nodes = {}
    stack = []
    position = struct_at
    end = struct_at + struct_size
    ended = False
    while position < end:
        bounds(position, 4, end, 'FDT token')
        token = struct.unpack_from('>I', image, position)[0]
        position += 4
        if token == 1:  # FDT_BEGIN_NODE
            nul = image.find(b'\0', position, end)
            if nul < 0:
                fail('FDT node name is not terminated')
            name = ascii_string(image[position:nul + 1], 'FDT node name')
            position = align4(nul + 1)
            if '/' in name or (not stack and name) or (stack and not name):
                fail('Invalid FDT node name')
            path = (stack[-1].rstrip('/') + '/' + name) if stack else '/'
            if path in nodes:
                fail('Duplicate FDT node: ' + path)
            nodes[path] = dict(name=name, parent=stack[-1] if stack else None,
                               properties={})
            stack.append(path)
        elif token == 2:  # FDT_END_NODE
            if not stack:
                fail('Unbalanced FDT node end')
            stack.pop()
        elif token == 3:  # FDT_PROP
            if not stack:
                fail('FDT property outside a node')
            bounds(position, 8, end, 'FDT property header')
            size, name_at = struct.unpack_from('>II', image, position)
            position += 8
            bounds(position, size, end, 'FDT property')
            if name_at >= len(strings):
                fail('FDT property name is out of bounds')
            nul = strings.find(b'\0', name_at)
            if nul < 0:
                fail('FDT property name is not terminated')
            name = ascii_string(strings[name_at:nul + 1], 'FDT property name')
            properties = nodes[stack[-1]]['properties']
            if not name or name in properties:
                fail('Duplicate or empty FDT property')
            properties[name] = (position, size)
            position = align4(position + size)
        elif token == 4:  # FDT_NOP
            pass
        elif token == 9:  # FDT_END
            if stack or '/' not in nodes:
                fail('Incomplete FDT tree')
            if any(image[position:end]):
                fail('Unexpected data after FDT end')
            ended = True
            break
        else:
            fail('Unsupported FDT token')
    if not ended or '/images' not in nodes:
        fail('Incomplete FIT images tree')
    for path, node in nodes.items():
        props = node['properties']
        # Stock ships an unpopulated mkimage signing template. A real signature
        # has a value; never strip it or accept a partially signed/unknown node.
        template = {'algo': b'sha256,rsa2048\0', 'padding': b'pss\0',
                    'key-name-hint': b'dev\0', 'sign-images': b'fdt\0kernel\0multi\0'}
        unsigned_template = (node['name'] == 'signature'
            and node['parent'] is not None
            and nodes[node['parent']]['parent'] == '/configurations'
            and set(props) == set(template)
            and all(image[props[k][0]:props[k][0] + props[k][1]] == v for k, v in template.items())
            and not any(child['parent'] == path for child in nodes.values()))
        if not unsigned_template and (node['name'].startswith('signature') or
                any(key in props for key in ('hashed-nodes', 'hashed-strings', 'key-name-hint'))):
            fail('Signed FIT images are not supported')
        if node['name'].startswith('cipher'):
            fail('Encrypted FIT images are not supported')
    if image[-64:-60] == b'AVBf':
        fail('AVB signed boot images are not supported')
    return total, nodes


def value(image, node, name):
    if name not in node['properties']:
        fail('Missing FIT property: ' + name)
    start, size = node['properties'][name]
    return image[start:start + size]


def cell(image, node, name):
    data = value(image, node, name)
    if len(data) != 4:
        fail('Unsupported FIT cell size: ' + name)
    return struct.unpack('>I', data)[0]


def image_range(image, node, total):
    props = node['properties']
    modes = [name for name in ('data', 'data-offset', 'data-position') if name in props]
    if len(modes) != 1 or 'image-data' in props:
        fail('Ambiguous or shared FIT image data')
    mode = modes[0]
    if mode == 'data':
        if 'data-size' in props:
            fail('Ambiguous inline FIT data size')
        start, size = props['data']
    else:
        size = cell(image, node, 'data-size')
        start = cell(image, node, mode)
        if mode == 'data-offset':
            start += align4(total)
        if start < align4(total):
            fail('External FIT data overlaps its tree')
    bounds(start, size, len(image), 'FIT image data')
    if not size:
        fail('Empty FIT image data')
    return start, size, mode


def digest(data, algo):
    if algo == 'crc32':
        return struct.pack('>I', zlib.crc32(data) & 0xffffffff)
    if algo not in ('md5', 'sha1', 'sha256', 'sha384', 'sha512'):
        fail('Unsupported FIT hash algorithm: ' + algo)
    return hashlib.new(algo, data).digest()


def parse_rsce(resource):
    bounds(0, BLOCK, len(resource), 'RSCE header')
    if resource[:4] != b'RSCE':
        fail('Resource image does not begin with RSCE')
    versions = struct.unpack_from('<HH', resource, 4)
    if versions != (0, 0) or resource[8:11] != b'\x01\x01\x01':
        fail('Unsupported RSCE version or block layout')
    count = struct.unpack_from('<I', resource, 12)[0]
    if not 1 <= count <= 256:
        fail('Invalid RSCE entry count')
    table_end = BLOCK * (count + 1)
    bounds(0, table_end, len(resource), 'RSCE entry table')
    entries = []
    names = set()
    for index in range(count):
        at = BLOCK * (index + 1)
        if resource[at:at + 4] != b'ENTR':
            fail('Invalid RSCE entry magic')
        raw = resource[at + 4:at + 224]
        nul = raw.find(b'\0')
        if nul < 0:
            fail('RSCE entry name is not terminated')
        name = ascii_string(raw[:nul + 1], 'RSCE entry name')
        if not name or name in names:
            fail('Duplicate or empty RSCE entry name')
        names.add(name)
        hash_size, offset, size = struct.unpack_from('<III', resource, at + 256)
        start = offset * BLOCK
        bounds(start, size, len(resource), 'RSCE entry data')
        if start < table_end or not size:
            fail('RSCE entry overlaps its table or is empty')
        extent = (start, ((size + BLOCK - 1) // BLOCK) * BLOCK)
        bounds(*extent, len(resource), 'RSCE entry allocation')
        if any(overlap(extent, item['extent']) for item in entries):
            fail('RSCE entry allocations overlap')
        if hash_size not in (0, 20, 32):
            fail('Unsupported RSCE entry hash size')
        if hash_size:
            algo = 'sha1' if hash_size == 20 else 'sha256'
            actual = digest(resource[start:start + size], algo)
            if resource[at + 224:at + 224 + hash_size] != actual:
                fail('Original RSCE hash mismatch: ' + name)
        entries.append(dict(name=name, start=start, size=size, extent=extent,
                            hash_start=at + 224, hash_size=hash_size))
    if any(name not in names for name in LOGOS):
        fail('RSCE must contain logo.bmp and logo_kernel.bmp')
    return entries


def validate_logo(logo):
    bounds(0, 54, len(logo), 'Logo BMP header')
    if logo[:2] != b'BM':
        fail('Logo must be a BMP')
    size, offset = struct.unpack_from('<I', logo, 2)[0], struct.unpack_from('<I', logo, 10)[0]
    header, width, height, planes, bpp, compression, pixels = struct.unpack_from('<IiiHHII', logo, 14)
    if (size != len(logo) or header != 40 or offset != 54 or width != 640 or
            height != -480 or planes != 1 or bpp != 24 or compression != 0 or
            pixels not in (0, 640 * 480 * 3) or len(logo) != 54 + 640 * 480 * 3):
        fail('Logo must be a 640x480 top-down 24-bit uncompressed BMP')


def inspect(image):
    """Validate an unsigned FIT and its resource hashes; return patch metadata."""
    total, nodes = parse_fdt(image)
    images = []
    candidates = []
    for path, node in nodes.items():
        if node['parent'] != '/images':
            continue
        start, size, mode = image_range(image, node, total)
        item = dict(path=path, start=start, size=size, mode=mode)
        images.append(item)
        name = node['name'].split('@', 1)[0].split('-', 1)[0]
        kind = ascii_string(value(image, node, 'type'), 'FIT image type')
        is_rsce = image[start:start + 4] == b'RSCE'
        if name == 'resource' or kind == 'resource':
            if not is_rsce:
                fail('Named FIT resource does not contain RSCE')
            candidates.append(item)
        elif kind == 'multi' and is_rsce:
            candidates.append(item)
    if len(candidates) != 1:
        fail('FIT must contain exactly one named RSCE resource')
    resource = candidates[0]
    node = nodes[resource['path']]
    if ascii_string(value(image, node, 'compression'), 'FIT compression') != 'none':
        fail('Compressed FIT resource images are not supported')
    for other in images:
        if other is not resource and overlap((resource['start'], resource['size']), (other['start'], other['size'])):
            fail('FIT resource overlaps another image')
    payload = image[resource['start']:resource['start'] + resource['size']]
    hashes = []
    for path, child in nodes.items():
        if child['parent'] != resource['path']:
            continue
        if not child['name'].startswith('hash'):
            fail('Unsupported FIT resource child node: ' + child['name'])
        algo = ascii_string(value(image, child, 'algo'), 'FIT hash algorithm')
        expected = digest(payload, algo)
        current = value(image, child, 'value')
        if current != expected:
            fail('Original FIT resource hash mismatch: ' + path)
        start, size = child['properties']['value']
        hashes.append(dict(path=path, algorithm=algo, start=start, size=size))
    if not hashes:
        fail('FIT resource has no verifiable hash')
    entries = parse_rsce(payload)
    return dict(schema=1, image_size=len(image), image_sha256=hashlib.sha256(image).hexdigest(),
                fit_size=total, resource=resource, hashes=hashes, entries=entries)


def patch(image, logo):
    """Return (patched bytes, review metadata), with no filesystem/device writes."""
    validate_logo(logo)
    details = inspect(image)
    result = bytearray(image)
    base = details['resource']['start']
    changes = []
    for entry in details['entries']:
        if entry['name'] not in LOGOS:
            continue
        if len(logo) > entry['size']:
            fail('Replacement logo exceeds original slot: ' + entry['name'])
        start, size = base + entry['start'], entry['size']
        result[start:start + size] = logo + bytes(size - len(logo))
        changes.append(dict(kind='logo', name=entry['name'], start=start, size=size))
        if entry['hash_size']:
            algo = 'sha1' if entry['hash_size'] == 20 else 'sha256'
            hash_at = base + entry['hash_start']
            result[hash_at:hash_at + entry['hash_size']] = digest(result[start:start + size], algo)
            changes.append(dict(kind='rsce_hash', name=entry['name'], start=hash_at, size=entry['hash_size']))
    resource = details['resource']
    payload = result[base:base + resource['size']]
    for item in details['hashes']:
        start, size = item['start'], item['size']
        result[start:start + size] = digest(payload, item['algorithm'])
        changes.append(dict(kind='fit_hash', name=item['path'], start=start, size=size))
    prepared = bytes(result)
    inspect(prepared)  # Verify the exact output, including all unchanged entries.
    details.update(output_sha256=hashlib.sha256(prepared).hexdigest(),
                   logo_sha256=hashlib.sha256(logo).hexdigest(), changes=changes)
    return prepared, details


def plan(image, logo):
    return patch(image, logo)[1]


def read_file(path, limit):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > limit:
        fail('Expected a bounded regular file: ' + str(path))
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        fail('Input file exceeds size limit')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    check = commands.add_parser('inspect');check.add_argument('--image', required=True)
    prepare = commands.add_parser('prepare')
    prepare.add_argument('--image', required=True)
    prepare.add_argument('--logo', required=True)
    prepare.add_argument('--output', required=True)
    args = parser.parse_args()
    try:
        original = read_file(args.image, MAX_IMAGE)
        if args.command == 'inspect':
            details = inspect(original)
        else:
            prepared, details = patch(original, read_file(args.logo, 1024 * 1024))
            output = Path(args.output)
            if output.resolve() in (Path(args.image).resolve(), Path(args.logo).resolve()) or output.exists():
                fail('Output must be a new regular file, separate from both inputs')
            descriptor, pending = tempfile.mkstemp(prefix='.rgds-boot-', dir=output.parent)
            try:
                with os.fdopen(descriptor, 'wb') as stream:
                    stream.write(prepared);stream.flush();os.fsync(stream.fileno())
                os.link(pending, output)  # Exclusive publication; never overwrite.
            finally:
                Path(pending).unlink(missing_ok=True)
        print(json.dumps(details, indent=2, sort_keys=True))
    except (OSError, ValueError) as error:
        parser.exit(1, 'boot_image: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
