"""Switching is gated on a verified checkpoint and the old engine's exit."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch
import test_game_menu as menus
from game_menu import SwitchGame, MenuInput, ANBERNIC_MENU_CODES, menu_button_action
from library import game_key, write_fields
import emulators


class GameSwitcherTests(unittest.TestCase):
    request = menus.GameMenuTests.request
    result = menus.GameMenuTests.result

    def setUp(self):
        menus.GameMenuTests.setUp(self)
        self.menu.rom.write_bytes(b'current game')
        self.next_game = (self.root / 'Next game.nds').resolve()
        self.next_game.write_bytes(b'next game')
        write_fields(self.menu.folder / 'switcher-library.ini', {
            'game_0_rom': str(self.menu.rom), 'game_0_title': 'Current',
            'game_1_rom': str(self.next_game), 'game_1_title': 'Next game',
            'game_2_rom': str(self.next_game), 'game_3_rom': str(self.root / 'gone.nds'),
        })
        self.menu.prepare_switcher()

    def test_switch_always_saves_and_waits_for_verified_exit(self):
        write_fields(self.menu.data / 'preferences.ini', {'auto_save': '0', 'core': '1'})
        write_fields(self.menu.data / 'library' / game_key(self.next_game) / 'game.ini', {'core': '3'})
        self.request('switch:1')
        self.assertEqual(self.menu.pending['action'], 'save-quit')
        self.assertEqual(self.menu.switch_target, SwitchGame('melonds', self.next_game))
        self.assertIsNone(self.menu.completed_switch())
        self.result('saved')
        self.assertIsNone(self.menu.completed_switch(), 'A save acknowledgement does not imply engine exit')
        self.child.result = 0
        self.menu.poll()
        self.assertEqual(self.menu.completed_switch(), SwitchGame('melonds', self.next_game))

    def test_logo_and_home_keys_open_different_pages(self):
        self.assertEqual(menu_button_action(312, True), 'menu')
        self.assertEqual(menu_button_action(354, True), 'menu')
        self.assertEqual(menu_button_action(316), 'menu')
        for code in (102, 139, 158, 172):
            self.assertEqual(menu_button_action(code, True), 'game-menu')
        for key, switcher in (('menu', True), ('game-menu', False)):
            self.menu.close_menu()
            self.ui = menus.Process()
            self.clock += 1
            with patch.object(self.menu.inputs, 'poll', return_value=key), patch('game_menu.subprocess.Popen', return_value=self.ui) as spawn:
                self.menu.poll()
            self.assertEqual('--game-switcher' in spawn.call_args.args[0], switcher)
            self.assertIsNone(self.menu.pending, 'Home opens a menu without exiting or saving')

    def test_stock_anbernic_packet_aliases_do_not_become_home(self):
        from audio import EVENT
        for codes in ((312,), (354,), (312, 354)):
            with patch('game_menu.Path.glob', return_value=[]):inputs=MenuInput()
            inputs.descriptors=[7];inputs.codes[7]=ANBERNIC_MENU_CODES;inputs.starts[7]=311
            inputs.calibrated[7]=True;inputs.mainline[7]=False
            packets=b''.join(EVENT.pack(0,0,1,code,down) for down in (1,0) for code in codes)
            with patch('game_menu.select.select',return_value=([7],[],[])),patch('game_menu.os.read',return_value=packets):
                self.assertFalse(inputs.poll(), codes)
            self.clock += .2
            with patch('game_menu.select.select',return_value=([],[],[])):
                self.assertEqual(inputs.poll(),'menu',codes)
                self.assertFalse(inputs.poll(),codes)

    def test_home_changes_open_switcher_to_game_menu_without_resuming(self):
        with patch.object(self.menu.inputs, 'poll', return_value='game-menu'):
            self.menu.poll()
        self.assertTrue(self.menu.paused)
        self.assertIs(self.menu.menu, self.ui)
        self.assertEqual(menus.read_fields(self.menu.status)['view_request'], '1')
        self.assertIsNone(self.menu.pending)

    def test_anbernic_opens_switcher_from_the_existing_game_menu(self):
        self.clock += 1
        with patch.object(self.menu.inputs, 'poll', return_value='menu'):
            self.menu.poll()
        self.assertTrue(self.menu.paused)
        self.assertIs(self.menu.menu, self.ui)
        status=menus.read_fields(self.menu.status)
        self.assertEqual(status['view_request'], '1')
        self.assertEqual(status['view'], 'switcher')
        self.assertIsNone(self.menu.pending)

    def test_plain_exit_acknowledgement_does_not_prove_a_switch_save(self):
        self.menu.checkpoint().parent.mkdir(parents=True, exist_ok=True)
        self.menu.checkpoint().write_bytes(b'previous checkpoint')
        self.request('switch:1')
        self.result('quitting')
        self.child.result = 0
        self.assertIsNone(self.menu.completed_switch())

    def test_save_failure_preserves_current_game_and_cancels_switch(self):
        for state in ('save-failed', 'save-timeout'):
            self.request('switch:1', state)
            self.result(state)
            self.assertIsNone(self.menu.switch_target)
            self.assertIsNone(self.child.poll())
            self.assertTrue(self.menu.paused)
            self.assertEqual(menus.read_fields(self.menu.status)['state'], 'error')

    def test_old_acknowledgement_or_missing_checkpoint_cannot_switch(self):
        self.request('switch:1')
        self.result('saved', token='old-token')
        self.child.result = 0
        self.assertIsNone(self.menu.completed_switch())
        write_fields(Path(str(self.menu.control) + '.result'), {'token': self.menu.pending['token'], 'state': 'saved'})
        self.assertIsNone(self.menu.completed_switch())

    def test_snapshot_deduplicates_and_invalid_selection_does_not_stop_game(self):
        self.assertEqual(len(self.menu.switch_games), 2)
        for index in ('-1', '99', 'nope'):
            self.request('switch:' + index, index)
            self.assertIsNone(self.menu.pending)
            self.assertIsNone(self.child.poll())
        self.next_game.unlink()
        self.request('switch:1', 'gone')
        self.assertIsNone(self.menu.pending)

    def test_resume_and_force_quit_do_not_launch_another_game(self):
        self.request('switch:1')
        self.result('save-failed')
        self.request('resume', 'resume')
        self.assertIsNone(self.menu.completed_switch())
        self.assertIsNone(self.menu.switch_target)

    def test_runner_reuses_session_and_forces_autoload_only_for_switch(self):
        calls = []
        def once(core, app, data, rom, display_handoff, fresh_start=False, display=None):
            calls.append((core, rom, display_handoff, os.environ.get('RGDS_FORCE_AUTOLOAD')))
            return SwitchGame('melonds', self.next_game) if len(calls) == 1 else 0
        with patch.dict(os.environ, {'RGDS_FORCE_AUTOLOAD': '0'}), patch.object(emulators, '_run_once', side_effect=once):
            self.assertEqual(emulators.run('drastic', self.menu.app, self.menu.data, self.menu.rom, 'handoff'), 0)
            self.assertEqual(os.environ['RGDS_FORCE_AUTOLOAD'], '0')
        self.assertEqual(calls, [('drastic', self.menu.rom, 'handoff', '0'), ('melonds', self.next_game, None, '1')])


if __name__ == '__main__':
    unittest.main()
