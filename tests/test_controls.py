"""Persisted control profiles, physical shortcuts and save-before-home routing."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime'))
import controls
from game_menu import GameMenu
from library import write_fields


class ControlProfileTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.data=Path(temp.name);self.rom=self.data/'game.nds'

    def test_global_preset_per_game_override_and_inheritance(self):
        controls.save_profile(self.data,{'preset':'ab-swap','shortcut':'startthenmenu','shortcut_action':'menu'})
        global_profile=controls.load_profile(self.data)
        self.assertEqual(global_profile['bind_a'],'b')
        controls.set_binding(self.data,'a','r3',self.rom)
        self.assertEqual(controls.load_profile(self.data,self.rom)['bind_a'],'r3')
        self.assertEqual(controls.load_profile(self.data)['bind_a'],'b')
        controls.save_profile(self.data,{'inherit':'1'},self.rom)
        self.assertEqual(controls.load_profile(self.data,self.rom),global_profile)
        self.assertFalse((self.data/'emulators').exists())

    def test_capture_swaps_duplicate_assignment_and_invalid_write_preserves_file(self):
        controls.set_binding(self.data,'a','b')
        profile=controls.load_profile(self.data)
        self.assertEqual((profile['bind_a'],profile['bind_b']),('b','a'))
        path=self.data/'controls.ini';before=path.read_bytes()
        for fields in ({'bind_a':'menu'},{'bind_a':'a'},{'shortcut':'surprise'},{'preset':'invalid'}):
            with self.assertRaises(ValueError):controls.save_profile(self.data,fields)
            self.assertEqual(path.read_bytes(),before)

    def test_left_handed_and_auxiliary_capture_match_physical_calibration(self):
        profile=controls.preset('left-handed')
        self.assertEqual([profile['bind_'+name] for name in ('a','b','x','y','up','down','left','right')],['right','down','up','left','x','b','y','a'])
        self.assertEqual([controls.captured_source(button=b) for b in (0,1,2,3,9,10,11,12)],['a','b','y','x','l3','l2','r2','r3'])
        self.assertIsNone(controls.captured_source(button=8))
        self.assertEqual(controls.evdev_source(1,311),'start')
        self.assertEqual(controls.evdev_source(1,313),'l3')
        self.assertEqual(controls.captured_source(hat=8),'left')

    def test_fast_forward_binding_swaps_conflicts_and_inherits_per_game(self):
        controls.set_fast_forward(self.data,'a')
        profile=controls.load_profile(self.data)
        self.assertEqual((profile['fast_forward_button'],profile['bind_a']),('a','none'))
        controls.set_fast_forward(self.data,'b')
        profile=controls.load_profile(self.data)
        self.assertEqual((profile['fast_forward_button'],profile['bind_b']),('b','a'))
        controls.set_binding(self.data,'x','b')
        self.assertEqual(controls.load_profile(self.data)['fast_forward_button'],'x')
        controls.set_fast_forward(self.data,'l2',self.rom)
        self.assertEqual(controls.load_profile(self.data,self.rom)['fast_forward_button'],'l2')
        controls.save_profile(self.data,{'inherit':'1'},self.rom)
        self.assertEqual(controls.load_profile(self.data,self.rom)['fast_forward_button'],'x')

    def test_hold_release_pause_and_toggle_are_not_sticky_or_repeated(self):
        speed=controls.FastForwardButton()
        speed.configure(dict(fast_forward_button='l2',fast_forward_mode='hold'))
        self.assertEqual(speed.feed('l2',True),'fast-forward-on')
        self.assertIsNone(speed.feed('l2',True))
        self.assertEqual(speed.pause(),'fast-forward-off')
        self.assertIsNone(speed.feed('l2',True))
        self.assertIsNone(speed.feed('l2',False))
        self.assertEqual(speed.feed('l2',True),'fast-forward-on')
        self.assertEqual(speed.feed('l2',False),'fast-forward-off')
        speed.configure(dict(fast_forward_button='l2',fast_forward_mode='toggle'))
        self.assertEqual(speed.feed('l2',True),'fast-forward-toggle')
        self.assertIsNone(speed.feed('l2',True));self.assertIsNone(speed.feed('l2',False))
        self.assertEqual(speed.feed('l2',True),'fast-forward-toggle')

    def test_shortcut_chord_works_in_both_orders_and_requires_release(self):
        for first,second in (('start','menu'),('menu','start')):
            shortcut=controls.Shortcut()
            self.assertIsNone(shortcut.feed(first,True,0))
            self.assertEqual(shortcut.feed(second,True,.03),'home')
            self.assertIsNone(shortcut.poll(.3))
            self.assertIsNone(shortcut.feed(second,True,.4))
            shortcut.feed(first,False,.5);shortcut.feed(second,False,.51)
            self.assertIsNone(shortcut.feed(first,True,1))
            self.assertEqual(shortcut.feed(second,True,1.01),'home')

    def test_plain_menu_delay_and_sequence_option(self):
        shortcut=controls.Shortcut();shortcut.feed('menu',True,0)
        self.assertIsNone(shortcut.poll(.149));self.assertEqual(shortcut.poll(.151),'menu')
        self.assertIsNone(shortcut.poll(.3))
        sequence=controls.Shortcut('startthenmenu','home')
        sequence.feed('start',True,0);sequence.feed('start',False,.05)
        self.assertEqual(sequence.feed('menu',True,.5),'home')

    def test_home_shortcut_waits_for_checkpoint_and_keeps_failed_game_alive(self):
        class Process:
            result=None
            def poll(self):return self.result
            def send_signal(self,_value):pass
            def terminate(self):self.result=0
            def kill(self):self.result=-9
            def wait(self,timeout=None):return self.result
        class Inputs:
            value='home'
            def poll(self):value,self.value=self.value,False;return value
            def close(self):pass
        child,ui=Process(),Process();inputs=Inputs()
        menu=GameMenu(child,self.data/'app',self.data/'data','drastic',self.rom,{'RGDS_MENU_LIVE':'1'},inputs)
        self.addCleanup(menu.close)
        write_fields(self.data/'data/preferences.ini',dict(auto_save=0))
        with patch('game_menu.subprocess.Popen',return_value=ui):menu.poll()
        self.assertEqual(menu.pending['action'],'save-quit')
        self.assertFalse(menu.restart)
        self.assertIsNone(child.poll())
        token=menu.pending['token']
        write_fields(Path(str(menu.control)+'.result'),dict(token=token,state='save-timeout'))
        menu.poll()
        self.assertIsNone(child.poll());self.assertIsNone(menu.pending)
        self.assertIn('state=error',menu.status.read_text())


if __name__=='__main__':unittest.main()
