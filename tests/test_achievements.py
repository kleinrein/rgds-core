"""RetroAchievements account and the player's way to the site (runtime/achievements.py), against a fake site."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import achievements  # noqa: E402


class Reply(io.BytesIO):
    def __init__(self, body, status=200):
        super().__init__(body)
        self.status = status


class FakeSite:
    def __init__(self, answers):
        self.answers = answers
        self.requests = []

    def __call__(self, request, timeout=None):
        self.requests.append(request)
        answer = self.answers(request)
        if isinstance(answer, Exception):
            raise answer
        return answer


class AccountTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.data = Path(self.temp.name) / "userdata"
        self.private = Path(self.temp.name) / "private"

    def tearDown(self):
        self.temp.cleanup()

    def test_login_keeps_the_token_not_the_password(self):
        site = FakeSite(
            lambda r: Reply(json.dumps({
                "Success": True,
                "User": "Tester",
                "Token": "tok",
                "Score": 12
            }).encode())
        )
        name = achievements.login(self.data, "tester", "s3cret pass", self.private, opener=site)
        self.assertEqual(name, "Tester")
        sent = site.requests[0].data.decode()
        self.assertIn("r=login2", sent)
        self.assertIn("p=s3cret+pass", sent)
        self.assertTrue(site.requests[0].full_url.startswith("https://retroachievements.org/"))
        self.assertEqual(achievements.read_account(self.data, self.private), {"user": "Tester", "token": "tok"})
        self.assertEqual((self.private / achievements.TOKEN).read_text(), "Tester\ntok\n")
        for path in self.private.iterdir():
            self.assertNotIn("s3cret", path.read_text())
        self.assertIn("user=Tester", (self.data / "achievements/account.ini").read_text())
        achievements.logout(self.data, self.private)
        self.assertEqual(achievements.read_account(self.data, self.private), {})
        self.assertFalse((self.private / achievements.TOKEN).exists())
        self.assertFalse((self.data / "achievements/account.ini").exists())

    def test_wrong_password(self):
        refused = urllib.error.HTTPError(
            "u", 401, "no", {}, io.BytesIO(b'{"Success":false,"Code":"invalid_credentials"}')
        )
        with self.assertRaisesRegex(achievements.AchievementsError, "Wrong username or password"):
            achievements.login(self.data, "t", "p", self.private, opener=FakeSite(lambda r: refused))
        self.assertEqual(achievements.read_account(self.data, self.private), {})

    def test_offline(self):
        with self.assertRaisesRegex(achievements.AchievementsError, "Could not reach"):
            achievements.login(self.data, "t", "p", self.private, opener=FakeSite(lambda r: OSError("down")))

    def test_needs_both_fields(self):
        with self.assertRaises(achievements.AchievementsError):
            achievements.login(self.data, "t", "", self.private, opener=FakeSite(lambda r: Reply(b"{}")))


class ServeTest(unittest.TestCase):
    def test_answers_are_framed_and_badges_saved_inside_the_folder(self):
        with tempfile.TemporaryDirectory() as folder:
            badges = Path(folder) / "badges"
            badges.mkdir()

            def answer(request):
                if request.full_url.endswith(".png"):
                    return Reply(b"\x89PNG")
                return Reply(b"{\"Success\":true}\n2nd line" + (request.data or b""))

            site = FakeSite(answer)
            lines = (
                "1\thttps://retroachievements.org/dorequest.php\tr=login2&u=a\tapplication/x-www-form-urlencoded\t\n"
                "2\thttps://media.retroachievements.org/Badge/1.png\t\t\t%s\n"
                "3\thttps://example.com/x\t\t\t\n"
                "4\thttps://media.retroachievements.org/Badge/2.png\t\t\t%s\n" %
                (badges / "1.png", Path(folder) / "outside.png")
            )
            out = io.BytesIO()
            achievements.serve(badges, io.BytesIO(lines.encode()), out, opener=site, workers=1)
            answers, data = {}, out.getvalue()
            while data:
                header, _, data = data.partition(b"\n")
                ident, status, length = header.split()
                answers[ident.decode()] = (int(status), data[:int(length)])
                data = data[int(length):]
            self.assertEqual(answers["1"], (200, b"{\"Success\":true}\n2nd liner=login2&u=a"))
            self.assertEqual(answers["2"], (200, b""))
            self.assertEqual((badges / "1.png").read_bytes(), b"\x89PNG")
            self.assertEqual(answers["3"], (0, b""))  # not RetroAchievements: never fetched
            self.assertEqual(answers["4"], (0, b""))  # outside the badges folder: never written
            self.assertFalse((Path(folder) / "outside.png").exists())
            self.assertEqual(len(site.requests), 2)


if __name__ == "__main__":
    unittest.main()
