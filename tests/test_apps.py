"""Apps (docs/APPS.md): app.ini checks, zip installs, covers and the launch plan, and the Ports shelf listing them."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
sys.path.insert(0, str(ROOT / 'runtime'))
import apps
import portmaster


def make_app(folder, **fields):
    folder.mkdir(parents=True, exist_ok=True)
    values = dict(schema='1', name='Hello', exec='hello.sh')
    values.update(fields)
    (folder / 'app.ini').write_text(''.join('%s=%s\n' % item for item in values.items() if item[1] is not None))
    (folder / 'hello.sh').write_text('#!/bin/bash\necho hello\n')
    return folder


class AppsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.apps = self.root / 'apps'
        self.data = self.root / 'userdata'

    def tearDown(self):
        self.temp.cleanup()

    def test_check_accepts_a_minimal_app_and_names_each_problem(self):
        self.assertEqual(apps.check(make_app(self.apps / 'hello')), [])
        problems = apps.check(make_app(self.apps / 'bad', exec='../escape.sh', arch='x86', colour='red', gl='vulkan'))
        self.assertTrue(any('exec=../escape.sh' in p for p in problems))
        self.assertTrue(any('arch=' in p for p in problems))
        self.assertTrue(any('colour=' in p for p in problems))
        self.assertTrue(any('gl=' in p for p in problems))
        self.assertIn('exec= is missing', ' '.join(apps.check(make_app(self.apps / 'none', exec=None))))
        self.assertIn('There is no app.ini', apps.check(self.apps / 'missing')[0])

    def test_only_blockers_stop_a_launch_and_a_bom_or_spaces_are_read_like_the_launcher_does(self):
        folder = make_app(self.apps / 'My App', screenshot='shot.png', icon='missing.png')
        self.assertTrue(any('screenshot=' in p for p in apps.check(folder)))
        self.assertEqual(apps.blockers(folder), [])
        (folder / 'app.ini').write_text('\ufeffschema = 1\nname = Hello World \nexec = hello.sh\n', encoding='utf-8')
        self.assertEqual(apps.check(folder), [p for p in apps.check(folder) if 'folder name' in p])
        self.assertEqual(apps.manifest(folder)['name'], 'Hello World')
        with mock.patch('linux_platform.rgds_os', return_value=True), \
                mock.patch.object(portmaster, 'panel_size', return_value=(640, 480)):
            plan = apps.launch_plan(ROOT, self.data, folder / 'app.ini', {})
        self.assertEqual(plan['port'], 'Hello World')
        self.assertEqual(plan['env']['RGDS_APP_DATA'], str(self.data / 'apps/My App'))

    def test_a_failed_swap_keeps_the_old_copy_and_a_cut_short_install_is_put_back(self):
        manifest = 'exec=hello.sh\n'
        apps.install(self.zip('a.zip', {'hello/app.ini': manifest, 'hello/hello.sh': 'echo 1\n'}), self.apps)
        real = Path.rename

        def failing(path, target):
            if path.name.startswith('.install-'): raise OSError('card removed')
            return real(path, target)

        with mock.patch.object(Path, 'rename', failing), self.assertRaises(OSError):
            apps.install(self.zip('b.zip', {'hello/app.ini': manifest, 'hello/hello.sh': 'echo 2\n'}), self.apps)
        self.assertEqual((self.apps / 'hello/hello.sh').read_text(), 'echo 1\n')
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()), ['hello'])
        # Power lost between the renames: only the hidden previous copy is there.
        (self.apps / 'hello').rename(self.apps / '.old-my-hello-4242')
        (self.apps / '.install-other-17').mkdir()
        self.assertEqual(apps.recover(self.apps), ['my-hello'])
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()), ['my-hello'])

    def test_only_an_icon_is_cropped_for_the_cover(self):
        cover = make_app(self.apps / 'cover')
        (cover / 'cover.png').write_bytes(b'png')
        icon = make_app(self.apps / 'icon')
        (icon / 'icon.png').write_bytes(b'png')
        with mock.patch.object(portmaster, 'icon_bmp', return_value=True) as cropped, \
                mock.patch('scraper.image_to_bmp') as whole:
            self.assertEqual(apps.artwork(self.data, self.apps), 2)
        self.assertEqual([call.args[0].name for call in cropped.call_args_list], ['icon.png'])
        self.assertEqual(whole.call_count, 1)

    def test_check_refuses_a_newer_core_and_a_program_that_cannot_run(self):
        app = self.root / 'core'
        app.mkdir()
        (app / 'VERSION').write_text('0.9.1\n')
        self.assertEqual(apps.check(make_app(self.apps / 'old', min_core='0.9.0'), app), [])
        self.assertIn('Needs RG DS Core 1.2', apps.check(make_app(self.apps / 'new', min_core='1.2'), app)[0])
        folder = make_app(self.apps / 'binary', exec='hello')
        (folder / 'hello').write_bytes(b'\x7fELF')
        self.assertIn('not executable', apps.check(folder)[0])
        (folder / 'hello').chmod(0o755)
        self.assertEqual(apps.check(folder), [])

    def zip(self, name, files):
        path = self.root / name
        with zipfile.ZipFile(path, 'w') as archive:
            for member, text in files.items():
                archive.writestr(member, text)
        return path

    def test_install_takes_a_folder_zip_or_a_flat_zip_and_replaces_an_older_copy(self):
        manifest = 'name=Hello\nexec=hello.sh\n'
        self.assertEqual(
            apps.install(self.zip('a.zip', {
                'hello/app.ini': manifest,
                'hello/hello.sh': 'echo 1\n'
            }), self.apps), 'hello'
        )
        self.assertTrue(os.access(self.apps / 'hello/hello.sh', os.X_OK))
        self.assertEqual(
            apps.install(self.zip('flat.zip', {
                'app.ini': manifest,
                'hello.sh': 'echo 2\n'
            }), self.apps), 'flat'
        )
        (self.apps / 'hello/leftover').write_text('old')
        apps.install(self.zip('b.zip', {'hello/app.ini': manifest, 'hello/hello.sh': 'echo 3\n'}), self.apps)
        self.assertEqual((self.apps / 'hello/hello.sh').read_text(), 'echo 3\n')
        self.assertFalse((self.apps / 'hello/leftover').exists())
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()), ['flat', 'hello'])

    def test_install_refuses_escaping_paths_and_zips_without_a_manifest_and_keeps_the_old_copy(self):
        apps.install(self.zip('a.zip', {'hello/app.ini': 'exec=hello.sh\n', 'hello/hello.sh': 'echo 1\n'}), self.apps)
        for files in (
            {
                '../evil/app.ini': 'exec=x.sh\n'
            }, {
                'hello/readme.txt': 'no manifest'
            }, {
                'hello/app.ini': 'exec=missing.sh\n'
            }
        ):
            with self.subTest(files=files), self.assertRaises(ValueError):
                apps.install(self.zip('hello.zip', files), self.apps)
        self.assertEqual((self.apps / 'hello/hello.sh').read_text(), 'echo 1\n')
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()), ['hello'])

    def test_pending_zips_install_once_and_a_failure_says_why(self):
        self.apps.mkdir()
        good = self.zip('apps/good.zip', {'good/app.ini': 'exec=run.sh\n', 'good/run.sh': 'true\n'})
        self.zip('apps/broken.zip', {'readme.txt': 'nothing'})
        self.assertEqual(apps.install_pending(self.apps), ['good'])
        self.assertFalse(good.exists())
        self.assertTrue((self.apps / '.installed/good.zip').is_file())
        self.assertIn('Not installed', (self.apps / 'broken.zip.txt').read_text())
        self.assertEqual([a['id'] for a in apps.installed(self.apps)], ['good'])

    def test_launch_plan_runs_the_script_in_its_folder_with_its_own_data_folder(self):
        folder = make_app(self.apps / 'hello', args='--fullscreen "two words"')
        (folder / 'lib').mkdir()
        with mock.patch('linux_platform.rgds_os', return_value=True), \
                mock.patch.object(portmaster, 'panel_size', return_value=(640, 480)):
            plan = apps.launch_plan(ROOT, self.data, folder / 'app.ini', {'WAYLAND_DISPLAY': 'w'})
        self.assertEqual(plan['command'], ['bash', str(folder / 'hello.sh'), '--fullscreen', 'two words'])
        self.assertEqual(plan['cwd'], str(folder))
        self.assertEqual(plan['port'], 'Hello')
        env = plan['env']
        self.assertEqual(env['HOME'], str(self.data / 'apps/hello'))
        self.assertTrue((self.data / 'apps/hello/config').is_dir())
        self.assertEqual(env['SDL_VIDEODRIVER'], 'kmsdrm')
        self.assertNotIn('WAYLAND_DISPLAY', env)
        self.assertEqual(env['RGDS_PANEL_WIDTH'], '640')
        self.assertTrue(env['LD_LIBRARY_PATH'].startswith(str(folder / 'lib')))
        self.assertEqual(env['SDL_GAMECONTROLLERCONFIG'], portmaster.RGDS_OS_PAD)

    def test_portmaster_launch_plan_hands_apps_over_without_portmaster_installed(self):
        folder = make_app(self.apps / 'hello')
        with mock.patch('linux_platform.rgds_os', return_value=True), \
                mock.patch.object(portmaster, 'panel_size', return_value=(640, 480)):
            plan = portmaster.launch_plan(
                ROOT, self.data, folder / 'app.ini', {}, folders=portmaster.Folders(self.root / 'pm')
            )
        self.assertEqual(plan['port'], 'Hello')
        with self.assertRaises(ValueError):
            apps.launch_plan(ROOT, self.data, make_app(self.apps / 'arm', arch='armhf') / 'app.ini', {})

    @unittest.skipUnless(BINARY.is_file(), 'build the host launcher first')
    def test_ports_shelf_lists_apps_by_their_name(self):
        make_app(self.apps / 'hello', name='Hello World')
        make_app(self.apps / '.hidden')
        (self.apps / 'loose').mkdir()
        (self.apps / 'install.sh').write_text('#!/bin/bash\n')
        spaced = make_app(self.apps / 'spaced')
        (spaced / 'app.ini').write_text('\ufeffname = Spaced Out \nexec=hello.sh\n', encoding='utf-8')
        (self.data / 'ui.ini').parent.mkdir(parents=True)
        (self.data / 'ui.ini').write_text('schema=1\nhome_layout=carousel\nbackground=plain\nshow_systems=1\n')
        (self.root / 'nds').mkdir()
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.data), '--rom-dir',
                str(self.root / 'nds'), '--system-dir', 'ports=' + str(self.apps), '--seconds', '2', '--cache-report',
                str(self.data / 'report.json')
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            json.loads((self.data / 'report.json').read_text())['port_titles'], ['Hello World', 'Spaced Out']
        )


if __name__ == '__main__':
    unittest.main()
