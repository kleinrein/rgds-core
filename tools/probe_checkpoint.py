#!/usr/bin/env python3
"""Read the original RGDS probe's VBlank clock from a disposable checkpoint.

This is a benchmark reader, not a save converter. It never modifies the file.
The homebrew probe writes its own 64-bit VBlank count and inverse to DS RAM;
this measures emulated time independently of renderer/presentation callbacks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAGIC = b'RGDSFRAMEPROBE01'
DRASTIC_MAGIC = b'DraStic-SaveState---------------'
MAX_BYTES = 64 * 1024 * 1024
MASK = (1 << 64) - 1


def decode_checkpoint(data, limit=MAX_BYTES):
    if not data or len(data) > limit:
        raise ValueError('Checkpoint is empty or exceeds the benchmark size limit')
    if not data.startswith(DRASTIC_MAGIC):
        # DSperate and melonDS write uncompressed machine-state chunks. Other
        # formats are accepted only if the original probe marker validates.
        return 'raw', data
    if len(data) < 0x46:
        raise ValueError('Truncated DraStic checkpoint header')
    compressed = struct.unpack_from('<I', data, 0x40)[0]
    if compressed != len(data) - 0x44:
        raise ValueError('DraStic checkpoint compressed length differs')
    stream = zlib.decompressobj()
    try:
        decoded = stream.decompress(data[0x44:], limit + 1)
    except zlib.error as error:
        raise ValueError('DraStic checkpoint has invalid compressed data') from error
    if len(decoded) > limit or stream.unconsumed_tail:
        raise ValueError('Checkpoint expands beyond the benchmark size limit')
    if not stream.eof or stream.unused_data:
        raise ValueError('DraStic checkpoint is incomplete or has trailing data')
    return 'drastic-zlib', decoded


def extract_counter(decoded):
    matches = []
    position = 0
    while True:
        position = decoded.find(MAGIC, position)
        if position < 0:
            break
        if position + len(MAGIC) + 16 <= len(decoded):
            count, inverse = struct.unpack_from('<QQ', decoded, position + len(MAGIC))
            if count ^ inverse == MASK:
                matches.append((position, count))
        position += len(MAGIC)
    values = {value for _, value in matches}
    if not values:
        raise ValueError('No complete RGDS probe clock found in checkpoint')
    if len(values) != 1:
        raise ValueError('Conflicting RGDS probe clocks found in checkpoint')
    return dict(vblank_count=values.pop(), marker_offsets=[offset for offset, _ in matches])


def read_probe_checkpoint(path):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError('Checkpoint is missing or exceeds the benchmark size limit')
    data = path.read_bytes()
    encoding, decoded = decode_checkpoint(data)
    return dict(path=str(path), sha256=hashlib.sha256(data).hexdigest(),
                encoding=encoding, checkpoint_bytes=len(data), decoded_bytes=len(decoded),
                **extract_counter(decoded))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(read_probe_checkpoint(args.checkpoint), sort_keys=True))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
