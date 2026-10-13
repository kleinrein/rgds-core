import json,sys,tempfile,unittest,zipfile
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'runtime'))
from save_library import SaveLibrary,digest
from quick_resume import arm,consume,record_exit,invalidate
from fields import write_fields

class SaveLibraryTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name);self.rom=self.root/'Game.nds';self.rom.write_bytes(b'test game');self.data=self.root/'data';self.v=SaveLibrary(self.data,self.rom,'dsperate','v1');self.v.canonical.parent.mkdir(parents=True,exist_ok=True);self.v.canonical.write_bytes(b'first state')
 def test_overwrite_confirmation_and_reversible_undo(self):
  self.v.store(0);self.v.canonical.write_bytes(b'new state')
  with self.assertRaisesRegex(ValueError,'Confirm'):self.v.store(0)
  self.v.store(0,True);self.v.undo_write();self.assertEqual(self.v.checked(self.v.catalog()['slots']['0']).read_bytes(),b'first state');self.v.undo_write();self.assertEqual(self.v.checked(self.v.catalog()['slots']['0']).read_bytes(),b'new state')
 def test_load_keeps_current_progress_for_undo(self):
  self.v.store(0);self.v.canonical.write_bytes(b'unsaved progress');self.v.stage_load(0);self.assertEqual(self.v.canonical.read_bytes(),b'first state');self.v.undo_load();self.assertEqual(self.v.canonical.read_bytes(),b'unsaved progress')
 def test_failed_copy_never_changes_catalog(self):
  self.v.store(0);before=self.v.catalog();self.v.canonical.write_bytes(b'new')
  with patch('save_library.copy_file',side_effect=OSError('full')),self.assertRaises(OSError):self.v.store(0,True)
  self.assertEqual(self.v.catalog(),before)
 def test_export_import_validates_game_engine_and_content(self):
  self.v.store(0);archive=self.v.export(0);self.v.import_slot(archive,1);self.assertEqual(self.v.catalog()['slots']['0']['object'],self.v.catalog()['slots']['1']['object'])
  other=SaveLibrary(self.data,self.rom,'melonds')
  with self.assertRaisesRegex(ValueError,'different'):other.import_slot(archive,0)
  self.rom.write_bytes(b'different revision')
  with self.assertRaisesRegex(ValueError,'different'):self.v.import_slot(archive,2)
 def test_corrupt_slot_cannot_replace_current_checkpoint(self):
  self.v.store(0);self.v.checked(self.v.catalog()['slots']['0']).write_bytes(b'corrupt');self.v.canonical.write_bytes(b'good')
  with self.assertRaisesRegex(ValueError,'verification'):self.v.stage_load(0)
  self.assertEqual(self.v.canonical.read_bytes(),b'good')
 def test_hostile_archive_cannot_escape_import(self):
  path=self.root/'bad.zip'
  with zipfile.ZipFile(path,'w') as z:z.writestr('../escape',b'bad')
  with self.assertRaises(ValueError):self.v.import_slot(path,0)
  self.assertFalse((self.root/'escape').exists())
 def test_resume_is_opt_in_verified_and_one_shot(self):
  self.assertFalse(arm(self.data));write_fields(self.data/'experience.ini',dict(quick_resume=1));record_exit(self.data,self.rom,'dsperate',0);self.assertTrue(arm(self.data));self.assertTrue(consume(self.data));self.assertFalse(consume(self.data));self.assertFalse((self.data/'session/resume-launch.ini').exists())
  self.assertTrue(arm(self.data));self.v.canonical.write_bytes(b'changed');self.assertFalse(consume(self.data))
 def test_new_launch_invalidates_previous_resume_candidate(self):
  write_fields(self.data/'experience.ini',dict(quick_resume=1));record_exit(self.data,self.rom,'dsperate',0);self.assertTrue(arm(self.data));invalidate(self.data);self.assertFalse(arm(self.data))
 def test_resume_rejects_replaced_rom(self):
  write_fields(self.data/'experience.ini',dict(quick_resume=1));record_exit(self.data,self.rom,'dsperate',0);self.assertTrue(arm(self.data));self.rom.write_bytes(b'new revision');self.assertFalse(consume(self.data))
 def test_disabled_resume_does_not_hash_rom_on_normal_exit(self):
  with patch('quick_resume.digest',side_effect=AssertionError('No exit-time ROM read when disabled')):record_exit(self.data,self.rom,'dsperate',0)
  self.assertFalse((self.data/'resume/candidate.json').exists())
 def test_unreadable_resume_state_falls_back_to_menu(self):
  write_fields(self.data/'experience.ini',dict(quick_resume=1));record_exit(self.data,self.rom,'dsperate',0);self.assertTrue(arm(self.data))
  with patch('quick_resume.digest',side_effect=OSError('Card read failed')):self.assertFalse(consume(self.data))
  self.assertFalse((self.data/'session/resume-launch.ini').exists())
 def test_old_checkpoint_does_not_arm_after_failed_save(self):
  record_exit(self.data,self.rom,'dsperate',self.v.canonical.stat().st_mtime+10);write_fields(self.data/'experience.ini',dict(quick_resume=1));self.assertFalse(arm(self.data))
if __name__=='__main__':unittest.main()
