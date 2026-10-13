"""Automatic DSperate resume must load the selected game and preserve newer saves."""
import os
import json
import struct
from pathlib import Path
import unittest
from unittest.mock import patch

import emulators
from fields import write_fields
from library import game_key
from test_runtime import Fixture


class DSperateResumeTests(Fixture):
    def setUp(self):
        super().setUp()
        self.binary('dsperate')
        self.rom=self.root/'Game.nds'
        self.rom.write_bytes(b'\0'*12+b'ABCD'+b'\0'*496)
        self.canonical=self.data/'library'/game_key(self.rom)/'auto-dsperate.dss'
        self.native=self.data/'emulators/dsperate/states/ABCD.auto.dss'
        self.battery=self.data/'emulators/dsperate/saves/Game.sav'
        self.manual=self.native.with_name('ABCD.0.dss')
        self.saved_bytes={}

    def state(self, contents=b'', rom=None):
        rom = rom or self.rom
        header = rom.read_bytes()[:0x160]
        identity = 1469598103934665603
        for byte in header: identity = ((identity ^ byte) * 1099511628211) & 0xffffffffffffffff
        identity ^= rom.stat().st_size
        # Exact v4 HEAD layout, including the real-sized RGB565 thumbnail.
        head = struct.pack('<IQQQQIII', int.from_bytes(header[12:16], 'little'),
                           identity, 0, 0, 42, 1, 128, 96) + bytes(128*96*2) + struct.pack('<I', 0)
        return (b'DSST' + struct.pack('<I', 4) + b'HEAD' + struct.pack('<I', len(head)) + head
                + b'TEST' + struct.pack('<I', len(contents)) + contents
                + b'VIEW' + struct.pack('<I', 19) + bytes(19))

    def saved(self,path,contents,stamp,rom=None):
        if path.suffix == '.dss': contents=self.state(contents,rom)
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(contents)
        os.utime(path,ns=(stamp,stamp))
        self.saved_bytes[path]=contents

    def plan(self,**kwargs):
        with patch.dict(os.environ,{},clear=True):
            return emulators.launch_plan('dsperate',self.app,self.data,self.rom,**kwargs)

    def selected(self,plan):
        command=plan['command']
        self.assertIn('--no-autoload',command)
        return Path(command[command.index('--load-state')+1]) if '--load-state' in command else None

    def test_canonical_checkpoint_is_explicit_without_replacing_native_or_manual_states(self):
        self.saved(self.canonical,b'canonical state',300)
        self.saved(self.native,b'older native',100)
        self.saved(self.manual,b'manual slot',200)
        self.assertEqual(self.selected(self.plan()),self.canonical)
        for path in (self.native,self.manual,self.canonical):
            self.assertEqual(path.read_bytes(),self.saved_bytes[path])

    def test_later_native_state_is_retained_and_canonical_wins_equal_timestamps(self):
        self.saved(self.canonical,b'canonical state',100)
        self.saved(self.native,b'later native',300)
        self.assertEqual(self.selected(self.plan()),self.native)
        os.utime(self.canonical,ns=(300,300))
        self.assertEqual(self.selected(self.plan()),self.canonical)

    def test_newer_battery_prevents_both_explicit_and_implicit_state_loading(self):
        self.saved(self.canonical,b'canonical state',100)
        self.saved(self.native,b'native state',200)
        self.saved(self.battery,b'newer in-game save',300)
        self.assertIsNone(self.selected(self.plan()))
        self.assertEqual(self.battery.read_bytes(),b'newer in-game save')
        self.assertEqual(self.native.read_bytes(),self.saved_bytes[self.native])
        self.assertEqual(self.canonical.read_bytes(),self.saved_bytes[self.canonical])

    def test_newer_same_code_other_rom_state_cannot_override_canonical(self):
        other=self.root/'Other revision.nds'
        other.write_bytes(self.rom.read_bytes()+b'different revision')
        self.saved(self.canonical,b'correct game',100)
        self.saved(self.native,b'other game',300,rom=other)
        self.assertEqual(self.selected(self.plan()),self.canonical)
        self.canonical.unlink()
        self.assertIsNone(self.selected(self.plan()))
        self.assertEqual(self.native.read_bytes(),self.saved_bytes[self.native])

    def test_wrong_code_version_or_truncated_headers_never_auto_load(self):
        self.saved(self.native,b'correct fallback',100)
        valid=self.state()
        for offset,change in ((4,struct.pack('<I',3)),(8,b'NOPE'),(12,struct.pack('<I',0xffffffff)),
                             (16,b'EFGH'),(56,struct.pack('<I',0xffffffff))):
            modified=bytearray(valid);modified[offset:offset+len(change)]=change
            self.canonical.parent.mkdir(parents=True,exist_ok=True);self.canonical.write_bytes(modified)
            self.assertEqual(self.selected(self.plan()),self.native)
        self.canonical.write_bytes(valid[:64])
        self.assertEqual(self.selected(self.plan()),self.native)
        self.assertEqual(self.native.read_bytes(),self.saved_bytes[self.native])

    def test_disabled_auto_load_overrides_existing_native_state(self):
        self.saved(self.canonical,b'canonical state',100)
        self.saved(self.native,b'native state',200)
        write_fields(self.data/'preferences.ini',dict(auto_load=0))
        self.assertIsNone(self.selected(self.plan()))

    def test_archive_identity_selects_its_canonical_state(self):
        archive=self.root/'collection/Game.zip'
        selected=self.data/'library'/game_key(archive)/'auto-dsperate.dss'
        self.saved(selected,b'archive state',200)
        self.saved(self.canonical,b'extracted path state',300)
        self.assertEqual(self.selected(self.plan(identity=archive)),selected)

    def test_dry_plan_does_not_read_states_or_create_userdata(self):
        with patch.object(emulators,'dsperate_resume_state',side_effect=AssertionError('Dry run reads states')):
            self.assertIsNone(self.selected(self.plan(prepare=False)))
        self.assertFalse(self.data.exists())

    def test_atomic_save_capability_requires_verified_packaged_source_and_binary(self):
        binary=self.app/'emulators/dsperate/dsperate'
        record=dict(kind='source-built-dsperate', source=dict(
            commit='1b76c355109c9f7576363ccc023927b3137d3c6f',
            patched_inputs_sha256='c8f4a7243832dd609cca4b0b33fed3120fadbf0a3106c0f8b497981c91ec9129'),
            binary=dict(sha256='a'*64))
        build=binary.parent/'build.json';build.write_text(json.dumps(record))
        manifest=dict(files={'emulators/dsperate/build.json':'b'*64,'emulators/dsperate/dsperate':'a'*64})
        self.assertTrue(emulators.dsperate_atomic_states(self.app,binary,manifest))
        (self.app/'manifest.json').write_text('{}')
        with patch('activate.verify_engine',return_value=manifest) as verify:
            self.assertEqual(self.plan()['env'].get('RGDS_STATE_ATOMIC_REPLACE'),'1')
            verify.assert_called_once_with(self.app,'dsperate')
            self.assertNotIn('RGDS_STATE_ATOMIC_REPLACE',self.plan(prepare=False)['env'])
        (self.app/'manifest.json').unlink()
        self.assertFalse(emulators.dsperate_atomic_states(self.app,binary,None))
        self.assertFalse(emulators.dsperate_atomic_states(self.app,self.root/'external-dsperate',manifest))
        self.assertFalse(emulators.dsperate_atomic_states(self.app,binary,dict(files={})))
        for field in ('commit','patched_inputs_sha256'):
            modified=json.loads(json.dumps(record));modified['source'][field]='unreviewed'
            build.write_text(json.dumps(modified))
            self.assertFalse(emulators.dsperate_atomic_states(self.app,binary,manifest))
        build.write_text(json.dumps(record));manifest['files']['emulators/dsperate/dsperate']='c'*64
        self.assertFalse(emulators.dsperate_atomic_states(self.app,binary,manifest))
        with patch.dict(os.environ,{'RGDS_STATE_ATOMIC_REPLACE':'1'},clear=True):
            self.assertNotIn('RGDS_STATE_ATOMIC_REPLACE',emulators.base_environment())
        self.assertNotIn('RGDS_STATE_ATOMIC_REPLACE',self.plan()['env'])


if __name__=='__main__':unittest.main()
