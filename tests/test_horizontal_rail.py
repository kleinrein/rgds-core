"""Rail contact feedback and cleanup through the native SDL renderer."""
import unittest
import test_launch_feedback as feedback


@unittest.skipUnless(feedback.BINARY.exists(), 'Build host launcher first')
class HorizontalRailTests(unittest.TestCase):
    exercise = feedback.FeedbackUITests.exercise
    crop = feedback.FeedbackUITests.crop

    def test_thumb_grab_feedback_survives_disabled_indicator_and_cleans_up(self):
        for mode in ('light', 'dark'):
            for animations in (True, False):
                with self.subTest(mode=mode, animations=animations):
                    options = dict(mode=mode, animations=animations, indicator=False)
                    base, _, _ = self.exercise(**options)
                    held, counts, state = self.exercise('100 tap 126 443\n', **options)
                    self.assertEqual(int(state.split()[1]), 0, 'Grabbing the thumb edge must not seek')
                    footer = lambda image: self.crop(image, 84, 887, 472, 73)
                    self.assertNotEqual(footer(base), footer(held))
                    self.assertLess(counts['frames'], 40, 'A held thumb must settle back to idle')
                    for end in ('release', 'focus-lost'):
                        released, _, _ = self.exercise(f'100 tap 126 443\n300 {end}\n', **options)
                        self.assertEqual(footer(base), footer(released), end)

    def test_caret_press_has_feedback_and_release_restores_surface(self):
        for x in (42, 598):
            with self.subTest(x=x):
                options = dict(indicator=False)
                base, _, _ = self.exercise(**options)
                held, _, _ = self.exercise(f'100 tap {x} 443\n', **options)
                button = lambda image: self.crop(image, 0 if x == 42 else 572, 889, 68, 71)   # the arrow button alone; the grabber starts at 74
                self.assertNotEqual(button(base), button(held))
                for end in ('release', 'focus-lost', 'drag 320 370'):
                    released, _, _ = self.exercise(f'100 tap {x} 443\n300 {end}\n', **options)
                    self.assertEqual(button(base), button(released), end)

    def test_captured_thumb_remains_pressed_outside_rail_without_launching(self):
        options = dict(indicator=False, animations=False)
        held, _, _ = self.exercise('100 tap 126 443\n200 drag 700 370\n', **options)
        released, _, state = self.exercise('100 tap 126 443\n200 drag 700 370\n300 release\n', **options)
        self.assertEqual(int(state.split()[1]), 4)
        self.assertNotEqual(self.crop(held, 84, 887, 472, 73), self.crop(released, 84, 887, 472, 73))


if __name__ == '__main__':
    unittest.main()
