"""Owned NNDDSS runtime assets and a version-checked loader configuration patch.

Only two constant filenames change; all ELF layout and code bytes stay intact.
The supplied core/loader remains external binary software, not a source build.
"""
import hashlib
from pathlib import Path

LOADER_SHA256 = 'c18fe24ad236f0b8b2c47c45ab833487af8111da7f04751de9f09719988b4468'
LOADER_PATHS = ((b'/mnt/vendor/oem/board.ini', b'assets/board.ini'),
                (b'/mnt/vendor/oem/language.ini', b'assets/language.ini'))


def loader_bytes(binary):
    original = Path(binary).read_bytes()
    if original.startswith(b'#!'):
        return original  # Explicit host development adapters used by lifecycle tests.
    if hashlib.sha256(original).hexdigest() != LOADER_SHA256:
        raise ValueError('Unrecognized NNDDSS loader. Stock-independent paths need a reviewed loader build.')
    patched = original
    for source, target in LOADER_PATHS:
        source += b'\0'
        if patched.count(source) != 1 or len(target) >= len(source):
            raise ValueError('NNDDSS loader configuration path mismatch')
        patched = patched.replace(source, target.ljust(len(source), b'\0'))
    return patched


def write_changed(path, content, mode=None):
    if path.is_file() and path.read_bytes() == content:
        if mode is not None and (path.stat().st_mode & 0o777) != mode:
            path.chmod(mode)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + '.partial')
    try:
        pending.write_bytes(content)
        if mode is not None:
            pending.chmod(mode)
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def install(app, root, binary):
    bios = app / 'assets/nnddss/freebios'
    font = app / 'assets/fonts/source/Nunito.ttf'
    # Check all required inputs before replacing any runtime component.
    images = {}
    for arm, size in ((7, 0x4000), (9, 0x1000)):
        name = 'drastic_bios_arm' + str(arm) + '.bin'
        path = bios / name
        if not path.is_file() or path.stat().st_size != size:
            raise ValueError('Missing replacement NNDDSS BIOS: ' + str(path) + '. Reinstall the complete RGDS package.')
        images[name] = path.read_bytes()
    if not font.is_file() or not font.stat().st_size:
        raise ValueError('Missing bundled NNDDSS font: ' + str(font))
    executable = loader_bytes(binary)
    font_bytes = font.read_bytes()
    write_changed(root / 'nnddss', executable, 0o755)
    for name, content in images.items():
        for folder, filename in (('assets/bios', name), ('User/system', name),
                                 ('User/system', name.replace('drastic_', 'nds_'))):
            write_changed(root / folder / filename, content)
    for name in ('orbitron.ttf', 'SourceHanSansSC-Normal.ttf'):
        # NNDDSS's fixed filenames are compatibility aliases, not font identity.
        write_changed(root / 'assets/fonts' / name, font_bytes)
    # The reviewed wrapper requires this compatibility identifier to proceed.
    # Display geometry continues to come from SDL; this does not certify Plus support.
    write_changed(root / 'assets/board.ini', b'RGds\n')
    write_changed(root / 'assets/language.ini', b'1\n')
