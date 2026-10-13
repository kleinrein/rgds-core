"""Exercise real host UI capture, swapping, scopes, reset and cancellation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.is_file(),'Build the host launcher first')
class ControlsSettingsUITests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.data=self.root/'data';self.data.mkdir()
        self.prefs=self.data/'preferences.ini';self.prefs.write_text('schema=1\nmode=light\nfont=rounded\ncore=1\n')
        self.state=self.root/'state';self.report=self.root/'cache.json'

    def fields(self,path):
        return dict(line.split('=',1) for line in path.read_text().splitlines() if '=' in line)

    def run_ui(self,script,game=False,restore=False,seconds=1):
        path=self.root/'input';path.write_text(script)
        command=[str(BINARY),'--preview','--preferences',str(self.prefs),'--data',str(self.data),
                 '--state',str(self.state),'--input-script',str(path),'--seconds',str(seconds),'--cache-report',str(self.report)]
        if game:
            rom=self.root/'my-game.nds';rom.write_bytes(bytes(128))
            command+=['--game-menu','--game-core','dsperate','--game-rom',str(rom)]
        elif not restore:command+=['--settings','--section','controls']
        result=subprocess.run(command,cwd=ROOT,env=dict(os.environ,SDL_VIDEODRIVER='dummy'),capture_output=True,text=True,timeout=seconds+5)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return json.loads(self.report.read_text()),result.stdout

    def to_a(self):return ''.join(f'{40+index*40} key Down\n' for index in range(4))+'220 key A\n'

    def test_capture_physical_b_swaps_a_b_and_persists_without_changing_launcher_navigation(self):
        report,_=self.run_ui(self.to_a()+'300 joy-down 1\n320 joy-up 1\n400 key Escape\n')
        fields=self.fields(self.data/'controls.ini')
        self.assertEqual((fields['bind_a'],fields['bind_b']),('b','a'))
        self.assertEqual(fields['preset'],'custom')
        self.assertEqual((fields['shortcut'],fields['shortcut_action']),('start+menu','home'))
        self.assertEqual(report['page'],1)
        self.assertIn('1 6 0\n',self.state.read_text())

    def test_direction_capture_and_unassign_keep_each_assigned_source_unique(self):
        self.run_ui(self.to_a()+'300 joy-hat 2\n320 joy-hat 0\n')
        fields=self.fields(self.data/'controls.ini')
        self.assertEqual((fields['bind_a'],fields['bind_right']),('right','a'))
        self.run_ui(self.to_a()+'300 key Backspace\n')
        fields=self.fields(self.data/'controls.ini')
        self.assertEqual(fields['bind_a'],'none')
        assigned=[value for key,value in fields.items() if key.startswith('bind_') and value!='none']
        self.assertEqual(len(assigned),len(set(assigned)))

    def test_cancel_capture_does_not_write_or_change_binding(self):
        report,_=self.run_ui(self.to_a()+'300 key Escape\n')
        self.assertFalse((self.data/'controls.ini').exists())
        self.assertEqual(report['page'],25)
        self.assertIn('25 6 4\n',self.state.read_text())
        self.assertEqual(report['render_file_reads'],0)
        self.assertEqual(report['render_stamp_checks'],0)

    def test_fast_forward_button_capture_and_hold_toggle_persist(self):
        self.state.write_text('1 0 1 ""\n25 6 17\ncontrols 0 1\n')
        self.run_ui('50 key A\n150 joy-down 10\n170 joy-up 10\n',restore=True)
        fields=self.fields(self.data/'controls.ini')
        self.assertEqual((fields['fast_forward_button'],fields['fast_forward_mode']),('l2','hold'))
        self.run_ui('50 key Down\n100 key A\n',restore=True)
        self.assertEqual(self.fields(self.data/'controls.ini')['fast_forward_mode'],'toggle')

    def test_lost_focus_cancels_capture_without_assigning_a_later_button(self):
        report,_=self.run_ui(self.to_a()+'300 focus-lost\n340 joy-down 1\n360 joy-up 1\n')
        self.assertFalse((self.data/'controls.ini').exists())
        self.assertFalse(report['controls_capturing'])
        self.assertEqual(report['page'],1)

    def test_failed_game_profile_write_keeps_inherited_layout(self):
        self.run_ui('50 click 510 364\n',game=True)
        import sys
        sys.path.insert(0,str(ROOT/'runtime'))
        from library import game_key
        path=self.data/'library'/game_key(self.root/'my-game.nds')/'controls.ini'
        path.mkdir(parents=True)
        self.state.unlink()
        report,_=self.run_ui('30 click 510 364\n'+self.to_a()+'300 joy-down 1\n320 joy-up 1\n',game=True)
        self.assertTrue(path.is_dir())
        self.assertTrue(report['controls_inherited'])
        self.assertFalse(report['controls_capturing'])
        self.assertEqual(report['controls_preset'],'default')

    def test_game_profile_changes_and_reset_return_to_global_mapping(self):
        (self.data/'controls.ini').write_text('schema=1\npreset=ab-swap\nshortcut=start+menu\nshortcut_action=home\n')
        self.run_ui('50 click 510 364\n100 key Down\n150 key A\n180 key Right\n220 key A\n',game=True)
        local=next((self.data/'library').glob('*/controls.ini'))
        self.assertEqual(self.fields(local)['inherit'],'0')
        self.assertEqual(self.fields(self.data/'controls.ini')['preset'],'ab-swap')
        self.state.unlink()
        select_reset='50 click 510 364\n'+''.join(f'{100+index*25} key Down\n' for index in range(16))+'550 key A\n'
        self.run_ui(select_reset,game=True)
        self.assertEqual(self.fields(local)['inherit'],'1')
        self.assertEqual(self.fields(self.data/'controls.ini')['preset'],'ab-swap')

    def test_back_returns_to_the_action_grid_and_default_reset_is_reversible(self):
        report,_=self.run_ui('40 click 510 364\n100 key Escape\n',game=True)
        self.assertEqual(report['page'],12)
        self.assertIn('12 6 9\n',self.state.read_text())
        self.state.unlink()
        self.run_ui('40 key Down\n80 key A\n120 key Right\n160 key A\n')
        self.assertEqual(self.fields(self.data/'controls.ini')['preset'],'ab-swap')
        self.run_ui(''.join(f'{40+index*25} key Down\n' for index in range(16))+'500 key A\n')
        fields=self.fields(self.data/'controls.ini')
        self.assertEqual(fields['preset'],'default')
        self.assertEqual(fields['bind_a'],'a')


if __name__=='__main__':unittest.main()
