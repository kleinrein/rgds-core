"""A QR code for a short text (byte mode, error correction M, versions 1-10).

Enough for the addresses the bottom screen shows (runtime/port_files.py), so
no library is shipped. Follows ISO/IEC 18004; the structure is the usual one
(function patterns, Reed-Solomon blocks, zigzag placement, best of 8 masks).
"""

# Error correction level M, versions 1-10: (codewords per block, blocks).
ECC_M = {
    1: (10, 1),
    2: (16, 1),
    3: (26, 1),
    4: (18, 2),
    5: (24, 2),
    6: (16, 4),
    7: (18, 4),
    8: (22, 4),
    9: (22, 5),
    10: (26, 5)
}
FORMAT_M = 0  # the two format bits of level M


def raw_codewords(version):
    result = (16 * version + 128) * version + 64
    if version >= 2:
        count = version // 7 + 2
        result -= (25 * count - 10) * count - 55
        if version >= 7: result -= 36
    return result // 8


def _gf_multiply(x, y):
    z = 0
    for i in reversed(range(8)):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z


def _rs_divisor(degree):
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            result[j] = _gf_multiply(result[j], root)
            if j + 1 < degree: result[j] ^= result[j + 1]
        root = _gf_multiply(root, 2)
    return result


def _rs_remainder(data, divisor):
    result = [0] * len(divisor)
    for byte in data:
        factor = byte ^ result.pop(0)
        result.append(0)
        for i, coefficient in enumerate(divisor):
            result[i] ^= _gf_multiply(coefficient, factor)
    return result


def _codewords(text, version):
    data = text.encode('utf-8')
    bits = [0, 1, 0, 0]
    count_bits = 8 if version <= 9 else 16
    bits += [(len(data) >> i) & 1 for i in reversed(range(count_bits))]
    for byte in data:
        bits += [(byte >> i) & 1 for i in reversed(range(8))]
    ecc, blocks = ECC_M[version]
    raw = raw_codewords(version)
    capacity = (raw - ecc * blocks) * 8
    if len(bits) > capacity: return None
    bits += [0] * min(4, capacity - len(bits))
    bits += [0] * (-len(bits) % 8)
    pad = 0xEC
    while len(bits) < capacity:
        bits += [(pad >> i) & 1 for i in reversed(range(8))]
        pad ^= 0xEC ^ 0x11
    codewords = [int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    short_count = blocks - raw % blocks
    short_length = raw // blocks
    divisor = _rs_divisor(ecc)
    split, at = [], 0
    for i in range(blocks):
        length = short_length - ecc + (0 if i < short_count else 1)
        block = codewords[at:at + length]
        at += length
        # Short blocks carry a placeholder where long blocks have one more data byte; it is skipped below.
        split.append(block + ([0] if i < short_count else []) + _rs_remainder(block, divisor))
    result = []
    for i in range(len(split[0])):
        for j, block in enumerate(split):
            if i != short_length - ecc or j >= short_count: result.append(block[i])
    return result


def _alignment_positions(version, size):
    if version == 1: return []
    count = version // 7 + 2
    step = (version * 8 + count * 3 + 5) // (count * 4 - 4) * 2
    return list(reversed([size - 7 - i * step for i in range(count - 1)] + [6]))


def _mask_bit(mask, x, y):
    return (
        (x + y) % 2 == 0, y % 2 == 0, x % 3 == 0, (x + y) % 3 == 0, (x // 3 + y // 2) % 2 == 0,
        x * y % 2 + x * y % 3 == 0, (x * y % 2 + x * y % 3) % 2 == 0, ((x + y) % 2 + x * y % 3) % 2 == 0
    )[mask]


def _penalty(modules):
    size = len(modules)
    score = 0
    lines = modules + [[modules[y][x] for y in range(size)] for x in range(size)]
    finder_a, finder_b = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0], [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1]
    for line in lines:
        run = 1
        for i in range(1, size + 1):
            if i < size and line[i] == line[i - 1]:
                run += 1
                continue
            if run >= 5: score += run - 2
            run = 1
        for i in range(size - 10):
            window = line[i:i + 11]
            if window == finder_a or window == finder_b: score += 40
    for y in range(size - 1):
        for x in range(size - 1):
            if modules[y][x] == modules[y][x + 1] == modules[y + 1][x] == modules[y + 1][x + 1]: score += 3
    dark = sum(map(sum, modules))
    score += abs(dark * 20 - size * size * 10) // (size * size) * 10
    return score


def encode(text):
    """The modules of text's QR code, rows of 0/1 without the quiet zone. ValueError when it does not fit."""
    for version in range(1, 11):
        data = _codewords(text, version)
        if data is not None: break
    else:
        raise ValueError('Too long for a QR code here.')
    size = version * 4 + 17
    modules = [[0] * size for _ in range(size)]
    function = [[False] * size for _ in range(size)]

    def put(x, y, dark):
        modules[y][x] = int(bool(dark))
        function[y][x] = True

    for i in range(size):
        put(6, i, i % 2 == 0)
        put(i, 6, i % 2 == 0)
    for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < size and 0 <= y < size:
                    put(x, y, max(abs(dx), abs(dy)) not in (2, 4))
    positions = _alignment_positions(version, size)
    last = len(positions) - 1
    for i, cx in enumerate(positions):
        for j, cy in enumerate(positions):
            if (i, j) in ((0, 0), (0, last), (last, 0)): continue
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    put(cx + dx, cy + dy, max(abs(dx), abs(dy)) != 1)

    def format_bits(mask):
        value = FORMAT_M << 3 | mask
        remainder = value
        for _ in range(10):
            remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537)
        bits = (value << 10 | remainder) ^ 0x5412
        for i in range(6):
            put(8, i, bits >> i & 1)
        put(8, 7, bits >> 6 & 1)
        put(8, 8, bits >> 7 & 1)
        put(7, 8, bits >> 8 & 1)
        for i in range(9, 15):
            put(14 - i, 8, bits >> i & 1)
        for i in range(8):
            put(size - 1 - i, 8, bits >> i & 1)
        for i in range(8, 15):
            put(8, size - 15 + i, bits >> i & 1)
        put(8, size - 8, 1)

    format_bits(0)  # reserves the format areas before the data goes in
    if version >= 7:
        remainder = version
        for _ in range(12):
            remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25)
        bits = version << 12 | remainder
        for i in range(18):
            a, b = size - 11 + i % 3, i // 3
            put(a, b, bits >> i & 1)
            put(b, a, bits >> i & 1)
    i = 0
    right = size - 1
    while right >= 1:
        if right == 6: right = 5
        for vertical in range(size):
            for j in range(2):
                x = right - j
                upward = (right + 1) & 2 == 0
                y = size - 1 - vertical if upward else vertical
                if not function[y][x] and i < len(data) * 8:
                    modules[y][x] = data[i >> 3] >> (7 - (i & 7)) & 1
                    i += 1
        right -= 2
    best = None
    for mask in range(8):
        candidate = [row[:] for row in modules]
        for y in range(size):
            for x in range(size):
                if not function[y][x] and _mask_bit(mask, x, y): candidate[y][x] ^= 1
        saved = [row[:] for row in modules]
        modules[:] = candidate
        format_bits(mask)
        candidate = [row[:] for row in modules]
        modules[:] = saved
        score = _penalty(candidate)
        if best is None or score < best[0]: best = (score, candidate)
    return best[1]


def text_rows(text):
    """encode() as strings of '1' and '0', the form the launcher reads."""
    return [''.join(map(str, row)) for row in encode(text)]
