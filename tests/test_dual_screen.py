"""The launcher's two-screens mark: the games RG DS Play gives a second screen.

The mark is drawn on the cover of every game that has a companion profile, so
the profiles and the launcher must agree on which games those are.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / 'play/profiles'

SOURCE = r'''
#include "launcher/dual_screen.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rgds;

// A cartridge header: a Game Boy one has its title at 134h and its maker
// code at 13Fh, a Game Boy Advance one its title at A0h and its code at ACh.
// A profile names a game either by its maker code (AZLE) or by its header
// title (HARVEST_MOON_GB), so both are written and the launcher may use
// either.
static void write_rom(const fs::path& path, const std::string& title, const std::string& code, bool gba) {
    std::vector<unsigned char> header(0x150, 0);
    const int title_at = gba ? 0xA0 : 0x134, code_at = gba ? 0xAC : 0x13F;
    const int title_max = gba ? 12 : 16;
    for (size_t i = 0; i < code.size() && i < 4; i++) header[code_at + i] = (unsigned char)code[i];
    for (size_t i = 0; i < title.size() && i < (size_t)title_max; i++) header[title_at + i] = (unsigned char)title[i];
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)header.data(), (std::streamsize)header.size());
}

int main(int argc, char** argv) {
    fs::path folder = argv[1];
    fs::path profiles = argv[2];
    // A cart with a maker code the profiles know.
    write_rom(folder / "coded.gbc", "ZELDA", "AZLE", false);
    assert(dual_screen_game(folder / "coded.gbc", "gbc", profiles));
    // The same code for a plain Game Boy cart.
    write_rom(folder / "coded.gb", "ZELDA", "AZLE", false);
    assert(dual_screen_game(folder / "coded.gb", "gb", profiles));
    // A cart that only names itself in its title.
    write_rom(folder / "named.gb", "SAGA", "", false);
    assert(dual_screen_game(folder / "named.gb", "gb", profiles));
    // A cart no profile knows.
    write_rom(folder / "other.gb", "TETRIS", "", false);
    assert(!dual_screen_game(folder / "other.gb", "gb", profiles));
    // A Game Boy Advance cart keeps its code at ACh.
    write_rom(folder / "advance.gba", "POKEMON", "AXPE", true);
    assert(dual_screen_game(folder / "advance.gba", "gba", profiles));
    // A DS game has its own two screens; the mark is for the other systems.
    write_rom(folder / "ds.nds", "ZELDA", "AZLE", false);
    assert(!dual_screen_game(folder / "ds.nds", "nds", profiles));
    // An archive cannot be opened to see its header, so it shows no mark.
    { std::ofstream zip(folder / "zipped.zip", std::ios::binary); zip << "PK"; }
    assert(!dual_screen_game(folder / "zipped.zip", "gb", profiles));
    // Every shipped profile must find its own game.
    for (const auto& system : {std::string("gb"), std::string("gba")}) {
        fs::path dir = profiles / system;
        if (!fs::is_directory(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".ini") continue;
            std::ifstream file(entry.path());
            std::string line, title, codes;
            while (std::getline(file, line)) {
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                std::string value = line.substr(eq + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                if (key == "title") title = value;
                if (key == "codes") codes = value;
            }
            if (codes.empty()) continue;
            std::istringstream words(codes);
            std::string code;
            while (words >> code) {
                fs::path rom = folder / (system + "-" + code + (system == "gba" ? ".gba" : ".gbc"));
                // The name the profile gives the game is the maker code
                // (AZLE) or the header title (HARVEST_MOON_GB); a cart
                // carries one or the other.
                write_rom(rom, code, code.size() <= 4 ? code : "", system == "gba");
                if (!dual_screen_game(rom, system, profiles)) {
                    std::fprintf(stderr, "profile %s does not find its own code %s\n",
                                 entry.path().filename().c_str(), code.c_str());
                    return 1;
                }
            }
        }
    }
    std::puts("ok");
    return 0;
}
'''


@unittest.skipUnless(shutil.which('clang++'), 'Host C++ compiler required')
class DualScreenTests(unittest.TestCase):
    def test_the_mark_finds_exactly_the_games_with_a_second_screen(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'roms').mkdir()
            (root / 'profiles').mkdir()
            for system in ('gb', 'gba'):
                (root / 'profiles' / system).mkdir()
                for profile in (PROFILES / system).glob('*.ini'):
                    (root / 'profiles' / system / profile.name).write_text(profile.read_text())
            source = root / 'dual.cpp'
            binary = root / 'dual'
            source.write_text(SOURCE)
            subprocess.run(
                ['clang++', '-std=c++17', '-I' + str(ROOT / 'src'), str(source), '-o', str(binary)],
                check=True, capture_output=True, text=True
            )
            result = subprocess.run(
                [str(binary), str(root / 'roms'), str(root / 'profiles')],
                capture_output=True, text=True, timeout=20
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
