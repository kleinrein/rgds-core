"""The kernel fragments rgds_os_defconfig names must reach the built kernel (tools/own_os_build.py
kernel_config_drift): Buildroot merges them only when the kernel is configured, so an existing kernel build keeps
the old settings, and Kconfig itself can override a line (DW HDMI, selected by Rockchip's DRM, did on 2026-11-20).
docs/OWN_BSP_PLAN.md, Phase D."""
import sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import own_os_build

DEFCONFIG = (ROOT / 'os/buildroot/configs/rgds_os_defconfig').read_text()
FRAGMENTS = own_os_build.kernel_fragments(DEFCONFIG)


def settings():
    """Every CONFIG_ line the fragments set, in order (a later fragment wins, as in merge_config.sh)."""
    wanted = {}
    for fragment in FRAGMENTS:
        for line in fragment.read_text().splitlines():
            if line.startswith('CONFIG_') and '=' in line:
                name, value = line.split('=', 1)
                wanted[name] = value
    return wanted


def kernel_config(wanted):
    """A kernel .config as Kconfig writes it: '# CONFIG_X is not set' for the ones that are off."""
    return ''.join(f'# {k} is not set\n' if v == 'n' else f'{k}={v}\n' for k, v in wanted.items())


class KernelFragmentsTest(unittest.TestCase):
    def output(self, tmp, fragments_line=None, kernel=None, base_line=None):
        out = Path(tmp)
        line = fragments_line or next(
            l for l in DEFCONFIG.splitlines() if l.startswith('BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES=')
        )
        base = next(l for l in DEFCONFIG.splitlines() if l.startswith('BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE='))
        (out / '.config').write_text(line + '\n' + (base_line or base) + '\n')
        if kernel is not None:
            (out / 'build/linux-custom').mkdir(parents=True)
            (out / 'build/linux-custom/.config').write_text(kernel)
        return out

    def test_the_defconfig_names_both_fragments_and_they_exist(self):
        self.assertEqual([f.name for f in FRAGMENTS], ['linux-rgds.fragment', 'linux-board.fragment'])
        for fragment in FRAGMENTS:
            self.assertTrue(fragment.is_file(), fragment)

    def test_before_the_first_kernel_build_there_is_nothing_to_compare(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(own_os_build.kernel_config_drift(self.output(tmp)), [])

    def test_a_kernel_with_every_fragment_setting_matches(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.output(tmp, kernel=kernel_config(settings()))
            self.assertEqual(own_os_build.kernel_config_drift(out), [])

    def test_a_kernel_built_before_the_board_trim_is_caught(self):
        wanted = settings()
        old = dict(wanted, CONFIG_XFS_FS='y', CONFIG_NETFILTER='y')
        with tempfile.TemporaryDirectory() as tmp:
            drift = own_os_build.kernel_config_drift(self.output(tmp, kernel=kernel_config(old)))
        self.assertIn('CONFIG_XFS_FS=n (built: y)', drift)
        self.assertIn('CONFIG_NETFILTER=n (built: y)', drift)
        self.assertEqual(len(drift), 2)

    def test_a_symbol_the_kernel_lacks_counts_as_off(self):
        wanted = settings()
        missing = {k: v for k, v in wanted.items() if k != 'CONFIG_HID_TIVO'}
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(own_os_build.kernel_config_drift(self.output(tmp, kernel=kernel_config(missing))), [])

    def test_a_kept_output_config_with_other_fragments_stops_the_build(self):
        stale = 'BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_RGDS_PATH)/board/rgds/linux-rgds.fragment"'
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit) as raised:
                own_os_build.kernel_config_drift(self.output(tmp, fragments_line=stale))
        self.assertIn('--reconfigure', str(raised.exception))

    def test_the_defconfig_builds_on_our_own_kernel_config(self):
        base = own_os_build.kernel_base_config(DEFCONFIG)
        self.assertEqual(base.name, 'linux-rgds_defconfig')
        self.assertTrue(base.is_file(), base)

    def test_a_kept_output_config_on_the_old_base_stops_the_build(self):
        stale = 'BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="$(BR2_EXTERNAL_RGDS_PATH)/board/rgds/linux-rocknix.config"'
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit) as raised:
                own_os_build.kernel_config_drift(self.output(tmp, base_line=stale))
        self.assertIn('another kernel config', str(raised.exception))


if __name__ == '__main__':
    unittest.main()
