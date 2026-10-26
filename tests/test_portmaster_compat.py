"""The PortMaster compat layer: pinned Ubuntu packages unpacked into portmaster/rgds (runtime/portmaster_compat.py)."""
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import unittest.mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
sys.path.insert(0, str(ROOT / 'tools'))
import portmaster_compat
from portmaster_compat_manifest import version_key


def deb(files, links=()):
    """A minimal .deb: ar archive with debian-binary and a data.tar.xz holding files and symlinks."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w:xz') as tar:
        for path, data in files.items():
            info = tarfile.TarInfo('./' + path)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        for path, target in links:
            info = tarfile.TarInfo('./' + path)
            info.type = tarfile.SYMTYPE
            info.linkname = target
            tar.addfile(info)

    def member(name, body):
        header = name.ljust(16).encode() + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'100644'.ljust(8)
        header += str(len(body)).ljust(10).encode() + b'`\n'
        return header + body + (b'\n' if len(body) & 1 else b'')

    return b'!<arch>\n' + member('debian-binary', b'2.0\n') + member('data.tar.xz', raw.getvalue())


class CompatTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'portmaster'
        self.root.mkdir()
        self.package = deb(
            {
                'usr/lib/aarch64-linux-gnu/libjpeg.so.8.2.2': b'jpeg8',
                'bin/tar': b'gnu tar'
            }, [('usr/lib/aarch64-linux-gnu/libjpeg.so.8', 'libjpeg.so.8.2.2')]
        )
        gl = io.BytesIO()
        with zipfile.ZipFile(gl, 'w') as zf:
            zf.writestr('port/gl4es.aarch64/libGL.so.1', b'gl4es')
        self.zip = gl.getvalue()
        cache = self.root / '.download/compat'
        cache.mkdir(parents=True)
        # Already "downloaded": install() takes cached files whose SHA-256 matches and never touches the network.
        (cache / 'libjpeg-turbo8_1_arm64.deb').write_bytes(self.package)
        (cache / 'port.zip').write_bytes(self.zip)
        self.manifest = Path(temp.name) / 'compat.json'
        self.write_manifest()

    def write_manifest(self, sha=None):
        manifest = dict(
            schema=1,
            mirrors=['http://127.0.0.1:9/none'],
            wrappers={'egrep': '#!/bin/sh\nexec grep -E "$@"\n'},
            packages=[
                dict(
                    name='libjpeg-turbo8',
                    version='1',
                    filename='pool/main/l/libjpeg-turbo8_1_arm64.deb',
                    sha256=sha or hashlib.sha256(self.package).hexdigest(),
                    size=len(self.package),
                    files=[['usr/lib/aarch64-linux-gnu/libjpeg.so.8', 'lib/libjpeg.so.8'], ['bin/tar', 'bin/tar']]
                )
            ],
            extras=[
                dict(
                    zip='http://127.0.0.1:9/none/port.zip',
                    sha256=hashlib.sha256(self.zip).hexdigest(),
                    size=len(self.zip),
                    files=[['port/gl4es.aarch64/libGL.so.1', 'lib/libGL.so.1',
                            hashlib.sha256(b'gl4es').hexdigest()]]
                )
            ]
        )
        self.manifest.write_text(json.dumps(manifest))

    def test_install_follows_symlinks_and_lays_out_bin_and_lib(self):
        self.assertFalse(portmaster_compat.current(self.root, self.manifest))
        portmaster_compat.install(self.root, path=self.manifest)
        base = self.root / 'rgds'
        self.assertEqual((base / 'lib/libjpeg.so.8').read_bytes(), b'jpeg8')  # the link's target, copied (FAT)
        self.assertEqual((base / 'bin/tar').read_bytes(), b'gnu tar')
        self.assertEqual((base / 'lib/libGL.so.1').read_bytes(), b'gl4es')
        self.assertIn('grep -E', (base / 'bin/egrep').read_text())
        self.assertTrue(os.access(base / 'bin/tar', os.X_OK))
        self.assertTrue(portmaster_compat.current(self.root, self.manifest))
        self.assertFalse((self.root / '.download/compat').exists())  # the downloads are cleaned up

    def test_a_bad_download_keeps_the_previous_layer(self):
        portmaster_compat.install(self.root, path=self.manifest)
        cache = self.root / '.download/compat'
        cache.mkdir(parents=True)
        (cache / 'libjpeg-turbo8_1_arm64.deb').write_bytes(self.package)
        self.write_manifest(sha='0' * 64)  # nothing matches, and the mirror is unreachable
        with self.assertRaises(ValueError):
            portmaster_compat.install(self.root, path=self.manifest)
        self.assertEqual((self.root / 'rgds/bin/tar').read_bytes(), b'gnu tar')
        self.assertFalse((self.root / 'rgds.new').exists() and (self.root / 'rgds.new/compat.ini').exists())

    def test_environment_puts_tools_first_and_libraries_last(self):
        portmaster_compat.install(self.root, path=self.manifest)
        env = portmaster_compat.environment({'PATH': '/usr/bin:/bin', 'LD_LIBRARY_PATH': '/port/libs'}, self.root)
        self.assertTrue(env['PATH'].startswith(str(self.root / 'rgds/bin') + ':'))
        self.assertEqual(env['LD_LIBRARY_PATH'], '/port/libs:' + str(self.root / 'rgds/lib'))
        bare = portmaster_compat.environment({}, self.root)
        self.assertEqual(bare['LD_LIBRARY_PATH'], str(self.root / 'rgds/lib'))

    def test_shipped_manifest_is_pinned_and_never_shadows_glibc(self):
        manifest = json.loads((ROOT / 'runtime/portmaster_files/compat.json').read_text())
        targets = [target for package in manifest['packages'] for _source, target in package['files']]
        self.assertTrue(
            all(len(p['sha256']) == 64 and p['filename'].endswith('_arm64.deb') for p in manifest['packages'])
        )
        self.assertTrue(manifest['mirrors'][0].startswith('http://ports.ubuntu.com/'))
        for name in (
            'lib/libc.so.6', 'lib/libstdc++.so.6', 'lib/libgcc_s.so.1', 'lib/libSDL2-2.0.so.0', 'lib/libz.so.1'
        ):
            self.assertNotIn(name, targets)
        for tool in ('bin/tar', 'bin/grep', 'bin/sed', 'bin/gawk', 'bin/find', 'bin/unzip', 'bin/zstd'):
            self.assertIn(tool, targets)
        self.assertIn('lib/libjpeg.so.8', targets)
        self.assertEqual(len(targets), len(set(targets)))

    def test_no_shared_libgl_and_armhf_stays_in_its_folder(self):
        # A libGL.so.1 on every port's path changes what PortMaster's Weston runtime picks (it probes for one).
        for name in ('compat.json', 'compat-armhf.json'):
            manifest = json.loads((ROOT / 'runtime/portmaster_files' / name).read_text())
            targets = [t for p in manifest['packages'] for _s, t in p['files']]
            targets += [e.get('target', '') for e in manifest.get('extras', [])]
            self.assertFalse([t for t in targets if t.startswith('lib/libGL')], name)
            if name == 'compat-armhf.json':
                self.assertTrue(all(t.startswith('armhf/') for t in targets))
                self.assertIn('armhf/ld-linux-armhf.so.3', targets)
                self.assertIn('armhf/lib/libmali.so.1', targets)
                for source, target in manifest['local']:
                    self.assertTrue((ROOT / 'runtime/portmaster_files' / source).is_file(), source)

    def armhf_elf(self, interp=b'/lib/ld-linux-armhf.so.3'):
        """A minimal 32-bit ARM executable: ELF header, one PT_INTERP program header, the interpreter name."""
        import struct
        name = interp + b'\0'
        header = b'\x7fELF' + bytes([1, 1, 1]) + bytes(9)
        header += struct.pack('<HHIIIIIHHHHHH', 2, 40, 1, 0, 52, 0, 0x5000400, 52, 32, 1, 40, 0, 0)
        program = struct.pack('<IIIIIIII', 3, 84, 0, 0, len(name), len(name), 4, 1)
        return header + program + name + bytes(2000)

    def test_patch_interp_points_armhf_programs_at_our_loader(self):
        program = self.root / 'game.armhf'
        program.write_bytes(self.armhf_elf())
        self.assertTrue(portmaster_compat.patch_interp(program))
        data = program.read_bytes()
        self.assertIn(portmaster_compat.ARMHF_LOADER.encode() + b'\0', data)
        self.assertNotIn(b'/lib/ld-linux-armhf.so.3', data)
        self.assertFalse(portmaster_compat.patch_interp(program))  # already pointed: unchanged
        other = self.root / 'other'
        other.write_bytes(self.armhf_elf(b'/lib/ld-musl-armhf.so.1'))
        self.assertFalse(portmaster_compat.patch_interp(other))  # not glibc's loader: left alone
        text = self.root / 'notes.txt'
        text.write_bytes(b'not an ELF' * 200)
        self.assertFalse(portmaster_compat.patch_interp(text))
        self.assertLess(len(portmaster_compat.ARMHF_LOADER), len('/lib/ld-linux-armhf.so.3') + 1)

    def test_prepare_armhf_copies_the_loader_and_patches_the_port(self):
        loader = self.root / 'rgds/armhf/ld-linux-armhf.so.3'
        loader.parent.mkdir(parents=True)
        loader.write_bytes(b'loader')
        port = self.root / 'ports/game'
        (port / 'bin').mkdir(parents=True)
        (port / 'bin/game.armhf').write_bytes(self.armhf_elf())
        target = Path(tempfile.mkdtemp()) / 'ld.so'
        with unittest.mock.patch.object(portmaster_compat, 'ARMHF_LOADER', str(target)):
            self.assertEqual(portmaster_compat.prepare_armhf(self.root, port), 1)
            self.assertEqual(target.read_bytes(), b'loader')
        # The program names the fixed loader path (/tmp is where the copy lives on the handheld).
        self.assertIn(b'/tmp/rgds-ld-armhf.so.3\0', (port / 'bin/game.armhf').read_bytes())
        env = portmaster_compat.environment({}, self.root)
        self.assertNotIn('armhf', env.get('LD_LIBRARY_PATH', ''))  # no armhf/lib folder yet
        (self.root / 'rgds/armhf/lib').mkdir()
        (self.root / 'rgds/lib').mkdir(exist_ok=True)
        env = portmaster_compat.environment({}, self.root)
        self.assertTrue(env['LD_LIBRARY_PATH'].endswith('rgds/lib:' + str(self.root / 'rgds/armhf/lib')))

    def aarch64_elf(self, needed):
        """A minimal 64-bit ARM ELF: one PT_LOAD over the file, a PT_DYNAMIC with DT_NEEDED entries and DT_STRTAB."""
        import struct
        strtab = b'\0' + b''.join(name.encode() + b'\0' for name in needed)
        offsets, at = [], 1
        for name in needed:
            offsets.append(at)
            at += len(name) + 1
        dyn_off = 64 + 2 * 56
        dynamic = b''.join(struct.pack('<qQ', 1, o) for o in offsets)
        str_off = dyn_off + len(dynamic) + 32
        dynamic += struct.pack('<qQ', 5, str_off) + struct.pack('<qQ', 0, 0)
        size = str_off + len(strtab) + 1100
        header = b'\x7fELF' + bytes([2, 1, 1]) + bytes(9)
        header += struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, 64, 0, 0, 64, 56, 2, 64, 0, 0)
        load = struct.pack('<IIQQQQQQ', 1, 5, 0, 0, 0, size, size, 0x1000)
        dyn = struct.pack('<IIQQQQQQ', 2, 6, dyn_off, dyn_off, dyn_off, len(dynamic), len(dynamic), 8)
        return (header + load + dyn + dynamic + strtab).ljust(size, b'\0')

    def test_gl_ports_get_gl4es_and_others_do_not(self):
        port = self.root / 'ports/bugdom'
        port.mkdir(parents=True)
        (port / 'Bugdom').write_bytes(self.aarch64_elf(['libSDL2-2.0.so.0', 'libOpenGL.so.0', 'libc.so.6']))
        (port / 'data.pak').write_bytes(b'x' * 5000)
        self.assertTrue(portmaster_compat.elf_needs(port / 'Bugdom', portmaster_compat.GL_NAMES))
        self.assertTrue(portmaster_compat.gl_port(port, {}))
        self.assertFalse(portmaster_compat.gl_port(port, {'runtime': ['weston_pkg_0.2.squashfs']}))
        (port / 'gl4es.aarch64').mkdir()
        (port / 'gl4es.aarch64/libGL.so.1').write_bytes(b'its own gl4es')
        self.assertFalse(portmaster_compat.gl_port(port, {}))  # brings gl4es itself
        plain = self.root / 'ports/plain'
        plain.mkdir(parents=True)
        (plain / 'game').write_bytes(self.aarch64_elf(['libSDL2-2.0.so.0']))
        self.assertFalse(portmaster_compat.gl_port(plain, {}))
        self.assertTrue(portmaster_compat.gl_port(plain, {'runtime': ['renpy_8.3.4.squashfs']}))
        env = portmaster_compat.gl_environment({'LD_LIBRARY_PATH': '/a'}, self.root)
        self.assertEqual(env, {'LD_LIBRARY_PATH': '/a'})  # no gl4es installed: nothing changes
        gl4es = self.root / 'rgds/gl4es'
        gl4es.mkdir(parents=True)
        (gl4es / 'libGL.so.1').write_bytes(b'gl4es')
        env = portmaster_compat.gl_environment({'LD_LIBRARY_PATH': '/a', 'LIBGL_GL': '15'}, self.root)
        self.assertEqual(env['LD_LIBRARY_PATH'], '/a:' + str(gl4es))
        self.assertEqual((env['LIBGL_ES'], env['LIBGL_GL']), ('2', '15'))  # a port's own setting wins
        (gl4es / 'librgds-gl-attrs.so').write_bytes(b'cap')
        env = portmaster_compat.gl_environment({'LD_PRELOAD': '/x.so'}, self.root)
        self.assertEqual(env['LD_PRELOAD'], str(gl4es / 'librgds-gl-attrs.so') + ':/x.so')

    def test_fallback_packages_name_their_mirrors(self):
        manifest = json.loads((ROOT / 'runtime/portmaster_files/compat.json').read_text())
        fallback = [p for p in manifest['packages'] if 'mirrors' in p]
        self.assertTrue(fallback)
        for package in fallback:
            self.assertTrue(
                package['mirrors'][0].startswith(('http://ports.ubuntu.com/', 'http://archive.debian.org/'))
            )
        targets = {t for p in manifest['packages'] for _s, t in p['files']}
        self.assertIn('lib/libreadline.so.7', targets)  # bionic, for Wizznic and Yatka
        self.assertIn('gl4es/libGLU.so.1', targets)
        self.assertEqual(
            sorted(t for _s, t in manifest['local']),
            ['gl4es/libGLX.so.0', 'gl4es/libOpenGL.so.0', 'gl4es/librgds-gl-attrs.so']
        )

    def test_dpkg_version_order(self):
        ordered = ['1.0~rc1', '1.0', '1.0-1', '1.0-1ubuntu0.1', '1.0-2', '1.0a', '1:0.1']
        self.assertEqual(sorted(ordered, key=version_key), ordered)


if __name__ == '__main__':
    unittest.main()
