import struct
import unittest
import zlib

from tools.probe_checkpoint import DRASTIC_MAGIC, MAGIC, MASK, decode_checkpoint, extract_counter


class ProbeCheckpointTests(unittest.TestCase):
    def marker(self, count):
        return MAGIC + struct.pack('<QQ', count, count ^ MASK)

    def checkpoint(self, data):
        compressed = zlib.compress(data)
        header = bytearray(0x44)
        header[:len(DRASTIC_MAGIC)] = DRASTIC_MAGIC
        struct.pack_into('<I', header, 0x40, len(compressed))
        return bytes(header) + compressed

    def test_both_raw_and_drastic_clocks_are_independent_of_display_counts(self):
        raw = b'other state data' + self.marker(0x100000123) + b'end'
        for payload, expected in ((raw, 'raw'), (self.checkpoint(raw), 'drastic-zlib')):
            encoding, decoded = decode_checkpoint(payload)
            self.assertEqual(encoding, expected)
            self.assertEqual(extract_counter(decoded)['vblank_count'], 0x100000123)

    def test_literal_magic_or_torn_count_is_not_a_clock(self):
        raw = MAGIC + bytes(16) + self.marker(123)
        self.assertEqual(extract_counter(raw)['vblank_count'], 123)
        with self.assertRaisesRegex(ValueError, 'No complete'):
            extract_counter(MAGIC + struct.pack('<QQ', 124, 123 ^ MASK))

    def test_duplicate_ram_mirrors_must_agree(self):
        self.assertEqual(len(extract_counter(self.marker(9) * 2)['marker_offsets']), 2)
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            extract_counter(self.marker(9) + self.marker(10))

    def test_truncated_and_corrupt_dra_stic_states_are_rejected(self):
        good = self.checkpoint(self.marker(17))
        with self.assertRaisesRegex(ValueError, 'length'):
            decode_checkpoint(good[:-1])
        broken = good[:-1] + bytes([good[-1] ^ 1])
        with self.assertRaisesRegex(ValueError, 'invalid compressed'):
            decode_checkpoint(broken)
        truncated = bytearray(good[:-4])
        struct.pack_into('<I', truncated, 0x40, len(truncated) - 0x44)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            decode_checkpoint(truncated)

    def test_small_compressed_bomb_is_bounded(self):
        with self.assertRaisesRegex(ValueError, 'expands beyond'):
            decode_checkpoint(self.checkpoint(bytes(8192)), limit=1024)


if __name__ == '__main__':
    unittest.main()
