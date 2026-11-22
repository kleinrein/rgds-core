"""The merged pad's sticks (src/pad/rgds_pad_axes.h, used by rgds-pad) must report what ROCKNIX's
rocknix-singleadc-joypad driver reported for the same ADC readings, so the RG DS's sticks feel the same after the
move to mainline's input drivers (docs/OWN_BSP_PLAN.md, Phase C step 2). The header is compiled here with the host's
C compiler and checked against a separate Python port of the driver's arithmetic (joypad_adc_check)."""
import itertools, re, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'src/pad/rgds_pad_axes.h'

SCALE, MAX, DEADZONE, TUNING = 2, 1800, 64, 250


def c_div(a, b):
    """C's integer division, which truncates toward zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def isqrt(x):
    r = 0
    while (r + 1) * (r + 1) <= x:
        r += 1
    return r


def driver(raw_x, raw_y, cal_x, cal_y):
    """joypad_adc_check for one stick: (x, y) before the per-axis inversion, or None for a skipped read."""
    if raw_x == 0 or raw_y == 0:
        return None
    x, y = raw_x * SCALE - cal_x, raw_y * SCALE - cal_y
    mag = isqrt(x * x + y * y)
    if mag <= DEADZONE:
        x = y = 0
    else:
        x = c_div(c_div(MAX * x, mag) * (mag - DEADZONE), MAX - DEADZONE)
        y = c_div(c_div(MAX * y, mag) * (mag - DEADZONE), MAX - DEADZONE)
    tune = lambda v: c_div(v * TUNING, 100) if v else 0
    clamp = lambda v: max(-MAX, min(MAX, v))
    return clamp(tune(x)), clamp(tune(y))


HARNESS = r'''
#include <stdio.h>
#include "rgds_pad_axes.h"
int main(int argc, char **argv) {
	(void)argv;
	if (argc > 1) { /* the mux-state table */
		for (int s = 0; s < 4; s++)
			printf("%d %d %d\n", rgds_pad_state_code[s], rgds_pad_state_axis[s], rgds_pad_state_invert[s]);
		printf("%d %d %d %d\n", rgds_pad_stick_states[0][0], rgds_pad_stick_states[0][1], rgds_pad_stick_states[1][0],
		       rgds_pad_stick_states[1][1]);
		return 0;
	}
	int rx, ry, cx, cy;
	while (scanf("%d %d %d %d", &rx, &ry, &cx, &cy) == 4) {
		struct rgds_pad_axis x = {0, 0, cx, 111}, y = {1, 1, cy, 222};
		int ok = rgds_pad_stick(&x, rx, &y, ry);
		if (ok) printf("%d %d %d %d\n", x.value, y.value, rgds_pad_report(&x), rgds_pad_report(&y));
		else printf("skip %d %d\n", x.value, y.value);
	}
	return 0;
}
'''


@unittest.skipIf(shutil.which('cc') is None, 'no C compiler')
class PadSticksTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name, 'harness.c')
        src.write_text(HARNESS)
        cls.binary = Path(cls.tmp.name, 'harness')
        subprocess.run(
            ['cc', '-std=c99', '-Wall', '-Werror', '-I',
             str(HEADER.parent),
             str(src), '-o',
             str(cls.binary)],
            check=True
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cases(self, cases):
        text = ''.join(f'{a} {b} {c} {d}\n' for a, b, c, d in cases)
        out = subprocess.run([str(self.binary)], input=text, capture_output=True, text=True, check=True).stdout
        return out.splitlines()

    def test_matches_the_driver_across_the_whole_range(self):
        cal = (512 * SCALE, 498 * SCALE)
        raws = list(range(1, 1024, 37)) + [1, 15, 480, 498, 505, 512, 519, 530, 1023]
        cases = [(x, y, *cal) for x, y in itertools.product(raws, raws)]
        for (rx, ry, cx, cy), line in zip(cases, self.run_cases(cases)):
            want = driver(rx, ry, cx, cy)
            x, y, report_x, report_y = map(int, line.split())
            self.assertEqual((x, y), want, (rx, ry))
            self.assertEqual((report_x, report_y), (x, -y), 'the second axis is inverted')

    def test_rest_and_the_deadzone_report_zero(self):
        # 31 raw units = 62 scaled, inside the 64 deadzone; 33 = 66, just outside
        lines = self.run_cases([(512, 512, 1024, 1024), (543, 512, 1024, 1024), (545, 512, 1024, 1024)])
        self.assertEqual(lines[0].split()[:2], ['0', '0'])
        self.assertEqual(lines[1].split()[:2], ['0', '0'])
        self.assertNotEqual(lines[2].split()[0], '0')

    def test_full_deflection_is_clamped_to_the_range(self):
        for line in self.run_cases([(1023, 512, 1024, 1024), (1, 512, 1024, 1024), (1023, 1023, 1024, 1024)]):
            x, y = map(int, line.split()[:2])
            self.assertLessEqual(abs(x), MAX)
            self.assertLessEqual(abs(y), MAX)
        self.assertEqual(self.run_cases([(1023, 512, 1024, 1024)])[0].split()[:2], [str(MAX), '0'])

    def test_mux_states_map_to_the_axes_rocknix_reported(self):
        """Derived again from ROCKNIX's driver: its select switch, converted to gpio-mux states (sel_a is bit 0,
        sel_b bit 1), its amux-channel-mapping <1 0 2 3>, report types by index and the RG DS's inversions."""
        driver = (ROOT / 'os/buildroot/board/rgds/kernel/rocknix-singleadc-joypad.c').read_text()
        switch = driver[driver.index('static int joypad_amux_select'):]
        switch = switch[:switch.index('default:')]
        cases = re.findall(r'case (\d):.*?sel_a_gpio, (\d)\);.*?sel_b_gpio, (\d)\);', switch, re.S)
        state_of_channel = {int(c): int(a) | int(b) << 1 for c, a, b in cases}
        self.assertEqual(len(state_of_channel), 4)
        ABS = {'X': 0, 'Y': 1, 'RX': 3, 'RY': 4}
        mapping = [1, 0, 2, 3]  # amux-channel-mapping in the RG DS tree for ROCKNIX's driver
        report = ['RY', 'RX', 'Y', 'X']  # the driver's report type per adc index
        inverted = {'X', 'Y', 'RY'}  # invert-absx, invert-absy, invert-absry
        want_axis, want_invert = [None] * 4, [None] * 4
        for index in range(4):
            state = state_of_channel[mapping[index]]
            want_axis[state] = ABS[report[index]]
            want_invert[state] = int(report[index] in inverted)
        want_pairs = [
            state_of_channel[mapping[0]], state_of_channel[mapping[1]], state_of_channel[mapping[2]],
            state_of_channel[mapping[3]]
        ]
        lines = subprocess.run([str(self.binary), 'table'], capture_output=True, text=True,
                               check=True).stdout.split('\n')
        rows = [list(map(int, line.split())) for line in lines[:4]]
        self.assertEqual(
            [r[0] for r in rows], [ABS['X'], ABS['Y'], ABS['RX'], ABS['RY']], 'adc-joystick axis per state'
        )
        self.assertEqual([r[1] for r in rows], want_axis)
        self.assertEqual([r[2] for r in rows], want_invert)
        self.assertEqual(list(map(int, lines[4].split())), want_pairs)

    def test_a_failed_read_keeps_the_last_values(self):
        self.assertEqual(self.run_cases([(0, 700, 1024, 1024), (700, 0, 1024, 1024)]), ['skip 111 222'] * 2)


if __name__ == '__main__':
    unittest.main()
