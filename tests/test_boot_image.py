"""Boot branding changes precise FIT/RSCE fields and rejects unsafe inputs."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import boot_image as boot


def logo(colour=0x18):
    pixels = bytes([colour]) * (640 * 480 * 3)
    return (b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54) +
            struct.pack('<IiiHHIIiiII', 40, 640, -480, 1, 24, 0,
                        len(pixels), 0, 0, 0, 0) + pixels)


def checksum(data, algo):
    if algo == 'crc32':return struct.pack('>I', zlib.crc32(data) & 0xffffffff)
    return hashlib.new(algo, data).digest()


def rsce(hash_size=32, names=('rk-kernel.dtb', 'logo.bmp', 'logo_kernel.bmp')):
    data = bytearray(512 * (len(names) + 1))
    data[:16] = struct.pack('<4sHHBBBBI', b'RSCE', 0, 0, 1, 1, 1, 0, len(names))
    positions = {}
    for index, name in enumerate(names):
        payload = b'original-kernel-dtb' if name.endswith('.dtb') else logo(0x20 + index)
        at = (index + 1) * 512
        start = len(data)
        data[at:at + 4] = b'ENTR'
        data[at + 4:at + 4 + len(name)] = name.encode()
        if hash_size in (20, 32):
            hashed = checksum(payload, 'sha1' if hash_size == 20 else 'sha256')
            data[at + 224:at + 224 + hash_size] = hashed
        struct.pack_into('<III', data, at + 256, hash_size, start // 512, len(payload))
        data.extend(payload)
        data.extend(bytes((-len(data)) % 512))
        positions[name] = dict(entry=at, start=start, size=len(payload))
    return bytes(data), positions


def fit(payload=None, mode='data-position', algorithms=('sha256',),
        signed=False, extra_resource=False, hash_size=32, total_padding=0,
        names=('rk-kernel.dtb', 'logo.bmp', 'logo_kernel.bmp'), kernel_external=False,
        signature_template=None):
    if payload is None:payload, positions = rsce(hash_size, names)
    else:positions = {}
    strings = bytearray()
    names_at = {}
    tree = bytearray()
    props = {}
    nodes = []
    def word(value):tree.extend(struct.pack('>I', value))
    def begin(name):
        word(1);tree.extend(name.encode() + b'\0');tree.extend(bytes((-len(tree)) % 4))
        nodes.append(name)
    def end():word(2);nodes.pop()
    def prop(name, value):
        if name not in names_at:
            names_at[name] = len(strings);strings.extend(name.encode() + b'\0')
        word(3);word(len(value));word(names_at[name])
        props[('/' + '/'.join(nodes[1:]), name)] = len(tree)
        tree.extend(value);tree.extend(bytes((-len(tree)) % 4))
    begin('');prop('description', b'RG DS fixture\0');begin('images')
    begin('kernel');prop('type', b'kernel\0')
    if kernel_external:
        prop('data-position', bytes(4));prop('data-size', struct.pack('>I', len(payload)))
    else:
        prop('data', b'KERNEL-MUST-STAY-UNCHANGED')
    end()
    for name in ('resource', 'resource@2') if extra_resource else ('resource',):
        begin(name);prop('type', b'multi\0');prop('compression', b'none\0')
        if mode == 'data':prop('data', payload)
        else:
            prop(mode, bytes(4));prop('data-size', struct.pack('>I', len(payload)))
        for index, algo in enumerate(algorithms):
            begin('hash-' + str(index));prop('algo', algo.encode() + b'\0')
            prop('value', checksum(payload, algo));end()
        if signed == 'image':
            begin('signature-1');prop('algo', b'sha256,rsa2048\0');prop('value', bytes(256));end()
        end()
    end();begin('configurations');begin('conf')
    if signed and signed != 'image':
        begin('signature-1');prop('algo', b'sha256,rsa2048\0');prop('value', bytes(256));end()
    if signature_template is not None:
        begin('signature')
        for key, value in signature_template.items():prop(key, value)
        end()
    end();end();end();word(9)
    structure_at = 56
    strings_at = structure_at + len(tree)
    total = strings_at + len(strings) + total_padding
    data_at = (total + 3) & ~3
    for (path, name), position in props.items():
        if name == 'data-position':struct.pack_into('>I', tree, position, data_at)
        elif name == 'data-offset':struct.pack_into('>I', tree, position, 0)
    header = struct.pack('>10I', 0xd00dfeed, total, structure_at, strings_at,
                         40, 17, 16, 0, len(strings), len(tree))
    data = bytearray(header + bytes(16) + tree + strings + bytes(total_padding))
    if mode != 'data':
        data.extend(bytes(data_at - len(data)));data.extend(payload)
    data.extend(b'PARTITION-TAIL-MUST-STAY-UNCHANGED')
    return data, dict(props={key:structure_at + at for key, at in props.items()},
                      positions=positions, resource=data_at if mode != 'data' else
                      structure_at + props[('/images/resource', 'data')],
                      total=total, structure=structure_at, strings=strings_at)


def rehash(data, layout, algorithms=('sha256',)):
    at = layout['resource']
    size = struct.unpack_from('>I', data, layout['props'][('/images/resource', 'data-size')])[0]
    for index, algo in enumerate(algorithms):
        start = layout['props'][('/images/resource/hash-' + str(index), 'value')]
        hashed = checksum(data[at:at + size], algo)
        data[start:start + len(hashed)] = hashed


class BootImageTests(unittest.TestCase):
    def test_exact_unsigned_stock_signing_template_is_preserved(self):
        template = {'algo': b'sha256,rsa2048\0', 'padding': b'pss\0',
                    'key-name-hint': b'dev\0', 'sign-images': b'fdt\0kernel\0multi\0'}
        data, layout = fit(signature_template=template)
        patched, details = boot.patch(data, logo(0x66))
        for key, value in template.items():
            start = layout['props'][('/configurations/conf/signature', key)]
            self.assertEqual(patched[start:start + len(value)], value)
        self.assertNotEqual(details['image_sha256'], details['output_sha256'])
        # Even an empty value or incomplete signature output must be rejected.
        for key, value in (('value', b''), ('value', bytes(256)),
                           ('hashed-nodes', b'/\0'), ('hashed-strings', bytes(8))):
            candidate, _ = fit(signature_template=dict(template, **{key:value}))
            with self.assertRaisesRegex(ValueError, 'Signed FIT'):boot.inspect(candidate)
        for key in template:
            candidate, _ = fit(signature_template={k:v for k,v in template.items() if k != key})
            with self.assertRaisesRegex(ValueError, 'Signed FIT'):boot.inspect(candidate)

    def test_external_position_offset_and_inline_preserve_unrelated_bytes(self):
        replacement = logo(0x66)
        for mode in ('data-position', 'data-offset', 'data'):
            with self.subTest(mode=mode):
                data, _ = fit(mode=mode, total_padding=1)
                prepared, details = boot.patch(data, replacement)
                self.assertEqual(len(prepared), len(data))
                allowed = bytearray(len(data))
                for change in details['changes']:
                    allowed[change['start']:change['start'] + change['size']] = bytes([1]) * change['size']
                self.assertTrue(all(a == b or permitted for a, b, permitted in zip(data, prepared, allowed)))
                self.assertEqual(prepared[-31:], data[-31:])
                for entry in details['entries']:
                    if entry['name'] in boot.LOGOS:
                        start = details['resource']['start'] + entry['start']
                        self.assertEqual(prepared[start:start + len(replacement)], replacement)
                self.assertEqual(boot.inspect(prepared)['image_sha256'], details['output_sha256'])
                self.assertEqual(boot.patch(prepared, replacement)[0], prepared)

    def test_hash_algorithms_are_updated_at_specific_properties(self):
        algorithms = ('sha1', 'sha256', 'sha384', 'sha512', 'md5', 'crc32')
        for entry_hash in (0, 20, 32):
            with self.subTest(entry_hash=entry_hash):
                data, layout = fit(algorithms=algorithms, hash_size=entry_hash)
                prepared, details = boot.patch(data, logo(0x77))
                self.assertEqual([h['algorithm'] for h in details['hashes']], list(algorithms))
                self.assertNotEqual(prepared, data)
                boot.inspect(prepared)

    def test_smaller_logo_cannot_exceed_existing_logical_slot(self):
        data, layout = fit(hash_size=0)
        entry = layout['resource'] + layout['positions']['logo.bmp']['entry']
        struct.pack_into('<I', data, entry + 264, 500000)
        rehash(data, layout)
        with self.assertRaisesRegex(ValueError, 'exceeds original slot'):
            boot.patch(data, logo())

    def test_signed_configuration_and_image_nodes_are_rejected(self):
        for signed in (True, 'image'):
            data, _ = fit(signed=signed)
            with self.assertRaisesRegex(ValueError, 'Signed FIT'):boot.inspect(data)
        data, layout = fit()
        # A signature marker stored structurally, not arbitrary payload text.
        needle = b'hash-0\0';at = data.find(needle, layout['structure'], layout['strings'])
        data[at:at + len(needle)] = b'cipher\0'
        with self.assertRaisesRegex(ValueError, 'Encrypted FIT'):boot.inspect(data)

    def test_avb_footer_is_rejected(self):
        data, _ = fit();data.extend(b'AVBf' + bytes(60))
        with self.assertRaisesRegex(ValueError, 'AVB signed'):boot.inspect(data)

    def test_ambiguous_resources_and_missing_resource_are_rejected(self):
        data, _ = fit(extra_resource=True)
        with self.assertRaisesRegex(ValueError, 'exactly one'):boot.inspect(data)
        data, layout = fit();data[layout['resource']:layout['resource'] + 4] = b'NOPE'
        with self.assertRaisesRegex(ValueError, 'does not contain RSCE'):boot.inspect(data)

    def test_corrupt_fit_digest_and_rsce_digest_are_rejected(self):
        data, layout = fit();at = layout['props'][('/images/resource/hash-0', 'value')]
        data[at] ^= 1
        with self.assertRaisesRegex(ValueError, 'Original FIT resource hash mismatch'):boot.inspect(data)
        data, layout = fit();at = layout['resource'] + layout['positions']['logo.bmp']['start'] + 100
        data[at] ^= 1;rehash(data, layout)
        with self.assertRaisesRegex(ValueError, 'Original RSCE hash mismatch'):boot.inspect(data)

    def test_unsupported_hash_sizes_and_algorithms_are_rejected(self):
        data, layout = fit(hash_size=16)
        with self.assertRaisesRegex(ValueError, 'Unsupported RSCE entry hash'):boot.inspect(data)
        data, layout = fit();at = layout['props'][('/images/resource/hash-0', 'algo')]
        data[at:at + 6] = b'blake2'
        with self.assertRaisesRegex(ValueError, 'Unsupported FIT hash'):boot.inspect(data)

    def test_resource_needs_hash_and_uncompressed_data(self):
        data, _ = fit(algorithms=())
        with self.assertRaisesRegex(ValueError, 'no verifiable hash'):boot.inspect(data)
        data, layout = fit();at = layout['props'][('/images/resource', 'compression')]
        data[at:at + 5] = b'gzip\0'
        with self.assertRaisesRegex(ValueError, 'Compressed FIT'):boot.inspect(data)

    def test_rsce_duplicate_missing_and_overlap_entries_are_rejected(self):
        for names, message in ((('logo.bmp', 'logo.bmp', 'logo_kernel.bmp'), 'Duplicate'),
                               (('rk-kernel.dtb', 'logo.bmp'), 'must contain')):
            data, _ = fit(names=names)
            with self.assertRaisesRegex(ValueError, message):boot.inspect(data)
        data, layout = fit(hash_size=0)
        first = layout['positions']['logo.bmp'];second = layout['positions']['logo_kernel.bmp']
        struct.pack_into('<I', data, layout['resource'] + second['entry'] + 260, first['start'] // 512)
        rehash(data, layout)
        with self.assertRaisesRegex(ValueError, 'allocations overlap'):boot.inspect(data)

    def test_rsce_entry_out_of_bounds_and_table_overlap_are_rejected(self):
        for offset in (0, 0xffffffff):
            data, layout = fit(hash_size=0)
            struct.pack_into('<I', data, layout['resource'] + 512 + 260, offset)
            rehash(data, layout)
            with self.assertRaises(ValueError):boot.inspect(data)

    def test_unsupported_rsce_header_layout_is_rejected(self):
        for offset, value in ((4, 1), (8, 2), (9, 2), (10, 2)):
            data, layout = fit();data[layout['resource'] + offset] = value;rehash(data, layout)
            with self.assertRaisesRegex(ValueError, 'Unsupported RSCE'):boot.inspect(data)

    def test_fdt_bounds_versions_tokens_and_structure_are_rejected(self):
        data, layout = fit()
        for at, value in ((0, 0), (4, len(data) + 1), (8, len(data)), (20, 16),
                          (layout['structure'], 100)):
            altered = bytearray(data);struct.pack_into('>I', altered, at, value)
            with self.assertRaises(ValueError):boot.inspect(altered)
        with self.assertRaisesRegex(ValueError, 'header'):boot.inspect(b'\xd0\x0d\xfe\xed')

    def test_external_data_cannot_overlap_tree_or_overrun_file(self):
        for position in (0, 4, 0xffffffff):
            data, layout = fit();struct.pack_into('>I', data, layout['props'][('/images/resource', 'data-position')], position)
            with self.assertRaises(ValueError):boot.inspect(data)
        data, layout = fit();struct.pack_into('>I', data, layout['props'][('/images/resource', 'data-size')], 0xffffffff)
        with self.assertRaisesRegex(ValueError, 'out of bounds'):boot.inspect(data)

    def test_external_resource_cannot_share_or_overlap_another_image(self):
        data, _ = fit(kernel_external=True)
        with self.assertRaisesRegex(ValueError, 'overlaps another image'):boot.inspect(data)

    def test_resource_image_type_can_identify_a_differently_named_rsce(self):
        data, layout = fit()
        at = data.find(b'resource\0', layout['structure'], layout['strings'])
        data[at:at + 9] = b'graphics\0'
        self.assertEqual(boot.inspect(data)['resource']['path'], '/images/graphics')

    def test_string_table_and_reservation_map_cannot_overlap_structure(self):
        data, layout = fit()
        for field in (12, 16):
            altered = bytearray(data)
            struct.pack_into('>I', altered, field, layout['structure'])
            with self.assertRaisesRegex(ValueError, 'overlap'):boot.inspect(altered)

    def test_fdt_duplicate_nodes_and_properties_are_rejected(self):
        data, layout = fit(extra_resource=True)
        at = data.find(b'resource@2\0', layout['structure'], layout['strings'])
        data[at:at + 11] = b'resource\0\0\0'
        with self.assertRaisesRegex(ValueError, 'Duplicate FDT node'):boot.inspect(data)
        data, layout = fit()
        type_at = data.find(b'type\0', layout['strings']) - layout['strings']
        compression = layout['props'][('/images/resource', 'compression')]
        struct.pack_into('>I', data, compression - 4, type_at)
        with self.assertRaisesRegex(ValueError, 'Duplicate or empty FDT property'):boot.inspect(data)

    def test_conflicting_inline_external_addresses_are_rejected(self):
        data, layout = fit()
        compression = layout['props'][('/images/resource', 'compression')]
        data_name = data.find(b'data\0', layout['strings']) - layout['strings']
        struct.pack_into('>I', data, compression - 4, data_name)
        with self.assertRaisesRegex(ValueError, 'Ambiguous or shared'):boot.inspect(data)

    def test_replacement_must_be_exact_supported_bmp_format(self):
        good = logo()
        for at, encoding, value in ((18, '<i', 480), (22, '<i', 480),
                                    (28, '<H', 32), (30, '<I', 1), (10, '<I', 100)):
            changed = bytearray(good);struct.pack_into(encoding, changed, at, value)
            with self.assertRaises(ValueError):boot.validate_logo(changed)
        with self.assertRaises(ValueError):boot.validate_logo(good[:-1])
        boot.validate_logo(good)

    def test_cli_never_overwrites_original_or_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder);image = folder / 'original.fit';art = folder / 'logo.bmp';output = folder / 'prepared.fit'
            data, _ = fit();image.write_bytes(data);art.write_bytes(logo(0x55))
            command = [sys.executable, '-B', str(ROOT / 'tools/boot_image.py'), 'prepare',
                       '--image', str(image), '--logo', str(art), '--output', str(output)]
            result = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['output_sha256'], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(image.read_bytes(), data)
            result = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(result.returncode, 1);self.assertIn('new regular file', result.stderr)
            command[-1] = str(image)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            self.assertEqual(image.read_bytes(), data)


if __name__ == '__main__':
    unittest.main()
