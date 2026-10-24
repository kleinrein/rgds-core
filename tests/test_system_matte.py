"""The matte of a studio product photo: the machine, on transparency.

The rules are tried against a given alpha, so these tests do not need a
segmentation model and do not need rembg.
"""
import importlib.util
from pathlib import Path
import unittest

try:
    import numpy as np
    from PIL import Image
except ImportError:  # the matte's tools live in .cache/venv, not in the test environment
    np = None
    Image = None

ROOT = Path(__file__).resolve().parents[1]


def matte_module():
    spec = importlib.util.spec_from_file_location('system_matte', ROOT / 'tools/system_matte.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def picture(size=256, studio=200, body=(90, 110, 170), hole=None, hole_alpha=0):
    """A picture of a machine on a studio backdrop.

    The machine is a filled square in the middle of the picture; the studio
    around it is what the segmentation model called background. The hole is
    a square cut out of the machine, either studio-coloured (a real opening)
    or in the colour of the machine (a part the segmentation model lost).
    """
    pixels = np.zeros((size, size, 3), np.uint8)
    pixels[:, :] = studio
    pixels[64:192, 64:192] = body
    alpha = np.zeros((size, size), np.uint8)
    alpha[64:192, 64:192] = 255
    if hole is not None:
        pixels[112:144, 112:144] = hole
        alpha[112:144, 112:144] = hole_alpha
    return Image.fromarray(pixels, 'RGB'), alpha


@unittest.skipUnless(np is not None and Image is not None, 'numpy and Pillow required')
class SystemMatteTest(unittest.TestCase):
    def setUp(self):
        self.matte = matte_module()

    def test_the_studio_is_taken_away(self):
        image, alpha = picture()
        cut, _kind = self.matte.matte(image, None, alpha=alpha)
        out = np.asarray(cut)[:, :, 3]
        self.assertEqual(int(out[0, 0]), 0, 'the studio is not transparent')
        self.assertEqual(int(out[128, 128]), 255, 'the machine is not opaque')

    def test_a_part_of_the_machine_the_model_lost_is_closed(self):
        # The hole shows the machine's own colour: the model lost it.
        image, alpha = picture(hole=(90, 110, 170), hole_alpha=0)
        cut, _kind = self.matte.matte(image, None, alpha=alpha)
        out = np.asarray(cut)[:, :, 3]
        self.assertEqual(int(out[128, 128]), 255, 'the lost part was not closed')
        colour = np.asarray(cut)[:, :, :3]
        self.assertEqual(tuple(colour[128, 128]), (90, 110, 170), 'the hole was not closed with the photograph')

    def test_a_real_opening_stays_open(self):
        # The hole shows the studio through it: a real opening.
        image, alpha = picture(hole=(200, 200, 200), hole_alpha=0)
        cut, _kind = self.matte.matte(image, None, alpha=alpha)
        out = np.asarray(cut)[:, :, 3]
        self.assertEqual(int(out[128, 128]), 0, 'the opening was closed')

    def test_the_second_model_can_veto_the_studio(self):
        # The first model keeps a patch of the studio, the second does not.
        image, alpha = picture()
        other = alpha.copy()
        other[100:160, 100:160] = 0
        pixels = np.asarray(image).copy()
        pixels[100:160, 100:160] = 200
        image = Image.fromarray(pixels, 'RGB')
        cut, _kind = self.matte.matte(image, None, alpha=alpha, alpha2=other)
        out = np.asarray(cut)[:, :, 3]
        self.assertEqual(int(out[130, 130]), 0, 'the studio the models disagreed about was kept')

    def test_the_edge_takes_its_colour_from_the_machine(self):
        image, alpha = picture()
        cut, _kind = self.matte.matte(image, None, alpha=alpha)
        colour = np.asarray(cut)[:, :, :3]
        # The edge of the machine is the colour of the machine, not of the studio.
        self.assertEqual(tuple(colour[64, 128]), (90, 110, 170))


if __name__ == '__main__':
    unittest.main()
