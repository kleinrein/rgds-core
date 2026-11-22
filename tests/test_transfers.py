"""Sources (local, SMB through a fake smbclient) and the transfer queue: resume, retry, placement, cancel."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import net_sources
from net_sources import LocalSource, SourceError, add_source, open_source, sources, remove_source
import transfers
from transfers import Queue, Runner, add, cancel, clear, detect, place, retry


class Fixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data = self.root / 'rgds'
        self.roms = self.root / 'roms'
        self.inbox = self.root / 'inbox'
        self.status = self.root / 'run/transfers'
        self.server = self.root / 'server'
        self.bundle = self.root / 'bundle'
        (self.bundle / 'bin').mkdir(parents=True)
        fake = self.bundle / 'bin/smbclient'
        fake.write_text(
            f'#!{sys.executable}\nimport runpy, sys\nsys.argv[0] = "smbclient"\n'
            f'runpy.run_path({str(ROOT / "tests/fixtures/fake_smbclient.py")!r}, run_name="__main__")\n'
        )
        fake.chmod(0o755)
        self.argv_log = self.root / 'argv.log'
        env = patch.dict(
            os.environ,
            FAKE_SMB_ROOT=str(self.server),
            FAKE_SMB_PASSWORD='hunter2',
            FAKE_SMB_ARGV=str(self.argv_log),
            FAKE_SMB_DROP='',
            FAKE_SMB_DROP_MARK=str(self.root / 'dropped')
        )
        env.start()
        self.addCleanup(env.stop)

    def serve(self, relative, content):
        path = self.server / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def smb(self, password='hunter2', share='Games'):
        return add_source(self.data, 'smb', password=password, name='nas', host='nas.local', share=share, user='owner')

    def runner(self):
        return Runner(self.data, self.roms, self.inbox, self.status, bundle=self.bundle, sleep=lambda s: None)

    def job(self, ident):
        return Queue(self.data).job(ident)


class SourceTests(Fixture):
    def test_local_lists_folders_first_and_resumes_a_partial_copy(self):
        folder = self.root / 'local'
        (folder / 'Sub').mkdir(parents=True)
        (folder / 'b.gba').write_bytes(b'x' * 3000)
        (folder / 'A.gb').write_bytes(b'y' * 10)
        source = LocalSource('l', folder)
        self.assertEqual([e.name for e in source.list()], ['Sub', 'A.gb', 'b.gba'])
        part = self.root / 'b.part'
        part.write_bytes(b'x' * 1000)
        source.fetch('b.gba', part)
        self.assertEqual(part.read_bytes(), b'x' * 3000)
        with self.assertRaises(SourceError):
            source.list('../..')

    def test_smb_lists_parses_names_with_spaces_and_hides_hidden_files(self):
        self.serve('Games/GBA/Golden Sun (Europe).gba', b'g' * 4096)
        self.serve('Games/GBA/.DS_Store', b'junk')
        (self.server / 'Games/GBA/Saves').mkdir()
        source = open_source(self.data, self.smb(), bundle=self.bundle, work=self.root / 'work')
        entries = source.list('GBA')
        self.assertEqual(
            [(e.name, e.is_dir, e.size) for e in entries],
            [('Saves', True, 0), ('Golden Sun (Europe).gba', False, 4096)]
        )
        self.assertGreater(entries[1].mtime, 0)
        self.assertEqual(source.stat('GBA/Golden Sun (Europe).gba').size, 4096)

    def test_smb_lists_shares_without_admin_ones(self):
        (self.server / 'Games').mkdir(parents=True)
        (self.server / 'Music').mkdir()
        source = open_source(self.data, self.smb(), bundle=self.bundle, work=self.root / 'work')
        self.assertEqual(source.shares(), ['Games', 'Music'])

    def test_wrong_password_and_missing_share_read_as_owner_messages(self):
        (self.server / 'Games').mkdir(parents=True)
        source = open_source(self.data, self.smb(password='wrong'), bundle=self.bundle, work=self.root / 'work')
        with self.assertRaisesRegex(SourceError, 'Wrong user name or password for nas') as caught:
            source.list()
        self.assertFalse(caught.exception.transient)
        other = open_source(self.data, self.smb(share='Nope'), bundle=self.bundle, work=self.root / 'work')
        with self.assertRaisesRegex(SourceError, 'no share called "Nope"'):
            other.list()

    def test_password_stays_out_of_argv_the_sources_file_and_the_work_folder(self):
        (self.server / 'Games').mkdir(parents=True)
        ident = self.smb()
        open_source(self.data, ident, bundle=self.bundle, work=self.root / 'work').list()
        self.assertNotIn('hunter2', self.argv_log.read_text())
        public, private = net_sources.config_paths(self.data)
        self.assertNotIn('hunter2', public.read_text())
        self.assertIn('hunter2', private.read_text())
        self.assertEqual(list((self.root / 'work').iterdir()), [])
        self.assertNotIn('password', sources(self.data)[ident])
        remove_source(self.data, ident)
        self.assertEqual(sources(self.data), {})
        self.assertNotIn('hunter2', private.read_text())

    def test_names_smbclient_cannot_quote_are_refused_before_running_it(self):
        source = open_source(self.data, self.smb(), bundle=self.bundle, work=self.root / 'work')
        with self.assertRaisesRegex(SourceError, 'Rename it on the server'):
            source.fetch('a";rm.gba', self.root / 'x.part')


class PlacementTests(Fixture):
    def staged(self, relative, content=b'rom'):
        path = self.root / 'stage' / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_systems_by_extension_folder_and_zip_contents(self):
        self.assertEqual(detect(self.staged('_/Mario Kart DS.nds')), 'nds')
        self.assertEqual(detect(self.staged('_/Zelda.gba')), 'gba')
        self.assertEqual(detect(self.staged('Genesis/Sonic.bin')), 'md')
        self.assertIsNone(detect(self.staged('_/Sonic.bin')))
        self.assertIsNone(detect(self.staged('_/readme.txt')))
        archive = self.root / 'stage/_/Pokemon.zip'
        with zipfile.ZipFile(archive, 'w') as packed:
            packed.writestr('Pokemon Black.nds', b'ds')
        self.assertEqual(detect(archive), 'nds')
        with zipfile.ZipFile(self.root / 'stage/_/Notes.zip', 'w') as packed:
            packed.writestr('notes.txt', b'hi')
        self.assertIsNone(detect(self.root / 'stage/_/Notes.zip'))

    def test_game_gear_goes_on_the_master_system_shelf_and_unknowns_to_the_inbox(self):
        final, system = place(self.staged('_/Shinobi.gg'), 'library', self.roms, self.inbox)
        self.assertEqual((final, system), (self.roms / 'sms/Shinobi.gg', 'gg'))
        final, system = place(self.staged('_/manual.pdf'), 'library', self.roms, self.inbox)
        self.assertEqual((final, system), (self.inbox / 'manual.pdf', ''))

    def test_same_file_is_not_copied_twice_and_a_different_one_gets_a_number(self):
        (self.roms / 'gba').mkdir(parents=True)
        (self.roms / 'gba/Zelda.gba').write_bytes(b'rom')
        final, _ = place(self.staged('_/Zelda.gba'), 'library', self.roms, self.inbox)
        self.assertEqual(final, self.roms / 'gba/Zelda.gba')
        self.assertEqual(sorted(p.name for p in (self.roms / 'gba').iterdir()), ['Zelda.gba'])
        final, _ = place(self.staged('_/Zelda.gba', b'other dump'), 'library', self.roms, self.inbox)
        self.assertEqual(final, self.roms / 'gba/Zelda (2).gba')

    def test_a_disc_folder_is_placed_whole_by_its_sheet(self):
        folder = self.root / 'stage/Final Fantasy VII (Disc 1)'
        self.staged('Final Fantasy VII (Disc 1)/Track 01.bin', b'data')
        self.staged('Final Fantasy VII (Disc 1)/FF7.cue', b'FILE "Track 01.bin" BINARY')
        final, system = place(folder, 'library', self.roms, self.inbox)
        self.assertEqual((final, system), (self.roms / 'psx/Final Fantasy VII (Disc 1)', 'psx'))
        self.assertTrue((final / 'Track 01.bin').is_file())


class QueueTests(Fixture):
    def test_a_game_from_a_genesis_folder_lands_in_the_mega_drive_folder(self):
        self.serve('Games/Genesis/Sonic.bin', b's' * 5000)
        job = add(self.data, self.smb(), 'Genesis/Sonic.bin', bundle=self.bundle)
        self.assertEqual((job['state'], job['size'], job['parent']), ('queued', 5000, 'Genesis'))
        self.runner().run()
        done = self.job(job['id'])
        self.assertEqual((done['state'], done['system']), ('done', 'md'))
        self.assertEqual((self.roms / 'md/Sonic.bin').read_bytes(), b's' * 5000)
        self.assertFalse(Queue(self.data).staging(done).exists())
        status = dict(line.split('=', 1) for line in self.status.read_text().splitlines())
        self.assertEqual((status['state'], status['queued'], status['serial']), ('idle', '0', '1'))

    def test_a_dropped_connection_resumes_from_the_partial_file(self):
        content = os.urandom(200_000)
        self.serve('Games/Zelda.gba', content)
        os.environ['FAKE_SMB_DROP'] = '70000'
        job = add(self.data, self.smb(), 'Zelda.gba', bundle=self.bundle)
        self.runner().run()
        self.assertEqual(self.job(job['id'])['state'], 'done')
        self.assertEqual((self.roms / 'gba/Zelda.gba').read_bytes(), content)
        calls = self.argv_log.read_text()
        self.assertIn('-c get "Zelda.gba"', calls)
        self.assertIn('-c reget "Zelda.gba"', calls)
        self.assertEqual(self.job(job['id'])['attempts'], 0)

    def test_a_wrong_password_fails_at_once_and_can_be_retried(self):
        self.serve('Games/Zelda.gba', b'z')
        ident = self.smb()
        job = add(self.data, ident, 'Zelda.gba', bundle=self.bundle)
        os.environ['FAKE_SMB_PASSWORD'] = 'changed'
        self.runner().run()
        failed = self.job(job['id'])
        self.assertEqual((failed['state'], failed['attempts']), ('failed', 1))
        self.assertIn('Wrong user name or password', failed['error'])
        os.environ['FAKE_SMB_PASSWORD'] = 'hunter2'
        retry(self.data, job['id'])
        self.runner().run()
        self.assertEqual(self.job(job['id'])['state'], 'done')

    def test_adding_checks_the_file_now_and_the_same_file_twice_is_one_job(self):
        self.serve('Games/Zelda.gba', b'z')
        ident = self.smb()
        with self.assertRaisesRegex(SourceError, 'not on nas'):
            add(self.data, ident, 'Missing.gba', bundle=self.bundle)
        first = add(self.data, ident, 'Zelda.gba', bundle=self.bundle)
        second = add(self.data, ident, '/Zelda.gba', bundle=self.bundle)
        self.assertEqual(first['id'], second['id'])

    def test_a_folder_job_copies_every_file_and_places_the_folder(self):
        self.serve('Games/DOS/Commander Keen/KEEN1.EXE', b'k' * 300)
        self.serve('Games/DOS/Commander Keen/data/LEVEL01.CK1', b'l' * 200)
        job = add(self.data, self.smb(), 'DOS/Commander Keen', bundle=self.bundle)
        self.assertEqual((job['kind'], job['size'], len(job['files'])), ('folder', 500, 2))
        self.runner().run()
        done = self.job(job['id'])
        self.assertEqual(done['state'], 'done', done['error'])
        # Nothing in it names a system: the owner says which, from the inbox.
        self.assertEqual(Path(done['placed']), self.inbox / 'Commander Keen')
        self.assertEqual((self.inbox / 'Commander Keen/data/LEVEL01.CK1').read_bytes(), b'l' * 200)

    def test_a_job_to_a_named_folder_skips_detection(self):
        self.serve('Games/Zelda.gba', b'z')
        target = self.root / 'Downloads'
        job = add(self.data, self.smb(), 'Zelda.gba', to=target, bundle=self.bundle)
        self.runner().run()
        self.assertEqual(Path(self.job(job['id'])['placed']), target / 'Zelda.gba')

    def test_cancel_drops_the_partial_copy_and_clear_forgets_finished_jobs(self):
        self.serve('Games/Zelda.gba', b'z' * 1000)
        job = add(self.data, self.smb(), 'Zelda.gba', bundle=self.bundle)
        staging = Queue(self.data).staging(job) / '_'
        staging.mkdir(parents=True)
        (staging / 'Zelda.gba.part').write_bytes(b'z' * 10)
        self.assertEqual(self.runner().received(self.job(job['id'])), 10)
        cancel(self.data, job['id'])
        self.assertFalse(Queue(self.data).staging(job).exists())
        self.runner().run()
        self.assertFalse((self.roms / 'gba/Zelda.gba').exists())
        clear(self.data)
        self.assertEqual(Queue(self.data).read()['jobs'], [])

    def test_a_full_card_fails_with_the_numbers(self):
        self.serve('Games/Zelda.gba', b'z' * 1000)
        job = add(self.data, self.smb(), 'Zelda.gba', bundle=self.bundle)
        usage = type('Usage', (), {'free': 10 << 20})
        with patch('transfers.shutil.disk_usage', return_value=usage):
            self.runner().run()
        failed = self.job(job['id'])
        self.assertEqual(failed['state'], 'failed')
        self.assertIn('Not enough space on the card', failed['error'])

    def test_a_job_left_running_by_a_power_cut_resumes(self):
        content = b'r' * 4000
        self.serve('Games/Zelda.gba', content)
        job = add(self.data, self.smb(), 'Zelda.gba', bundle=self.bundle)
        Queue(self.data).update(job['id'], state='running')
        staging = Queue(self.data).staging(job) / '_'
        staging.mkdir(parents=True)
        (staging / 'Zelda.gba.part').write_bytes(content[:1500])
        self.runner().run()
        self.assertEqual((self.roms / 'gba/Zelda.gba').read_bytes(), content)
        self.assertIn('reget', self.argv_log.read_text())

    def test_cli_add_list_and_run(self):
        self.serve('Games/Zelda.gba', b'z')
        ident = self.smb()
        common = [
            '--data',
            str(self.data), '--roms',
            str(self.roms), '--inbox',
            str(self.inbox), '--status',
            str(self.status), '--bundle',
            str(self.bundle)
        ]
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(transfers.main(common + ['add', ident, 'Zelda.gba']), 0)
        self.assertEqual(json.loads(out.getvalue())['name'], 'Zelda.gba')
        self.assertEqual(transfers.main(common + ['run']), 0)
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            transfers.main(common + ['list'])
        self.assertEqual(json.loads(out.getvalue())[0]['state'], 'done')
        with patch('sys.stderr', new_callable=io.StringIO) as err:
            self.assertEqual(transfers.main(common + ['add', ident, 'Nope.gba']), 1)
        self.assertIn('not on nas', err.getvalue())

    def test_cli_adds_several_runs_them_and_answers_the_launcher(self):
        self.serve('Games/Zelda.gba', b'z')
        self.serve('Games/Metroid.gba', b'm')
        ident = self.smb()
        answer = self.root / 'answer'
        common = [
            '--data',
            str(self.data), '--roms',
            str(self.roms), '--inbox',
            str(self.inbox), '--status',
            str(self.status), '--bundle',
            str(self.bundle)
        ]
        with patch('sys.stdout', new_callable=io.StringIO):
            code = transfers.main(common + ['add', ident, 'Zelda.gba', 'Metroid.gba', '--run', '--answer', str(answer)])
        self.assertEqual((code, answer.read_text()), (0, 'ok\n'))
        self.assertEqual(sorted(p.name for p in (self.roms / 'gba').iterdir()), ['Metroid.gba', 'Zelda.gba'])
        with patch('sys.stdout', new_callable=io.StringIO), patch('sys.stderr', new_callable=io.StringIO):
            transfers.main(common + ['add', ident, 'Gone.gba', '--answer', str(answer)])
        self.assertTrue(answer.read_text().startswith('error\tGone.gba is not on nas'))


if __name__ == '__main__':
    unittest.main()
