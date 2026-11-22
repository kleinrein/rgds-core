"""runtime/files.py: copy/move/delete/rename/mkdir/extract/peek and the save and system-folder confirms."""
from pathlib import Path
import sys
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import files
from files import Refused, check_name, delete, extract, make_folder, peek, purge_trash, rename, transfer, trash_root
from fields import read_fields


class FilesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.card = Path(temporary.name)
        self.data = self.card / 'rgds'
        (self.data / 'userdata').mkdir(parents=True)
        self.roms = self.card / 'roms'
        (self.roms / 'gba').mkdir(parents=True)
        (self.roms / 'nds').mkdir(parents=True)

    def make(self, relative, content=b'rom'):
        path = self.card / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_copy_keeps_the_original_and_numbers_a_clash(self):
        game = self.make('roms/gba/Zelda.gba', b'z' * 3000)
        self.make('inbox/Zelda.gba', b'other')
        placed = transfer([game], self.card / 'inbox', self.data)
        self.assertEqual(placed, [self.card / 'inbox/Zelda (2).gba'])
        self.assertTrue(game.exists())
        self.assertEqual(placed[0].read_bytes(), b'z' * 3000)
        self.assertFalse(list((self.card / 'inbox').glob('*.part')))

    def test_move_of_a_folder_on_the_same_card_is_a_rename(self):
        self.make('inbox/Keen/KEEN1.EXE', b'k')
        self.make('inbox/Keen/data/L1', b'l')
        (self.roms / 'dos').mkdir()
        placed = transfer([self.card / 'inbox/Keen'], self.roms / 'dos', self.data, move=True)
        self.assertEqual(placed, [self.roms / 'dos/Keen'])
        self.assertFalse((self.card / 'inbox/Keen').exists())
        self.assertEqual((self.roms / 'dos/Keen/data/L1').read_bytes(), b'l')

    def test_a_folder_cannot_go_inside_itself(self):
        self.make('inbox/A/b.txt')
        with self.assertRaisesRegex(Refused, 'inside itself'):
            transfer([self.card / 'inbox/A'], self.card / 'inbox/A', self.data, move=True)

    def test_moving_or_deleting_a_save_asks_first(self):
        save = self.make('roms/gba/Zelda.sav', b'progress')
        with self.assertRaises(Refused) as caught:
            delete([save], self.data)
        self.assertTrue(caught.exception.confirm)
        self.assertIn('save file', str(caught.exception))
        self.assertTrue(save.exists())
        with self.assertRaises(Refused):
            transfer([self.roms / 'gba'], self.card, self.data, move=True)  # a folder with a save in it
        delete([save], self.data, confirmed=True)
        self.assertFalse(save.exists())

    def test_the_system_folder_asks_even_for_a_rename(self):
        setting = self.make('rgds/userdata/settings.ini', b'x=1')
        with self.assertRaisesRegex(Refused, 'system folder') as caught:
            rename(setting, 'other.ini', self.data)
        self.assertTrue(caught.exception.confirm)
        self.assertEqual(rename(setting, 'other.ini', self.data, confirmed=True).name, 'other.ini')

    def test_copying_a_save_needs_no_confirm(self):
        save = self.make('roms/gba/Zelda.sav', b'progress')
        transfer([save], self.card / 'roms/nds', self.data)
        self.assertTrue((self.roms / 'nds/Zelda.sav').exists())

    def test_delete_goes_to_the_trash_with_where_it_came_from_and_old_trash_goes(self):
        game = self.make('roms/gba/Zelda.gba')
        delete([game], self.data)
        batches = list(trash_root(self.data).iterdir())
        self.assertEqual(len(batches), 1)
        self.assertTrue((batches[0] / 'Zelda.gba').exists())
        self.assertIn(str(game), (batches[0] / '.from.tsv').read_text())
        self.assertEqual(purge_trash(self.data, now=time.time() + 31 * 86400), 1)
        self.assertEqual(list(trash_root(self.data).iterdir()), [])

    def test_deleting_inside_the_trash_is_for_good(self):
        game = self.make('roms/gba/Zelda.gba')
        delete([game], self.data)
        inside = next(trash_root(self.data).iterdir()) / 'Zelda.gba'
        delete([inside], self.data)
        self.assertFalse(inside.exists())

    def test_names_the_card_cannot_store_are_refused_with_the_reason(self):
        for name, reason in (('a:b', ':'), ('what?', '?'), ('  ', 'Type a name'), ('x.', 'end with a dot')):
            with self.assertRaisesRegex(Refused, reason.replace('?', r'\?')):
                check_name(name)
        self.assertEqual(check_name(' Zelda (EU).gba '), 'Zelda (EU).gba')

    def test_rename_changes_only_the_case(self):
        game = self.make('roms/gba/zelda.gba')
        self.assertEqual(rename(game, 'Zelda.gba', self.data).name, 'Zelda.gba')
        self.assertEqual([p.name for p in (self.roms / 'gba').iterdir()], ['Zelda.gba'])

    def test_rename_onto_another_file_is_refused(self):
        game = self.make('roms/gba/a.gba')
        self.make('roms/gba/b.gba')
        with self.assertRaisesRegex(Refused, 'already something called b.gba'):
            rename(game, 'b.gba', self.data)

    def test_new_folder(self):
        self.assertTrue(make_folder(self.roms, 'Hacks').is_dir())
        with self.assertRaises(Refused):
            make_folder(self.roms, 'Hacks')

    def test_extract_into_a_folder_named_after_the_zip_and_refuse_zip_slip(self):
        archive = self.card / 'inbox/Disc.zip'
        archive.parent.mkdir()
        with zipfile.ZipFile(archive, 'w') as packed:
            packed.writestr('Disc.cue', 'FILE "Disc.bin" BINARY')
            packed.writestr('Disc.bin', b'd' * 100)
            packed.writestr('__MACOSX/._Disc.bin', b'junk')
        target = extract(archive)
        self.assertEqual(sorted(p.name for p in target.iterdir()), ['Disc.bin', 'Disc.cue'])
        evil = self.card / 'inbox/Evil.zip'
        with zipfile.ZipFile(evil, 'w') as packed:
            packed.writestr('../../escaped.txt', 'x')
        with self.assertRaisesRegex(Refused, 'outside its folder'):
            extract(evil)
        self.assertFalse((self.card / 'escaped.txt').exists())
        self.assertFalse(list((self.card / 'inbox').glob('.*part')))

    def test_peek_lists_a_zip_heads_a_text_and_shrinks_a_picture(self):
        archive = self.card / 'inbox/Games.zip'
        archive.parent.mkdir()
        with zipfile.ZipFile(archive, 'w') as packed:
            packed.writestr('a.gba', b'a' * 2048)
            packed.writestr('b.gba', b'b')
        out = self.card / 'session/files-peek'
        peek(archive, out)
        lines = out.with_suffix('.txt').read_text().splitlines()
        self.assertEqual(lines[0], str(archive))
        self.assertEqual(lines[1], '2 files, 2.0 KB unpacked')
        self.assertIn('a.gba  2.0 KB', lines[2])
        readme = self.make('inbox/readme.txt', b'Hello\nWorld\n')
        peek(readme, out)
        self.assertEqual(out.with_suffix('.txt').read_text().splitlines()[1:], ['Hello', 'World'])
        self.assertFalse(out.with_suffix('.bmp').exists())
        from screenshots import Picture, encode_png
        picture = self.card / 'inbox/shot.png'
        picture.write_bytes(encode_png(Picture(640, 480, bytes(640 * 480 * 3))))
        peek(picture, out)
        self.assertTrue(out.with_suffix('.bmp').exists())
        self.assertEqual(out.with_suffix('.txt').read_text().splitlines()[1], '640 × 480')
        binary = self.make('roms/gba/Zelda.sav', b'hi')
        peek(binary, out)
        self.assertEqual(out.with_suffix('.txt').read_text().splitlines(), [str(binary)])

    def test_cli_reports_a_confirm_then_goes_ahead(self):
        save = self.make('roms/gba/Zelda.sav')
        status = self.card / 'session/files.ini'
        self.assertEqual(files.main(['--data', str(self.data), '--status', str(status), 'delete', str(save)]), 3)
        fields = read_fields(status)
        self.assertEqual((fields['state'], fields['confirm']), ('confirm', '1'))
        self.assertEqual(
            files.main(['--data', str(self.data), '--status',
                        str(status), '--confirmed', 'delete',
                        str(save)]), 0
        )
        self.assertEqual(read_fields(status)['state'], 'done')
        self.assertEqual(
            files.main(['--data', str(self.data), '--status',
                        str(status), 'mkdir',
                        str(self.roms), 'a:b']), 1
        )
        self.assertIn('cannot have :', read_fields(status)['message'])


if __name__ == '__main__':
    unittest.main()
