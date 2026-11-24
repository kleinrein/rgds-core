"""DSiWare: which DS files need a DSi, where the game itself starts in them, and the console files DSCore needs.

DSCore runs DSiWare without a NAND (dscore/README.md, "Nintendo DSi"), but only on real BIOS dumps: the DS pair
(bios9, bios7) and the DSi pair (bios9i, bios7i). Nobody may ship those, so they come from the owner's BIOS folder.
A .cia wraps the game (an SRL: the same header a .nds has) behind its certificates, ticket and TMD; a .nds, .dsi or
.srl is the SRL itself. The header tests match DSCore's (dscore/src/core/io/dsi_title_install.cpp): DSiWare has the
DSi bit in its unit code and title ID high 00030004; a unit code of 2 (BlocksDS homebrew) also runs on a DS, and
DSCore starts those as DS games.
"""
import contextlib, os, zipfile
from pathlib import Path

SUFFIXES = ('.nds', '.dsi', '.srl', '.cia')
DSIWARE_HIGH = 0x00030004
CIA_HEADER = 0x2020
# The owner's BIOS folder on RG DS OS (rcS makes it; File sharing shares it as "bios"). RGDS_BIOS_DIR for tests.
BIOS_FOLDER = '/data/rgds/bios'
# The names other emulators give the dumps (melonDS, RetroArch's melonDS DS, DraStic, DSCore's own docs), and the
# size each must have: a file of another size under one of these names is something else.
BIOS_FILES = {
    'bios9': (('bios9.bin', 'biosnds9.bin', 'nds_bios_arm9.bin'), 4096),
    'bios7': (('bios7.bin', 'biosnds7.bin', 'nds_bios_arm7.bin'), 16384),
    'bios9i': (('dsi_bios9.bin', 'biosdsi9.bin', 'bios9i.bin', 'bios9dsi.bin'), 65536),
    'bios7i': (('dsi_bios7.bin', 'biosdsi7.bin', 'bios7i.bin', 'bios7dsi.bin'), 65536),
}
NEEDS_BIOS = 'DSiWare needs the DS and DSi BIOS files in the BIOS folder'
ENCRYPTED = 'This .cia is encrypted: copy the decrypted DSiWare file instead'


class Unreadable(ValueError):
    """A DSiWare file DSCore cannot start; its message is the player's own words."""


def _align(value):
    return (value + 0x3f) & ~0x3f


def srl_span(stream, size):
    """(offset, length) of the SRL in an open file of size bytes: all of it, or a CIA's one content."""
    stream.seek(0)
    head = stream.read(0x20)
    if len(head) < 0x20 or int.from_bytes(head[0:4], 'little') != CIA_HEADER:
        return 0, size
    word = lambda at: int.from_bytes(head[at:at + 4], 'little')
    tmd = _align(_align(_align(word(0)) + word(8)) + word(12))
    content = _align(tmd + word(16))
    length = int.from_bytes(head[24:32], 'little')
    stream.seek(tmd)
    signature = int.from_bytes(stream.read(4), 'big')
    # The content chunk record: right after a DSi TMD's header, or after a 3DS one's 64 content info records.
    record = tmd + 0x1e4 if signature == 0x00010001 else tmd + 0x204 + 0x900
    stream.seek(record)
    chunk = stream.read(0x10)
    if signature not in (0x00010001, 0x00010004) or len(chunk) < 0x10 or content + length > size:
        raise Unreadable('This .cia is not a DSiWare file')
    if int.from_bytes(chunk[6:8], 'big') & 1: raise Unreadable(ENCRYPTED)
    return content, min(length, int.from_bytes(chunk[8:16], 'big'))


@contextlib.contextmanager
def open_srl(path):
    """(stream, offset, length) of the game in path: loose, or the one DS file in a .zip. None when it has none."""
    path = Path(path)
    archive = None
    try:
        if path.suffix.lower() == '.zip':
            archive = zipfile.ZipFile(path)
            games = [i for i in archive.infolist() if not i.is_dir() and Path(i.filename).suffix.lower() in SUFFIXES]
            if len(games) != 1:
                yield None
                return
            stream, size = archive.open(games[0]), games[0].file_size
        elif path.suffix.lower() in SUFFIXES:
            stream, size = path.open('rb'), path.stat().st_size
        else:
            yield None
            return
        with stream:
            offset, length = srl_span(stream, size)
            yield stream, offset, length
    finally:
        if archive: archive.close()


def read_header(path, length=0x1000):
    """The first length bytes of the game's header, or None (no DS game, or too short). Raises Unreadable."""
    try:
        with open_srl(path) as found:
            if found is None: return None
            stream, offset, size = found
            stream.seek(offset)
            header = stream.read(min(length, size))
            return header if len(header) == length else None
    except (OSError, zipfile.BadZipFile, EOFError):
        return None


def kind(path):
    """'dsi' for DSiWare that needs a DSi, 'both' for DSiWare that also runs on a DS, None for a DS game (or not a
    game this reads). A .cia is always DSiWare; an encrypted one is still 'dsi', so starting it says why it cannot."""
    path = Path(path)
    try:
        header = read_header(path)
    except Unreadable:
        return 'dsi'
    if header is None or not header[0x12] & 2: return None
    if int.from_bytes(header[0x234:0x238], 'little') != DSIWARE_HIGH: return None  # a DSi-enhanced cartridge
    return 'both' if header[0x12] & 3 != 3 else 'dsi'


def game_code(path):
    """The four-letter game code (DSCore names DSiWare saves by it), or None."""
    try:
        header = read_header(path, 0x200)
    except Unreadable:
        return None
    if header is None: return None
    code = header[12:16].decode('ascii', errors='replace')
    return code if len(code) == 4 and all(32 < ord(c) < 127 and c not in '/\\' for c in code) else None


def bios_folder(env=None):
    return Path((env if env is not None else os.environ).get('RGDS_BIOS_DIR') or BIOS_FOLDER)


def console_files(folder):
    """{'bios9': path, ...} for each dump found in folder (or its nds or dsi subfolder), by name and size."""
    folder = Path(folder)
    found = {}
    for where in (folder, folder / 'nds', folder / 'dsi'):
        try:
            names = {p.name.lower(): p for p in where.iterdir() if p.is_file()}
        except OSError:
            continue
        for key, (candidates, size) in BIOS_FILES.items():
            if key in found: continue
            for name in candidates:
                path = names.get(name)
                try:
                    if path is not None and path.stat().st_size == size:
                        found[key] = path
                        break
                except OSError:
                    pass
    return found


def missing_files(folder):
    """The dumps DSiWare still needs, by key, in BIOS_FILES order."""
    found = console_files(folder)
    return [key for key in BIOS_FILES if key not in found]


def save_files(saves, code):
    """The files DSCore keeps a DSiWare title's saves in (dsi_nand_persist.cpp: GAMECODE.pub and GAMECODE.prv)."""
    return [Path(saves) / (code + suffix) for suffix in ('.pub', '.prv')] if code else []
