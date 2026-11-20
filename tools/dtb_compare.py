#!/usr/bin/env python3
"""Compare two device tree blobs by meaning, not by bytes (docs/OWN_BSP_PLAN.md, Phase C).

Phandle numbers change whenever two trees differ at all, so a plain dtc decompile shows every clock, pin and supply
as different. This reads each .dtb, resolves every phandle reference to the path of the node it points at, using the
target's #...-cells the way the kernel does, and prints the nodes and properties that really differ.

  dtb_compare.py A.dtb B.dtb            what differs between A and B
  dtb_compare.py A.dtb B.dtb --quiet    exit 0 when they mean the same, 1 when not (for scripts)
"""
import argparse
from pathlib import Path
import struct
import sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9

# property -> the cells property of the node each phandle points at (None: a list of bare phandles)
SPECIFIERS = {
    'clocks': '#clock-cells',
    'assigned-clocks': '#clock-cells',
    'assigned-clock-parents': '#clock-cells',
    'resets': '#reset-cells',
    'power-domains': '#power-domain-cells',
    'phys': '#phy-cells',
    'pwms': '#pwm-cells',
    'io-channels': '#io-channel-cells',
    'dmas': '#dma-cells',
    'iommus': '#iommu-cells',
    'sound-dai': '#sound-dai-cells',
    'thermal-sensors': '#thermal-sensor-cells',
    'cooling-device': '#cooling-cells',
    'interrupts-extended': '#interrupt-cells',
    'mboxes': '#mbox-cells',
    'nvmem-cells': None,
    'operating-points-v2': None,
    'interrupt-parent': None,
    'remote-endpoint': None,
    'cpu': None,
    'cpus': None,
    'ports': None,
    'trip': None,
    'backlight': None,
    'panel': None,
    'monitored-battery': None,
    'power-supplies': None,
    'mmc-pwrseq': None,
    'ddc-i2c-bus': None,
    'rockchip,grf': None,
    'rockchip,pmu': None,
    'rockchip,pipe-grf': None,
    'rockchip,pipe-phy-grf': None,
    'rockchip,phy-grf': None,
    'rockchip,usbgrf': None,
    'msi-parent': None,
    'cpu-idle-states': None,
    'joypad': None,
    'mux-controls': '#mux-control-cells',
    'io-backends': '#io-backend-cells',
    'audio-routing': 'string',
    'pm_qos': None,
    'snps,axi-config': None,
    'snps,mtl-rx-config': None,
    'snps,mtl-tx-config': None,
    'simple-audio-card,aux-devs': None,
}
# fixed layouts: (cells before the phandle, cells after it) per entry
FIXED = {'gpio-ranges': (0, 3), 'msi-map': (1, 2), 'iommu-map': (1, 2)}


def parse(path):
    """{node path: {property: raw bytes}} of a .dtb, plus {phandle: node path}."""
    blob = Path(path).read_bytes()
    magic, _, off_struct, off_strings, _, version, _, _, size_strings, size_struct = struct.unpack_from('>10I', blob)
    if magic != 0xd00dfeed:
        raise SystemExit(f'{path}: not a device tree blob')
    strings = blob[off_strings:off_strings + size_strings]
    nodes, stack, pos, end = {}, [], off_struct, off_struct + size_struct
    while pos < end:
        token, = struct.unpack_from('>I', blob, pos)
        pos += 4
        if token == FDT_BEGIN_NODE:
            name_end = blob.index(b'\0', pos)
            stack.append(blob[pos:name_end].decode())
            pos = (name_end + 4) & ~3
            nodes['/' + '/'.join(s for s in stack if s)] = {}
        elif token == FDT_END_NODE:
            stack.pop()
        elif token == FDT_PROP:
            length, name_off = struct.unpack_from('>II', blob, pos)
            pos += 8
            name = strings[name_off:strings.index(b'\0', name_off)].decode()
            nodes['/' + '/'.join(s for s in stack if s)][name] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == FDT_END:
            break
    phandles = {}
    for node, props in nodes.items():
        for key in ('phandle', 'linux,phandle'):
            if key in props:
                phandles[struct.unpack('>I', props[key])[0]] = node
    return nodes, phandles


def cells(raw):
    return list(struct.unpack(f'>{len(raw) // 4}I', raw)) if len(raw) % 4 == 0 else None


def text(raw):
    """A string list when the value looks like one, else None."""
    if not raw or raw[-1:] != b'\0' or b'\0\0' in raw[:-1]:
        return None
    try:
        parts = raw[:-1].decode('ascii').split('\0')
    except UnicodeDecodeError:
        return None
    return parts if all(p and p.isprintable() for p in parts) else None


def interrupt_map(node, values, nodes, phandles, target):
    """interrupt-map entries: child unit address + child specifier, parent phandle, parent address + specifier."""
    def number(path, prop, default):
        raw = nodes.get(path, {}).get(prop)
        return struct.unpack('>I', raw)[0] if raw else default

    child = number(node, '#address-cells', 0) + number(node, '#interrupt-cells', 1)
    out, i = [], 0
    while i + child < len(values):
        parent = phandles.get(values[i + child])
        tail = number(parent, '#address-cells', 0) + number(parent, '#interrupt-cells', 1)
        out.append((*values[i:i + child], target(values[i + child]), *values[i + child + 1:i + child + 1 + tail]))
        i += child + 1 + tail
    return tuple(out)


def meaning(name, raw, nodes, phandles, node_of=None):
    """A comparable form of one property value: phandles become node paths."""
    words = text(raw)
    if words is not None and name not in ('phandle', 'linux,phandle'):
        return tuple(words)
    values = cells(raw)
    if values is None:
        return raw.hex()

    def target(handle):
        return phandles.get(handle, f'<phandle {handle:#x}?>')

    def cell_count(handle, prop):
        node = phandles.get(handle)
        raw = nodes.get(node, {}).get(prop)
        return struct.unpack('>I', raw)[0] if raw else 0

    base = name
    if name.endswith('-gpios') or name.endswith('-gpio') or name in ('gpios', 'gpio'):
        spec = '#gpio-cells'
    elif name.endswith('-supply') or name.startswith('pinctrl-'):
        spec = None
    elif name == 'rockchip,pins':
        # groups of bank, pin, function, config phandle
        return tuple((v[0], v[1], v[2], target(v[3])) for v in zip(*[iter(values)] * 4))
    elif name in FIXED:
        before, after = FIXED[name]
        step = before + 1 + after
        return tuple((*v[:before], target(v[before]), *v[before + 1:]) for v in zip(*[iter(values)] * step))
    elif name == 'interrupt-map':
        return interrupt_map(node_of, values, nodes, phandles, target)
    elif base in SPECIFIERS:
        spec = SPECIFIERS[base]
        if spec == 'string':
            return raw.hex()
    else:
        return tuple(values)
    out, i = [], 0
    while i < len(values):
        handle = values[i]
        if handle == 0:  # an empty entry, as in clocks = <0>
            out.append(('none',))
            i += 1
            continue
        count = 0 if spec is None else cell_count(handle, spec)
        out.append((target(handle), *values[i + 1:i + 1 + count]))
        i += 1 + count
    return tuple(out)


def compare(a_path, b_path):
    a_nodes, a_ph = parse(a_path)
    b_nodes, b_ph = parse(b_path)
    only_a = sorted(set(a_nodes) - set(b_nodes))
    only_b = sorted(set(b_nodes) - set(a_nodes))
    changed = []
    for node in sorted(set(a_nodes) & set(b_nodes)):
        a_props = {
            k: meaning(k, v, a_nodes, a_ph, node)
            for k, v in a_nodes[node].items() if k not in ('phandle', 'linux,phandle')
        }
        b_props = {
            k: meaning(k, v, b_nodes, b_ph, node)
            for k, v in b_nodes[node].items() if k not in ('phandle', 'linux,phandle')
        }
        for key in sorted(set(a_props) | set(b_props)):
            if a_props.get(key) != b_props.get(key):
                changed.append((node, key, a_props.get(key), b_props.get(key)))
    return only_a, only_b, changed


def short(value, width=110):
    text = 'absent' if value is None else repr(value)
    return text if len(text) <= width else text[:width - 3] + '...'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('a')
    parser.add_argument('b')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()
    only_a, only_b, changed = compare(args.a, args.b)
    if not args.quiet:
        for title, nodes in ((f'only in {args.a}', only_a), (f'only in {args.b}', only_b)):
            print(f'{title}: {len(nodes)} nodes')
            for node in nodes:
                print('  ' + node)
        print(f'properties that differ: {len(changed)}')
        for node, key, a, b in changed:
            print(f'  {node} :: {key}\n      a: {short(a)}\n      b: {short(b)}')
    return 0 if not (only_a or only_b or changed) else 1


if __name__ == '__main__':
    sys.exit(main())
