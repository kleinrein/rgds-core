"""tools/dtb_compare.py tells device trees apart by meaning: the same tree with its phandles numbered differently is
the same, a changed value or node is not (docs/OWN_BSP_PLAN.md, Phase C, where it proves each step)."""
import struct, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import dtb_compare


def fdt(tree):
    """A minimal .dtb from {path: {property: bytes}}, nodes in the dict's order (parents before children)."""
    strings, struct_parts, offsets = b'', [], {}

    def name_offset(name):
        nonlocal strings
        if name not in offsets:
            offsets[name] = len(strings)
            strings += name.encode() + b'\0'
        return offsets[name]

    def pad(data):
        return data + b'\0' * (-len(data) % 4)

    open_path = []
    for path, props in tree.items():
        parts = [p for p in path.split('/') if p]
        while open_path and open_path != parts[:len(open_path)]:
            struct_parts.append(struct.pack('>I', 2))
            open_path.pop()
        name = parts[-1] if parts else ''
        struct_parts.append(struct.pack('>I', 1) + pad(name.encode() + b'\0'))
        open_path = parts
        for key, value in props.items():
            struct_parts.append(struct.pack('>III', 3, len(value), name_offset(key)) + pad(value))
    for _ in range(len(open_path) + 1):
        struct_parts.append(struct.pack('>I', 2))
    struct_parts.append(struct.pack('>I', 9))
    body = b''.join(struct_parts)
    header_size, rsvmap = 40, b'\0' * 16
    off_struct = header_size + len(rsvmap)
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    header = struct.pack(
        '>10I', 0xd00dfeed, total, off_struct, off_strings, header_size, 17, 16, 0, len(strings), len(body)
    )
    return header + rsvmap + body + strings


def u32(*values):
    return struct.pack(f'>{len(values)}I', *values)


def board(cru, regulator, clock_id=7, pull=1):
    """A tree with a clock provider, a regulator, a pin group and a consumer of all three."""
    return {
        '/': {},
        '/cru': {
            '#clock-cells': u32(1),
            'phandle': u32(cru)
        },
        '/pinctrl': {},
        '/pinctrl/pcfg-pull-up': {
            'phandle': u32(30 + pull)
        },
        '/pinctrl/hall': {
            'rockchip,pins': u32(0, 19, 0, 30 + pull),
            'phandle': u32(40)
        },
        '/reg': {
            'regulator-name': b'vcc_amp\0',
            'phandle': u32(regulator)
        },
        '/amp': {
            'compatible': b'aw,aw87391-left\0',
            'clocks': u32(cru, clock_id),
            'vcc-supply': u32(regulator),
            'pinctrl-0': u32(40),
        },
    }


class DtbCompareTest(unittest.TestCase):
    def compare(self, a, b):
        with tempfile.TemporaryDirectory() as tmp:
            pa, pb = Path(tmp, 'a.dtb'), Path(tmp, 'b.dtb')
            pa.write_bytes(fdt(a))
            pb.write_bytes(fdt(b))
            return dtb_compare.compare(pa, pb)

    def test_renumbered_phandles_mean_the_same(self):
        self.assertEqual(self.compare(board(cru=1, regulator=2), board(cru=9, regulator=5)), ([], [], []))

    def test_a_clock_id_that_happens_to_equal_a_phandle_is_still_a_clock_id(self):
        # the clock ID 2 is also the regulator's phandle in the first tree: it must not be read as a reference
        self.assertEqual(
            self.compare(board(cru=1, regulator=2, clock_id=2), board(cru=9, regulator=5, clock_id=2)), ([], [], [])
        )

    def test_a_changed_clock_is_found(self):
        _, _, changed = self.compare(board(cru=1, regulator=2, clock_id=7), board(cru=1, regulator=2, clock_id=8))
        self.assertEqual([(node, key) for node, key, _, _ in changed], [('/amp', 'clocks')])

    def test_a_pin_group_pointing_at_another_config_is_found(self):
        a = board(cru=1, regulator=2)
        b = {}
        for path, props in board(cru=1, regulator=2).items():  # the new pin config inside /pinctrl
            b[path] = props
            if path == '/pinctrl/pcfg-pull-up':
                b['/pinctrl/pcfg-pull-none'] = {'phandle': u32(50)}
        b['/pinctrl/hall']['rockchip,pins'] = u32(0, 19, 0, 50)
        only_a, only_b, changed = self.compare(a, b)
        self.assertEqual(only_b, ['/pinctrl/pcfg-pull-none'])
        self.assertEqual([(node, key) for node, key, _, _ in changed], [('/pinctrl/hall', 'rockchip,pins')])

    def test_a_missing_node_is_found(self):
        a = board(cru=1, regulator=2)
        b = dict(a)
        b['/ramoops'] = {'compatible': b'ramoops\0'}
        self.assertEqual(self.compare(a, b)[:2], ([], ['/ramoops']))

    def test_the_shipped_tree_against_itself(self):
        shipped = ROOT / '.cache/own-os/build/rk3568-anbernic-rg-ds.dtb'
        if not shipped.is_file():
            self.skipTest('no RG DS OS build in .cache/own-os/build')
        self.assertEqual(dtb_compare.compare(shipped, shipped), ([], [], []))


if __name__ == '__main__':
    unittest.main()
