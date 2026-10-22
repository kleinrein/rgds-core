#!/usr/bin/env python3
"""Raster-rules test: build the RGDS raster-rules ROM (tests/fixtures/raster_rules), run it through the
headless engine twice (CPU raster as the reference, GPU raster at 2x under lavapipe in the Lima VM) and
compare the 3D layer per screen cell. Each 64x48 cell draws one rendering rule (see the ROM source);
inside a polygon every sample gives the DS corner value, so a mismatch away from polygon edges is a
real rule difference, while edge pixels may differ (no DS edge-row emulation on the GPU) and are
reported separately; inside a polygon a difference of up to one 6-bit step per blended layer is the GPU blend's
float rounding of the DS integer blend (tolerated by default, --tolerance).

  python3 tools/raster_rules.py build                      -> build/raster-rules/raster-rules.nds
  python3 tools/raster_rules.py run --engine BIN [--gpu-env K=V ...] [--out DIR]
  python3 tools/raster_rules.py compare DIR                -> table, PNGs in DIR
  python3 tools/raster_rules.py all --engine BIN [...]      (build, run, compare)

BIN is a Linux dsperate-headless built with DSPERATE_VULKAN=ON (build/frag-cost/zbuild.sh NAME SRC headless);
it runs in the Lima VM (--local runs it here instead). Exit status 1 when any cell has interior mismatches.
"""
import argparse, os, struct, subprocess, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/raster-rules'
CELLS = [  # (column, row): name
    ((0, 0), 'marker'), ((1, 0), 'same id over alpha-0 texels'), ((2, 0), 'other id over alpha-0 texels'), ((3, 0), 'depth-write cut-out'),
    ((0, 1), 'cut-out without depth write'), ((1, 1), 'depth-equal decal'), ((2, 1), 'shadow over id 0 / own id'), ((3, 1), 'alpha test, same id over it'),
    ((0, 2), 'alpha-31 A5I3, same id over opaque texels'), ((1, 2), 'fog by depth'), ((2, 2), 'toon shading'), ((3, 2), 'decal blend'),
    ((0, 3), 'repeat + flip wrap, opaque cut-out'), ((1, 3), 'translucent over empty rear plane'), ((2, 3), 'opaque overlap order'), ((3, 3), 'alpha-31 A3I5, same id over opaque texels'),
]
MASK = 0x1F3F3F3F  # RGB666 + 5-bit alpha of a 3D layer record word


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    llvm = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin'))
    (OUT / 'start.s').write_text(
        '.syntax unified\n.arm\n.section .text.start\n.global _start\n_start:\n ldr sp, =0x023ff000\n bl main\n b .\n'
    )
    (OUT / 'arm9.ld').write_text(
        'ENTRY(_start)\nSECTIONS { . = 0x02000000; .text : { *(.text.start) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }\n'
    )
    flags = ['-target', 'arm-none-eabi', '-mcpu=arm946e-s', '-marm', '-O2', '-ffreestanding', '-fno-builtin', '-Wall']
    subprocess.run([llvm / 'clang', *flags, '-c', OUT / 'start.s', '-o', OUT / 'start.o'], check=True)
    subprocess.run(
        [llvm / 'clang', *flags, '-c', ROOT / 'tests/fixtures/raster_rules/arm9.c', '-o', OUT / 'main.o'], check=True
    )
    subprocess.run(
        [llvm / 'ld.lld', '-T', OUT / 'arm9.ld', OUT / 'start.o', OUT / 'main.o', '-o', OUT / 'arm9.elf'], check=True
    )
    subprocess.run([llvm / 'llvm-objcopy', '-O', 'binary', OUT / 'arm9.elf', OUT / 'arm9.bin'], check=True)
    subprocess.run(
        [
            llvm / 'clang', '-target', 'arm-none-eabi', '-mcpu=arm7tdmi', '-c',
            ROOT / 'tests/fixtures/nnddss_probe/arm7.s', '-o', OUT / 'arm7.o'
        ],
        check=True
    )
    subprocess.run(
        [llvm / 'ld.lld', '-Ttext=0x03800000', '-e', '_start', OUT / 'arm7.o', '-o', OUT / 'arm7.elf'], check=True
    )
    subprocess.run([llvm / 'llvm-objcopy', '-O', 'binary', OUT / 'arm7.elf', OUT / 'arm7.bin'], check=True)
    image = bytearray(0x20000)
    image[:12] = b'RGDS RASTER '
    image[12:16] = b'####'
    image[16:18] = b'00'
    for arm, offset, address, header in ((9, 0x200, 0x02000000, 0x20), (7, 0x8000, 0x03800000, 0x30)):
        code = (OUT / f'arm{arm}.bin').read_bytes()
        assert len(code) < 0x7e00
        struct.pack_into('<4I', image, header, offset, address, address, len(code))
        image[offset:offset + len(code)] = code
    struct.pack_into('<2I', image, 0x80, len(image), 0x200)

    def crc(data):
        value = 0xffff
        for byte in data:
            value ^= byte
            for _ in range(8):
                value = (value >> 1) ^ (0xa001 if value & 1 else 0)
        return value

    struct.pack_into('<H', image, 0x15c, crc(image[0xc0:0x15c]))
    struct.pack_into('<H', image, 0x15e, crc(image[:0x15e]))
    rom = OUT / 'raster-rules.nds'
    rom.write_bytes(image)
    print(rom)
    return rom


def run(engine, out, gpu_env, local, frame=20):
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    rom = OUT / 'raster-rules.nds'
    gpu_dir = out / 'gpu'
    gpu_dir.mkdir(exist_ok=True)
    for f in gpu_dir.glob('*.raw'):
        f.unlink()
    env_gpu = ' '.join(gpu_env)
    script = f"""set -e
cd {out}
{engine} {rom} --direct --frames {frame + 4} --dump-3d {out}/cpu.raw --dump-from {frame} --dump-count 1 > {out}/cpu.log 2>&1
rm -rf /tmp/hd; ln -s {gpu_dir} /tmp/hd
env {env_gpu} VK_ICD_FILENAMES=${{VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}} DS_HIRES_DUMP={frame + 1}:1 DS_HIRES_DEBUG=1 {engine} {rom} --direct --frames {frame + 4} --present 2 > {out}/gpu.log 2>&1
rm -f /tmp/hd
ls {gpu_dir}
"""
    if local:
        subprocess.run(['bash', '-c', script], check=True)
    else:
        lima = ROOT / '.cache/linux-builder/lima/bin/limactl'
        subprocess.run(
            [lima, 'shell', 'rgds-image', '--', 'bash', '-c', script],
            check=True,
            env={
                **os.environ, 'LIMA_HOME': str(ROOT / '.cache/linux-builder/state')
            }
        )
    (out / 'gpu_env.txt').write_text(env_gpu + '\n')
    return out


def load_words(path, w, h):
    import numpy as np
    d = np.fromfile(path, dtype='<u4')
    if d.size != w * h: raise SystemExit(f'{path}: {d.size} words, expected {w * h}')
    return d.reshape(h, w) & MASK


def png(path, img):
    import numpy as np
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].astype(np.uint8).tobytes() for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    Path(path).write_bytes(
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    )


def rgb(words):  # RGB666 + alpha -> 8-bit RGB, alpha 0 shown as dark magenta checks
    import numpy as np
    r, g, b, a = (words & 63) << 2, ((words >> 8) & 63) << 2, ((words >> 16) & 63) << 2, (words >> 24) & 31
    img = np.stack([r, g, b], -1).astype(np.int32)
    img[a == 0] = [40, 0, 40]
    return img


def compare(out, tolerance=2):
    import numpy as np
    out = Path(out)
    cpu = load_words(out / 'cpu.raw', 256, 192)
    hs = sorted((out / 'gpu').glob('*-h.raw'))
    if not hs: raise SystemExit('no GPU dump (see gpu.log)')
    gpu = load_words(hs[-1], 512, 384)
    # Every 2x2 block of the GPU image is held against the DS pixel. Edge pixels (a different word within
    # the 3x3 neighbourhood of either image: the GPU has no DS edge-row emulation and draws a quad's last
    # column) are reported apart. Inside, a difference of one 6-bit step is the GPU blend's float rounding of
    # the DS integer blend (documented) and two stacked layers can reach two; anything larger is a rule
    # difference and fails the cell.
    g4 = gpu.reshape(192, 2, 256, 2).transpose(0, 2, 1, 3)

    def chans(w):
        return np.stack([w & 63, (w >> 8) & 63, (w >> 16) & 63, (w >> 24) & 31], -1).astype(np.int32)

    c4 = chans(cpu)[:, :, None, None, :]
    delta = np.abs(chans(g4) - c4).max(axis=(2, 3, 4))

    def edges(img):
        pad = np.pad(img, 1, mode='edge')
        e = np.zeros(img.shape, bool)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                e |= pad[1 + dy:193 + dy, 1 + dx:257 + dx] != img
        return e

    edge = edges(cpu) | edges(gpu[::2, ::2]) | edges(gpu[1::2, 1::2])
    rule_bad = (delta > tolerance) & ~edge
    round_bad = (delta > 0) & (delta <= tolerance) & ~edge
    edge_bad = (delta > 0) & edge
    env = (out / 'gpu_env.txt').read_text().strip() if (out / 'gpu_env.txt').exists() else ''
    print(f"raster rules: {out} (GPU env: {env or 'none'}; tolerance {tolerance})")
    print(f"{'cell':6} {'case':48} {'rule':>6} {'round':>6} {'edge':>6}  result")
    failed = 0
    for (cx, cy), name in CELLS:
        sl = (slice(cy * 48, cy * 48 + 48), slice(cx * 64, cx * 64 + 64))
        rb, ob, eb = int(rule_bad[sl].sum()), int(round_bad[sl].sum()), int(edge_bad[sl].sum())
        failed += rb > 0
        print(f"({cx},{cy})  {name:48} {rb:6d} {ob:6d} {eb:6d}  {'FAIL' if rb else 'ok'}")
    print(
        f"total: {int(rule_bad.sum())} rule, {int(round_bad.sum())} rounding and {int(edge_bad.sum())} edge pixels differ; {failed} of {len(CELLS)} cells fail"
    )
    interior_bad = rule_bad
    marker = cpu[24, 32]
    if marker & 0x3F3F3F != 0x3F or (marker >> 24) != 31:
        print(f"warning: marker cell is not opaque red (word {marker:08x}); orientation or decode is off")
    png(out / 'cpu.png', rgb(cpu))
    png(out / 'gpu.png', rgb(gpu))
    diff = rgb(np.repeat(np.repeat(cpu, 2, 0), 2, 1)) // 3
    big = lambda m: np.repeat(np.repeat(m, 2, 0), 2, 1)
    diff[big(edge_bad)] = [255, 220, 0]
    diff[big(round_bad)] = [0, 120, 255]
    diff[big(interior_bad)] = [255, 0, 0]
    png(out / 'diff.png', diff)
    return failed == 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=('build', 'run', 'compare', 'all'))
    p.add_argument('dir', nargs='?', help='compare: the run folder')
    p.add_argument('--engine', help='run/all: Linux dsperate-headless with Vulkan (absolute path)')
    p.add_argument('--out', default=str(OUT / 'run'), help='run/all: output folder (default build/raster-rules/run)')
    p.add_argument(
        '--gpu-env',
        action='append',
        default=[],
        metavar='KEY=VALUE',
        help='run/all: engine environment for the GPU run (e.g. DS_LEAN_NODISCARD=1)'
    )
    p.add_argument('--local', action='store_true', help='run/all: run the engine here instead of in the Lima VM')
    p.add_argument('--frame', type=int, default=20, help='run/all: the frame to dump (the scene is static)')
    p.add_argument(
        '--tolerance',
        type=int,
        default=2,
        help=
        'compare: largest per-channel step still counted as blend rounding (default 2: one step per blended layer, two layers stack in the tests)'
    )
    a = p.parse_args()
    ok = True
    if a.command in ('build', 'all'): build()
    if a.command in ('run', 'all'):
        if not a.engine: p.error('--engine is required')
        run(Path(a.engine).resolve(), a.out, a.gpu_env, a.local, a.frame)
    if a.command == 'compare': ok = compare(a.dir or a.out, a.tolerance)
    if a.command == 'all': ok = compare(a.out, a.tolerance)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
