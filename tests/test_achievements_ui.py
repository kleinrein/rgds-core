"""RetroAchievements in the launcher: the Library page, the sign-in worker and the Game info list's file."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"

FAKE_SCRIPT = r'''import json, sys
from pathlib import Path
data = Path(sys.argv[sys.argv.index("--data") + 1])
folder = data / "achievements"
folder.mkdir(parents=True, exist_ok=True)
with (folder / "calls").open("a") as calls:
    calls.write(json.dumps({"argv": sys.argv[1:], "stdin": sys.stdin.read()}) + "\n")
if sys.argv[1] == "login":
    (folder / "account.ini").write_text("user=Ada\n")
    (folder / "status.ini").write_text("state=ready\nmessage=Signed in to RetroAchievements as Ada.\n")
else:
    (folder / "account.ini").unlink(missing_ok=True)
    (folder / "status.ini").write_text("state=ready\nmessage=Signed out of RetroAchievements.\n")
'''


@unittest.skipUnless(BINARY.exists(), "Build the host launcher first")
class AchievementsPageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / "app"
        self.data = self.root / "data"
        (self.app / "runtime").mkdir(parents=True)
        self.data.mkdir()
        for name in ("assets", "themes"):
            (self.app / name).symlink_to(ROOT / name, target_is_directory=True)
        (self.app / "runtime/achievements.py").write_text(FAKE_SCRIPT)
        self.state = self.root / "state.txt"
        self.state.write_text('1 0 0 ""\n')
        self.prefs = self.root / "preferences.ini"
        self.ui = self.root / "ui.ini"
        self.ui.write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")

    def run_ui(self, script, seconds=2):
        path = self.root / "input.txt"
        path.write_text(script)
        result = subprocess.run(
            [
                str(BINARY), "--preview", "--app",
                str(self.app), "--data",
                str(self.data), "--preferences",
                str(self.prefs), "--state",
                str(self.state), "--input-script",
                str(path), "--seconds",
                str(seconds), "--settings", "--section", "achievements"
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy"),
            capture_output=True,
            text=True,
            timeout=15
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def calls(self):
        path = self.data / "achievements/calls"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def test_the_switch_is_saved_per_player_and_off_by_default(self):
        self.run_ui("")
        self.assertNotIn("achievements=1", self.ui.read_text() if self.ui.exists() else "")
        self.run_ui("20 key A\n")
        self.assertIn("achievements=1\n", self.ui.read_text())
        self.run_ui("20 key Left\n")
        self.assertIn("achievements=0\n", self.ui.read_text())

    def test_sign_in_asks_name_then_password_and_sends_them_on_stdin(self):
        self.run_ui("20 key Down\n60 key A\n120 text Ada\n160 key Return\n220 text hunter2\n260 key Return\n")
        calls = self.calls()
        self.assertEqual(len(calls), 1, calls)
        self.assertEqual(calls[0]["argv"][0], "login")
        self.assertNotIn("hunter2", " ".join(calls[0]["argv"]))  # never on the command line
        self.assertEqual(json.loads(calls[0]["stdin"]), {"user": "Ada", "password": "hunter2"})
        self.assertEqual(calls[0]["argv"][calls[0]["argv"].index("--data") + 1], str(self.data))
        self.assertTrue((self.data / "achievements/worker.log").exists())
        # A successful sign-in switches achievements on.
        self.assertIn("achievements=1\n", self.ui.read_text())
        # The ScreenScraper account is untouched.
        self.assertFalse((self.data / "scraper/account.ini").exists())

    def test_signed_in_account_offers_sign_out(self):
        (self.data / "achievements").mkdir()
        (self.data / "achievements/account.ini").write_text("user=Ada\n")
        # Account row, A opens Change account / Sign out; Down, A signs out.
        self.run_ui("20 key Down\n60 key A\n140 key Down\n180 key A\n")
        calls = self.calls()
        self.assertEqual([call["argv"][0] for call in calls], ["logout"])
        self.assertFalse((self.data / "achievements/account.ini").exists())

    def test_back_lands_on_the_library_row(self):
        self.run_ui("20 key Escape\n")
        page, section, focus = (int(value) for value in self.state.read_text().splitlines()[1].split()[:3])
        self.assertEqual((page, section), (6, 2))  # Page::LibraryOptions, the Library section
        self.assertEqual(focus, 9)  # RetroAchievements, the last of ten rows while Games opens is hidden


@unittest.skipUnless(shutil.which("clang++") and shutil.which("sdl2-config"), "C++/SDL headers required")
class AchievementsFileTests(unittest.TestCase):
    def test_parser_reads_totals_sorts_unlocked_first_and_skips_unknown_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "achievements.tsv").write_text(
                "#rgds-achievements 1\n"
                "game\t1234\tSuper Game\t2\t3\t15\t40\tAda\t1763500000\n"
                "ach\t1\t0\t25\t0\t\tLocked one\tDo it\t\t3/10\n"
                "ach\t2\t1\t5\t1763400000\t12.5\tEarly\tFirst\t/b/2.png\t\n"
                "future\tsomething new\n"
                "ach\t3\t1\t10\t1763450000\t\tLater\tSecond\t\t\n"
                "ach\tshort\n",
                encoding="utf-8"
            )
            (folder / "empty").mkdir()
            (folder / "empty/achievements.tsv").write_text("#rgds-achievements 1\ngame\t9\tNone\t0\t0\t0\t0\tAda\t1\n")
            (folder / "bad").mkdir()
            (folder / "bad/achievements.tsv").write_text("game\t9\tNo header\t0\t0\t0\t0\tAda\t1\n")
            source = folder / "test.cpp"
            binary = folder / "test"
            source.write_text(
                r'''
#include "launcher/game_info.hpp"
#include <cassert>
int main(int argc, char** argv) {
    const rgds::fs::path root = argv[1];
    auto a = rgds::load_game_achievements(root);
    assert(a.known && a.game_id == "1234" && a.title == "Super Game" && a.account == "Ada");
    assert(a.unlocked == 2 && a.total == 3 && a.points == 15 && a.points_total == 40 && a.written == 1763500000);
    assert(a.list.size() == 3);
    assert(a.list[0].id == "3" && a.list[1].id == "2" && a.list[2].id == "1"); // unlocked, latest first
    assert(a.list[1].badge == "/b/2.png" && a.list[1].rarity == "12.5" && a.list[1].points == 5);
    assert(!a.list[2].unlocked && a.list[2].progress == "3/10" && a.list[2].description == "Do it");
    auto none = rgds::load_game_achievements(root / "empty");
    assert(none.known && none.total == 0 && none.list.empty());
    assert(!rgds::load_game_achievements(root / "bad").known);
    assert(!rgds::load_game_achievements(root / "missing").known);
}
'''
            )
            subprocess.run(
                [
                    "clang++", "-std=c++17", "-pthread",
                    *shlex.split(subprocess.check_output(["sdl2-config", "--cflags"], text=True)),
                    "-I" + str(ROOT / "src"),
                    str(source), "-o",
                    str(binary)
                ],
                check=True
            )
            subprocess.run([str(binary), str(folder)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
