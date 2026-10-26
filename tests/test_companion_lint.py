"""The companion rules (docs/COMPANION.md) on every shipped profile.

The static checks (sources, badges, labels, conditions, translatable words)
need nothing built; the drawing checks need play-lint:
python3 tools/build_play.py host
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
LINT = ROOT / 'build/play/host/play-lint'
spec = importlib.util.spec_from_file_location('lint_companions', ROOT / 'tools/lint_companions.py')
lint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lint)


class CompanionLintTests(unittest.TestCase):
    def test_every_profile_keeps_the_written_rules(self):
        english = lint.catalog(ROOT / 'assets/locales/en-US.tsv')
        catalogs = {
            p.stem: lint.catalog(p)
            for p in sorted((ROOT / 'assets/locales').glob('*.tsv')) if p.stem not in ('en-US', 'languages')
        }
        problems = []
        for path in sorted((ROOT / 'play/profiles').glob('*/*.ini')):
            problems += lint.check_profile(path, english, catalogs)
        self.assertEqual(problems, [])

    def test_the_rules_catch_what_they_say(self):
        english = {'Coins': 'Coins'}
        with self.subTest('badge, label and words'):
            path = ROOT / 'build/lint-sample.ini'
            path.parent.mkdir(exist_ok=True)
            path.write_text(
                '# A sample\n# from nowhere\n[game]\ntitle = T\ncodes = AB\n[view]\nformat = board\n'
                'tile1 = Coins | bcd A000 2 icon coin\ntile2 = A label far too long to fit | num A002 1\n'
            )
            try:
                problems = lint.check_profile(path, english, {'xx-XX': {}})
            finally:
                path.unlink()
            text = '\n'.join(problems)
            self.assertIn('badge', text)
            self.assertIn('label', text)
            self.assertIn("'Coins' is not translated in xx-XX", text)

    @unittest.skipUnless(LINT.exists(), 'Build RG DS Play first: tools/build_play.py host')
    def test_every_screen_is_drawn_by_the_rules(self):
        command = [str(LINT), '--fonts', str(ROOT / 'assets/fonts')]
        for system in sorted(p for p in (ROOT / 'play/profiles').iterdir() if p.is_dir()):
            command += ['--profiles', str(system)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:])
        self.assertIn(' 0 problems', result.stdout)


if __name__ == '__main__':
    unittest.main()
