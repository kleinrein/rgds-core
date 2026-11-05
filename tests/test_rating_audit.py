import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('rating_audit', ROOT / 'tools/rating_audit.py')
rating_audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rating_audit)


class MinimumAgeTest(unittest.TestCase):
    def test_every_rating_the_catalogs_hold(self):
        # Every value in GameTDB's DS and 3DS lists and Libretro's ESRB lists on 2026-11-05.
        expected = {
            'ESRB EC': 3,
            'ESRB E': 3,
            'ESRB KA': 3,
            'ESRB E10+': 10,
            'ESRB T': 13,
            'ESRB M': 17,
            'PEGI 3': 3,
            'PEGI 6': 6,
            'PEGI 7': 7,
            'PEGI 12': 12,
            'PEGI 16': 16,
            'PEGI 18': 18,
            'CERO A': 3,
            'CERO B': 12,
            'CERO C': 15,
            'CERO D': 17,
            'CERO Z': 18,
            'GRB ALL': 3,
            'GRB 12': 12,
            'GRB 15': 15,
            'GRB 18': 18,
            'CSRR ALL': 3
        }
        for rating, age in expected.items():
            self.assertEqual(rating_audit.minimum_age(rating), age, rating)

    def test_no_usable_rating_counts_as_unrated(self):
        for rating in ('', None, 'ESRB', 'PEGI', 'CERO', 'GRB', 'CSRR', 'ESRB NOT RATED', 'ESRB RP', 'NEW 7'):
            self.assertIsNone(rating_audit.minimum_age(rating), rating)


class SameTableTest(unittest.TestCase):
    def test_the_launcher_reads_every_rating_as_the_runtime_does(self):
        import shutil, subprocess, sys, tempfile
        if not shutil.which('clang++'): self.skipTest('Requires host C++')
        sys.path.insert(0, str(ROOT / 'runtime'))
        from age_rating import BOARDS, minimum_age
        ratings = [board + ' ' + value for board, values in BOARDS.items() for value in values]
        ratings += [
            '', 'ESRB', 'PEGI', 'CERO', 'GRB', 'CSRR', 'USK', 'ESRB NOT RATED', 'ESRB RP', 'NEW 7', 'PEGI 99', 'ESRB '
        ]
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'driver.cpp'
            source.write_text(
                '#include "launcher/age_rating.hpp"\n#include <iostream>\n'
                'int main() { std::string line; while (std::getline(std::cin, line)) '
                'std::cout << rgds::minimum_age(line) << "\\n"; }\n'
            )
            binary = Path(folder) / 'driver'
            subprocess.run(
                ['clang++', '-std=c++17', '-I' + str(ROOT / 'src'),
                 str(source), '-o', str(binary)], check=True
            )
            result = subprocess.run([str(binary)], input='\n'.join(ratings) + '\n', capture_output=True, text=True)
        expected = [minimum_age(rating) if minimum_age(rating) is not None else -1 for rating in ratings]
        self.assertEqual([int(line) for line in result.stdout.split()], expected)


class RowTest(unittest.TestCase):
    def test_an_empty_board_is_unrated_and_counted(self):
        row = rating_audit.Row()
        row.add('Rated', 'PEGI 3')
        row.add('Board only', 'PEGI')
        row.add('Nothing', '')
        self.assertEqual(row.games, 3)
        self.assertEqual(row.ages[3], 1)
        self.assertEqual(sorted(row.unrated), ['Board only', 'Nothing'])
        self.assertEqual(row.empty_board, 1)


if __name__ == '__main__':
    unittest.main()
