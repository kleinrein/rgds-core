"""Photo mode's compositor (src/launcher/photo_mode.hpp): every lens and frame draws, the pen, stamps, eraser and
undo touch only the ink layer, and the date stamp lands in the corner, with no launcher or device."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r'''
#include "launcher/photo_play_sources.hpp"
#include "launcher/photo_mode.hpp"
#include <cassert>
#include <cstdio>
#include <set>

using namespace photo;
static size_t differ(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i++) n += (a[i] & 0xFFFFFFu) != (b[i] & 0xFFFFFFu);
    return n;
}
int main(int, char** argv) {
    play::Font font;
    assert(font.load(argv[1]));
    std::vector<uint32_t> source(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) source[size_t(y) * W + x] = rgba(x * 255 / W, y * 255 / H, (x / 32 + y / 32) % 2 ? 200 : 40);
    Editor e;
    e.font = &font;
    e.open(source, 2026, 11, 20);
    const auto plain = e.compose();
    assert(differ(plain, source) == 0 && !e.edited);

    // Every lens changes the picture (Normal leaves it), and the same choices draw the same pixels twice.
    std::set<uint64_t> looks;
    for (int l = 1; l < int(Lens::Count); l++) {
        e.set_lens(Lens(l));
        const auto first = e.compose();
        assert(differ(first, plain) > W * H / 20);
        e.set_frame(1);
        e.set_frame(0);
        assert(differ(first, e.compose()) == 0);
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t v : first) hash = (hash ^ v) * 1099511628211ull;
        looks.insert(hash);
    }
    assert(looks.size() == size_t(Lens::Count) - 1);
    e.set_lens(Lens::Normal);

    // Every frame (ours and the player's console frames) draws round the photo and keeps some of it.
    assert(e.frames() > int(Decor::Count) + 20);
    for (int f = 1; f < e.frames(); f++) {
        e.set_frame(f);
        const auto& framed = e.compose();
        assert(differ(framed, plain) > W * H / 20);
    }
    e.set_frame(0);

    // Pen and stamps go on the ink layer; undo takes the last one back; the eraser clears ink, never the photo.
    e.ink = 2;
    e.pen_size = 1;
    e.begin(100, 100);
    e.stroke(300, 100);
    const auto drawn = e.compose();
    assert(differ(drawn, plain) > 500 && (drawn[size_t(100) * W + 200] & 0xFFFFFFu) == (inks[2] & 0xFFFFFFu));
    e.stamp = 0;
    e.stamp_size = 2;
    e.place_stamp(400, 300);
    assert(differ(e.compose(), drawn) > 1000);
    assert(e.undo());
    assert(differ(e.compose(), drawn) == 0);
    e.ink = eraser_ink;
    e.pen_size = 2;
    e.begin(90, 100);
    e.stroke(320, 100);
    assert(differ(e.compose(), plain) == 0);

    // The date: orange segments in the lower right corner only.
    e.set_date(true);
    const auto dated = e.compose();
    size_t orange = 0, elsewhere = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint32_t v = dated[size_t(y) * W + x];
            if ((v & 0xFFFFFFu) == (plain[size_t(y) * W + x] & 0xFFFFFFu)) continue;
            if (x > W / 2 && y > H * 3 / 4 && red(v) > 200 && green(v) > 90 && blue(v) < 140) orange++;
            else if (!(x > W / 2 && y > H * 3 / 4)) elsewhere++;
        }
    assert(orange > 400 && elsewhere == 0);
    assert(date_text(2026, 11, 20, 0) == "20 11 '26" && date_text(2026, 11, 20, 2) == "'26 11 20");
    std::printf("ok %d frames\n", e.frames());
    return 0;
}
'''


class PhotoModeTests(unittest.TestCase):
    def test_compositor(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(PROGRAM)
            subprocess.run(
                [
                    os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I',
                    str(ROOT / 'src'), '-I',
                    str(ROOT / 'play/src'),
                    str(source), '-o',
                    str(Path(tmp) / 'test')
                ],
                check=True
            )
            result = subprocess.run(
                [str(Path(tmp) / 'test'), str(ROOT / 'assets/fonts')], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(result.stdout.startswith('ok'))


if __name__ == '__main__':
    unittest.main()
