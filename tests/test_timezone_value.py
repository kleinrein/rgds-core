import datetime
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime"))
from emulators import timezone_value  # noqa: E402

SUMMER = datetime.datetime(2026, 7, 1, 12, tzinfo=datetime.timezone.utc)
WINTER = datetime.datetime(2026, 1, 1, 12, tzinfo=datetime.timezone.utc)


class TimezoneValueTest(unittest.TestCase):
    def test_automatic_keeps_the_zone_name_so_tzdata_decides(self):
        self.assertEqual(timezone_value({"timezone": "Europe/Oslo"}, SUMMER), "Europe/Oslo")
        self.assertEqual(timezone_value({"timezone": "Europe/Oslo", "dst": "0"}, SUMMER), "Europe/Oslo")
        self.assertEqual(timezone_value({}, SUMMER), "Europe/Oslo")

    def test_forced_off_is_the_standard_offset_even_in_summer(self):
        off = {"timezone": "Europe/Oslo", "dst": "2"}
        self.assertEqual(timezone_value(off, SUMMER), "<+0100>-1")
        self.assertEqual(timezone_value(off, WINTER), "<+0100>-1")

    def test_forced_on_is_an_hour_ahead_of_standard_time(self):
        on = {"timezone": "Europe/Oslo", "dst": "1"}
        self.assertEqual(timezone_value(on, SUMMER), "<+0200>-2")
        self.assertEqual(timezone_value(on, WINTER), "<+0200>-2")

    def test_half_hour_and_western_offsets(self):
        self.assertEqual(timezone_value({"timezone": "Asia/Kathmandu", "dst": "2"}, SUMMER), "<+0545>-5:45")
        self.assertEqual(timezone_value({"timezone": "America/New_York", "dst": "2"}, SUMMER), "<-0500>+5")

    def test_utc_and_unknown_zones_are_left_alone(self):
        self.assertEqual(timezone_value({"timezone": "UTC", "dst": "1"}, SUMMER), "UTC")
        self.assertEqual(timezone_value({"timezone": "Nowhere/Land", "dst": "1"}, SUMMER), "Nowhere/Land")


if __name__ == "__main__":
    unittest.main()
