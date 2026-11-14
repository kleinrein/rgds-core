"""The Storage view's measuring helper: kinds of content, partition edges, and no background work."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
import storage_usage  # noqa: E402


def make(root, name, size):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\0' * size)


class StorageUsageTests(unittest.TestCase):
    def test_each_kind_of_content_is_counted_where_it_belongs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, size in (
                ('roms/nds/a.nds', 300000), ('ports/p/game.bin', 20000), ('music/m.mp3', 50000),
                ('rgds/userdata/cache/c', 7000), ('rgds/userdata/library/k/box.bmp', 9000),
                ('rgds/userdata/saves/s.sav', 3000), ('rgds/userdata/save-history/h', 2000),
                ('rgds/userdata/preferences.ini', 100), ('rgds/app/engine', 40000), ('stray.txt', 500)
            ):
                make(root, name, size)
            totals = storage_usage.scan(root / 'rgds/userdata', lambda *_: None, root=root)
        self.assertGreaterEqual(totals['games'], 320000)
        self.assertLess(totals['games'], 360000)
        self.assertGreaterEqual(totals['music'], 50000)
        self.assertGreaterEqual(totals['cache'], 7000)
        self.assertGreaterEqual(totals['artwork'], 9000)
        self.assertGreaterEqual(totals['saves'], 5000)
        self.assertGreaterEqual(totals['system'], 40000)
        self.assertGreaterEqual(totals['other'], 500)
        self.assertLess(totals['cache'], 20000)  # the cache is not counted as saves, system or anything else

    def test_progress_is_published_while_walking_and_a_final_report_ends_it(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for number in range(20):
                make(root, f'roms/gba/{number}.gba', 1000)
            seen = []
            ticks = iter(range(0, 1000))
            storage_usage.scan(
                root / 'rgds/userdata',
                lambda totals, done: seen.append(done),
                root=root,
                interval=5,
                clock=lambda: next(ticks)
            )
        self.assertGreater(len(seen), 2)
        self.assertEqual(seen[-1], True)
        self.assertNotIn(True, seen[:-1])

    def test_a_missing_folder_reports_zero_instead_of_failing(self):
        totals = storage_usage.scan('/nonexistent/rgds/userdata', lambda *_: None, root='/nonexistent')
        self.assertEqual(sum(totals.values()), 0)


if __name__ == '__main__':
    unittest.main()
