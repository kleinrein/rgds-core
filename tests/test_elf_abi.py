"""Required ELF versions are structural metadata, not arbitrary string matches."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from elf_abi import inspect_elf


def fixture(requirements=(('libc.so.6', ('GLIBC_2.17', 'GLIBC_2.34')),), machine=183):
    strings = bytearray(b"\0")
    offsets = {}

    def name(value):
        if value not in offsets:
            offsets[value] = len(strings)
            strings.extend(value.encode() + b"\0")
        return offsets[value]

    name("GLIBC_999.99")  # Unreferenced text in the linked string table.
    needs = bytearray()
    for index, (library, versions) in enumerate(requirements):
        count = len(versions)
        record_size = 16 + count * 16
        needs.extend(
            struct.pack("<HHIII", 1, count, name(library), 16, record_size if index + 1 < len(requirements) else 0)
        )
        for auxiliary, version in enumerate(versions):
            needs.extend(struct.pack("<IHHII", 0, 0, auxiliary + 2, name(version), 16 if auxiliary + 1 < count else 0))
    data = bytearray(64)
    strings_at = len(data)
    data.extend(strings)
    needs_at = len(data)
    data.extend(needs)
    # This exported-version section must not be treated as a requirement.
    exports_at = len(data)
    exports = b"GLIBC_999.99\0"
    data.extend(exports)
    sections_at = len(data)
    sections = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0), (0, 3, 0, 0, strings_at, len(strings), 0, 0, 1, 0)]
    if requirements:
        sections.append((0, 0x6FFFFFFE, 0, 0, needs_at, len(needs), 1, len(requirements), 4, 0))
    sections.append((0, 0x6FFFFFFF, 0, 0, exports_at, len(exports), 1, 1, 4, 0))
    for section in sections:
        data.extend(struct.pack("<IIQQQQIIQQ", *section))
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into(
        "<16sHHIQQQIHHHHHH", data, 0, identity, 3, machine, 1, 0, 0, sections_at, 0, 64, 0, 0, 64, len(sections), 0
    )
    return data, dict(needs=needs_at, strings=strings_at, string_size=len(strings), sections=sections_at)


def dynamic_fixture():
    data, offsets = fixture(())
    header = list(struct.unpack_from("<16sHHIQQQIHHHHHH", data))
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offsets["sections"] + i * 64) for i in range(header[12])]
    data = data[:offsets["sections"]]
    strings = b"\0libSDL2-2.0.so.0\0libbridge.so\0/usr/lib\0SDL_RenderPresent\0malloc\0private_cpp\0"
    string_at = len(data)
    data.extend(strings)
    string_index = len(sections)
    sections.append((0, 3, 0, 0, string_at, len(strings), 0, 0, 1, 0))
    dynamic_at = len(data)
    for tag, name in ((1, b"libSDL2"), (14, b"libbridge"), (29, b"/usr/lib")):
        data.extend(struct.pack("<qQ", tag, strings.index(name)))
    data.extend(struct.pack("<qQ", 0, 0))
    sections.append((0, 6, 0, 0, dynamic_at, 64, string_index, 0, 8, 16))
    symbol_at = len(data)
    data.extend(struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0))
    for name, visibility, index in ((b"SDL_RenderPresent", 0, 1), (b"malloc", 0, 0), (b"private_cpp", 2, 1)):
        data.extend(struct.pack("<IBBHQQ", strings.index(name), 0x12, visibility, index, 1, 4))
    sections.append((0, 11, 0, 0, symbol_at, 96, string_index, 0, 8, 24))
    interpreter_at = len(data)
    path = b"/lib/ld-linux-aarch64.so.1\0"
    data.extend(path)
    header[5] = len(data)
    header[9] = 56
    header[10] = 1
    data.extend(struct.pack("<IIQQQQQQ", 3, 4, interpreter_at, 0, 0, len(path), len(path), 1))
    header[6] = len(data)
    header[12] = len(sections)
    for section in sections:
        data.extend(struct.pack("<IIQQQQIIQQ", *section))
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, *header)
    return data, dict(dynamic=dynamic_at, interpreter=interpreter_at, strings=string_at, symbols=symbol_at)


class ElfAbiTests(unittest.TestCase):
    def inspect(self, data, **kwargs):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "payload.so"
            path.write_bytes(data)
            return inspect_elf(path, **kwargs)

    def test_aarch64_required_versions_ignore_unreferenced_strings_and_exports(self):
        data, _ = fixture((('libc.so.6', ('GLIBC_2.17', 'GLIBC_2.34')), ('libm.so.6', ('GLIBC_2.18', 'GLIBC_2.17'))))
        result = self.inspect(data, source="supplied/private-payload")
        self.assertEqual(result["machine"], "aarch64")
        self.assertEqual(result["glibc_versions"], ["2.17", "2.18", "2.34"])
        self.assertEqual(result["glibc_minimum"], "2.34")
        self.assertEqual(result["source"], "supplied/private-payload")
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_host_elf_uses_numeric_version_order(self):
        data, _ = fixture((('libc.so.6', ('GLIBC_2.9', 'GLIBC_2.3.4', 'GLIBC_2.17')),), machine=62)
        result = self.inspect(data)
        self.assertEqual(result["machine"], "x86_64")
        self.assertEqual(result["glibc_versions"], ["2.3.4", "2.9", "2.17"])

    def test_version_headers_may_precede_all_auxiliary_records(self):
        data, offsets = fixture((('libc.so.6', ('GLIBC_2.17',)), ('libm.so.6', ('GLIBC_2.34',))))
        start = offsets["needs"]
        first = list(struct.unpack_from("<HHIII", data, start))
        second = list(struct.unpack_from("<HHIII", data, start + 32))
        first[3:5] = [32, 16]
        second[3:5] = [32, 0]
        auxiliaries = bytes(data[start + 16:start + 32] + data[start + 48:start + 64])
        data[start:start + 64] = struct.pack("<HHIII", *first) + struct.pack("<HHIII", *second) + auxiliaries
        self.assertEqual(self.inspect(data)["glibc_minimum"], "2.34")

    def test_missing_glibc_requirements_do_not_invent_an_abi_floor(self):
        for requirements in ((), (('libstdc++.so.6', ('CXXABI_1.3', 'GLIBCXX_3.4.29')),)):
            data, _ = fixture(requirements)
            result = self.inspect(data)
            self.assertIsNone(result["glibc_minimum"])
            self.assertEqual(result["glibc_versions"], [])

    def test_non_elf_is_not_an_error(self):
        self.assertIsNone(self.inspect(b"#!/bin/sh\nexit 0\n"))
        self.assertIsNone(self.inspect(b""))

    def test_truncated_header_and_section_table_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "header is truncated"):
            self.inspect(b"\x7fELF")
        data, _ = fixture()
        with self.assertRaisesRegex(ValueError, "section-header table is truncated"):
            self.inspect(data[:-1])

    def test_out_of_bounds_section_and_linked_string_table_are_rejected(self):
        data, offsets = fixture()
        struct.pack_into("<Q", data, offsets["sections"] + 64 + 24, len(data) + 1)
        with self.assertRaisesRegex(ValueError, "section contents is truncated or out of bounds"):
            self.inspect(data)
        data, offsets = fixture()
        struct.pack_into("<I", data, offsets["sections"] + 2 * 64 + 40, 99)
        with self.assertRaisesRegex(ValueError, "no valid linked string table"):
            self.inspect(data)

    def test_truncated_auxiliary_chain_is_rejected(self):
        data, offsets = fixture()
        struct.pack_into("<I", data, offsets["needs"] + 8, 10000)
        with self.assertRaisesRegex(ValueError, "auxiliary record is truncated or out of bounds"):
            self.inspect(data)

    def test_string_offset_and_missing_terminator_are_rejected(self):
        data, offsets = fixture()
        struct.pack_into("<I", data, offsets["needs"] + 16 + 8, 10000)
        with self.assertRaisesRegex(ValueError, "version string is out of bounds"):
            self.inspect(data)
        data, offsets = fixture()
        data[offsets["strings"] + offsets["string_size"] - 1] = ord('X')
        with self.assertRaisesRegex(ValueError, "version string is not terminated"):
            self.inspect(data)

    def test_overlapping_chains_and_disagreeing_record_counts_are_rejected(self):
        data, offsets = fixture()
        struct.pack_into("<I", data, offsets["needs"] + 16 + 12, 0)
        with self.assertRaisesRegex(ValueError, "chain ends early or overlaps"):
            self.inspect(data)
        data, offsets = fixture()
        struct.pack_into("<I", data, offsets["sections"] + 2 * 64 + 44, 5)
        with self.assertRaisesRegex(ValueError, "record count disagrees"):
            self.inspect(data)

    def test_wrong_elf_format_is_rejected_clearly(self):
        for offset, value in ((4, 1), (5, 2)):
            data, _ = fixture()
            data[offset] = value
            with self.assertRaisesRegex(ValueError, "only ELF64 little-endian"):
                self.inspect(data)

    def test_dynamic_dependencies_and_public_exports_are_structural(self):
        data, _ = dynamic_fixture()
        result = self.inspect(data, symbols=True)
        self.assertEqual(result["needed"], ["libSDL2-2.0.so.0"])
        self.assertEqual(result["soname"], "libbridge.so")
        self.assertEqual(result["runpaths"], ["/usr/lib"])
        self.assertEqual(result["interpreter"], "/lib/ld-linux-aarch64.so.1")
        self.assertEqual(result["exports"], ["SDL_RenderPresent"])
        self.assertEqual(result["imports"], ["malloc"])
        self.assertNotIn("exports", self.inspect(data))

    def test_malformed_dynamic_paths_and_unterminated_interpreter_are_rejected(self):
        data, offsets = dynamic_fixture()
        struct.pack_into("<Q", data, offsets["dynamic"] + 8, 99999)
        with self.assertRaisesRegex(ValueError, "dynamic string is out of bounds"):
            self.inspect(data)
        data, offsets = dynamic_fixture()
        data[data.index(0, offsets["interpreter"])] = ord("x")
        with self.assertRaisesRegex(ValueError, "interpreter is not a terminated"):
            self.inspect(data)


if __name__ == "__main__":
    unittest.main()
