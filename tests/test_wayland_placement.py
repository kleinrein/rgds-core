"""Fullscreen requests and renderer buffers are insufficient routing evidence."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_image_wayland import panel_placement

SETUP = '''wl_output#11.geometry(0, 0, 640, 480, 0, "wayland", "none", 0)
wl_output#13.geometry(640, 0, 640, 480, 0, "wayland", "none", 0)
xdg_wm_base#15.get_xdg_surface(new id xdg_surface#20, wl_surface#17)
xdg_surface#20.get_toplevel(new id xdg_toplevel#27)
xdg_toplevel#27.set_app_id("org.rgds.top")
xdg_wm_base#15.get_xdg_surface(new id xdg_surface#36, wl_surface#31)
xdg_surface#36.get_toplevel(new id xdg_toplevel#37)
xdg_toplevel#37.set_app_id("org.rgds.bottom")
xdg_toplevel#27.set_fullscreen(wl_output#13)
xdg_toplevel#37.set_fullscreen(wl_output#11)
'''


class PlacementTests(unittest.TestCase):
    def test_each_surface_is_visible_on_its_own_output_until_close(self):
        log = SETUP + 'wl_surface#17.enter(wl_output#13)\nwl_surface#31.enter(wl_output#11)\nwl_surface#17.destroy()\n'
        self.assertEqual(panel_placement(log), dict(top=[(640, 0)], bottom=[(0, 0)]))

    def test_two_windows_on_lower_output_fail_despite_correct_requests(self):
        log = SETUP + 'wl_surface#17.enter(wl_output#11)\nwl_surface#31.enter(wl_output#11)\nwl_surface#17.destroy()\n'
        with self.assertRaisesRegex(ValueError, 'assigned output'):
            panel_placement(log)

    def test_hidden_or_spanning_top_window_is_not_a_working_panel(self):
        for change in ('wl_surface#17.leave(wl_output#13)', 'wl_surface#17.enter(wl_output#11)'):
            log = SETUP + 'wl_surface#17.enter(wl_output#13)\nwl_surface#31.enter(wl_output#11)\n' + change + '\nwl_surface#17.destroy()\n'
            with self.assertRaises(ValueError):
                panel_placement(log)


if __name__ == '__main__': unittest.main()
