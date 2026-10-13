"""Generate a fallback save-chip entry without copying a stock game database."""
import hashlib
import json
import os
from pathlib import Path
import zlib

from nnddss_assets import write_changed

LEGACY_PREFIX = b'<?xml version="1.0" encoding="utf-8"?>\n<!-- RGDS generated save-type fallback v1 -->\n'
PREFIX = b"<?xml version='1.0' encoding='UTF-8'?>\n<database>\n  <cartridge title='RGDS save type "
SIZES = (0, 512, 8192, 65536, 131072, 262144, 524288, 1048576)


def _generated(content):
    return content.startswith((PREFIX, LEGACY_PREFIX))


def _crc32(rom, root):
    # This core's restricted parser requires CRC32 even for code/title lookup.
    # Hash a changed cartridge once, then reuse it across game switches.
    key = hashlib.sha256(os.fsencode(str(rom.absolute()))).hexdigest()
    cache = root / 'xdg-cache/save-types' / (key + '.json')
    stamp = lambda info: [info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns]
    with rom.open('rb') as source:
        before = stamp(os.fstat(source.fileno()))
        try:
            stored = json.loads(cache.read_text())
            if stored['stat'] == before and isinstance(stored['crc32'], int) and 0 <= stored['crc32'] <= 0xffffffff:
                return stored['crc32']
        except (OSError, ValueError, KeyError, TypeError):
            pass
        crc = 0
        for chunk in iter(lambda: source.read(256 * 1024), b''):
            crc = zlib.crc32(chunk, crc)
        if stamp(os.fstat(source.fileno())) != before:
            raise ValueError('Game changed while preparing its save-chip definition; retry launch.')
    write_changed(cache, (json.dumps(dict(stat=before, crc32=crc)) + '\n').encode())
    return crc


def prepare(app, data, root, rom):
    """Keep custom databases and saves untouched; fill a missing chip definition.

    The XML names the current ROM's exact header title and code. A generated
    fallback can be replaced for another ROM; a user's database always wins.
    Returns the generated save type, or None when no supported entry applies.
    """
    rom = Path(rom)
    if not rom.is_file():
        return None  # Host lifecycle adapters may have no cartridge payload.
    with rom.open('rb') as stream:
        header = stream.read(16)
    if (len(header) != 16 or header[12:16] == b'####'
            or any(value < 32 or value > 126 for value in header[12:16])):
        return None
    code = f'{int.from_bytes(header[12:16], "little"):08x}'
    title = header[:12].split(b'\0', 1)[0]
    if not title or any(value < 32 or value > 126 for value in title):
        return None
    target = root / 'User/game_database.xml'
    if target.is_symlink():
        return None
    explicit = data / 'bios/nnddss/game_database.xml'
    if explicit.is_file():
        # The user explicitly chose this file; refresh a previous generated
        # fallback, but never replace an independently edited runtime database.
        if not target.exists() or _generated(target.read_bytes()):
            write_changed(target, explicit.read_bytes())
        return None
    if target.exists() and not _generated(target.read_bytes()):
        return None
    table = json.loads((app / 'assets/nnddss/save-types/types.json').read_text())
    if table.get('schema') != 1 or not isinstance(table.get('entries'), dict):
        raise ValueError('NNDDSS save-type database is invalid; reinstall RGDS.')
    entry = table['entries'].get(code)
    if not entry or len(entry) != 2 or not isinstance(entry[1], int) or not 0 <= entry[1] < len(SIZES):
        return None  # NAND/unknown require game-specific support, not a guess.
    rom_size, kind = entry
    crc = _crc32(rom, root)
    quote = lambda value: value.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace("'", '&apos;').replace('"', '&quot;')
    # DraStic's line-based reader is not a general XML parser: it requires this
    # declaration, single-quoted attributes, no comments, and lines <256 bytes.
    content = PREFIX.decode() + quote(header[12:16].decode('ascii')) + "'>\n    <slot1>\n"
    content += f"      <rom name='rom' size='0x{rom_size:08x}' crc32='{crc:08x}' id='{code}' title='{quote(title.decode('ascii'))}' />\n"
    if kind:
        chip = 'EEPROM' if kind <= 4 else 'Flash'
        content += f"      <save name='save' size='0x{SIZES[kind]:x}' type='{chip}' />\n"
    content += '    </slot1>\n  </cartridge>\n</database>\n'
    write_changed(target, content.encode())
    return dict(code=code, bytes=SIZES[kind], chip='none' if not kind else 'EEPROM' if kind <= 4 else 'Flash')
