"""DS favourite-colour accents stay readable on both built-in palettes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'Host C++ compiler required')
class AccentTests(unittest.TestCase):
    def test_every_colour_meets_contrast_on_light_and_dark_surfaces(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'accent.cpp';binary = Path(folder) / 'accent'
            source.write_text(r'''
#include "launcher/accent.hpp"
#include <cassert>
#include <cstdio>
using namespace rgds;
int main(){
    for(bool dark:{false,true}){
        for(int i=-1;i<16;i++){
            Theme theme=dark?default_dark_theme():default_theme();const Theme original=theme;
            apply_accent(theme,i,dark);
            if(i<0){assert(theme.at("accent").r==original.at("accent").r && theme.at("focus").g==original.at("focus").g);continue;}
            assert(contrast_ratio(theme.at("accent"),theme.at("background"))>=3.0);
            assert(contrast_ratio(theme.at("accent"),theme.at("panel"))>=3.0);
            assert(contrast_ratio(theme.at("on_accent"),theme.at("accent"))>=3.0);
            assert(contrast_ratio(theme.at("ink"),theme.at("focus"))>=4.5);
        }
    }
    Theme theme=default_theme();apply_accent(theme,16,false);apply_accent(theme,-2,false);
    assert(theme.at("accent").r==default_theme().at("accent").r);
    std::puts("ok");
}
''')
            subprocess.run(['clang++', '-std=c++17', '-I' + str(ROOT / 'src'), str(source), '-o', str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':unittest.main()
