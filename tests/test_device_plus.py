"""Plus identification, changed event numbering and unverified hardware boundaries."""
import json
import os
import shutil
import struct
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from device_profile import detect, stock_environment
from input_devices import discover
from linux_platform import hardware_profile
import emulators
import session


def bitmap(value):
    words = []
    while value:
        words.append(format(value & ((1 << 64) - 1), 'x'))
        value >>= 64
    return ' '.join(reversed(words or ['0']))


class PlusProfileTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sys = self.root / 'sys'
        self.dev = self.root / 'dev/input'
        self.dev.mkdir(parents=True)

    def panels(self, mode):
        for index in (1, 2):
            folder = self.sys / ('class/drm/card0-DSI-' + str(index))
            folder.mkdir(parents=True, exist_ok=True)
            (folder / 'status').write_text('connected\n')
            (folder / 'modes').write_text(mode + '\n')

    def model(self, value):
        path = self.sys / 'firmware/devicetree/base/model'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value + '\0')

    def device(self, event, name, *, touch=False):
        folder = self.sys / 'class/input' / event / 'device'
        (folder / 'capabilities').mkdir(parents=True)
        (folder / 'name').write_text(name)
        (folder / 'properties').write_text('2' if touch else '0')
        (folder / 'capabilities/abs').write_text(bitmap((1 << 53) | (1 << 54) if touch else 3))
        (folder / 'capabilities/key').write_text(bitmap(1 << (330 if touch else 304)))
        (self.dev / event).touch()

    def environment(self):
        with patch('device_profile.detect', side_effect=lambda: detect(self.sys)), \
             patch('input_devices.discover', side_effect=lambda: discover(self.sys / 'class/input', self.dev)):
            return stock_environment({})

    def test_model_aliases_and_reused_board_identifier(self):
        for model in ('Anbernic RG DS Plus', 'anbernic,rg-ds-plus', 'RGDS+'):
            self.model(model)
            self.assertEqual(detect(self.sys)['model'], 'rg-ds-plus')
            self.assertEqual(detect(self.sys)['source'], 'device-tree')
        self.model('Anbernic RG DS')
        self.panels('640x480')
        self.assertEqual(detect(self.sys)['model'], 'rg-ds')
        self.panels('1024x768')
        self.assertEqual(detect(self.sys)['model'], 'rg-ds-plus')
        self.assertEqual(detect(self.sys)['source'], 'panel-modes')

    def test_unknown_and_disconnected_panels_do_not_claim_plus(self):
        self.assertEqual(detect(self.sys)['model'], 'unknown')
        self.panels('1024x768')
        self.assertEqual(detect(self.sys)['model'], 'rg-ds-plus')
        (self.sys / 'class/drm/card0-DSI-2/status').write_text('disconnected')
        self.assertEqual(detect(self.sys)['model'], 'unknown')

    def test_single_lower_touch_and_reordered_controller_reach_engines(self):
        self.panels('1024x768')
        self.device('event9', 'Goodix lower', touch=True)
        self.device('event6', 'ANBERNIC-rk3568-keys')
        environment = self.environment()
        self.assertEqual(environment['RGDS_DEVICE_MODEL'], 'rg-ds-plus')
        self.assertEqual(environment['RGDS_TOUCH_DEV'], str(self.dev / 'event9'))
        self.assertEqual(environment['RGDS_KEYS_DEV'], str(self.dev / 'event6'))
        self.assertEqual(environment['RGDS_INPUT_AMBIGUOUS'], '0')
        with patch.dict(os.environ, environment, clear=True):
            engine = emulators.base_environment()
        self.assertEqual(engine['RGDS_KEYS_DEV'], str(self.dev / 'event6'))

    def test_stock_session_rediscovers_plus_input_after_cleaning_inherited_state(self):
        self.model('RG DS Plus')
        self.device('event9', 'Goodix lower', touch=True)
        self.device('event6', 'ANBERNIC-rk3568-keys')
        runtime = self.root / 'run'
        runtime.mkdir()
        socket = runtime / 'wayland-0'
        socket.touch()
        library = Path('/usr/lib/libSDL2-2.0.so.0')
        with patch.dict(os.environ, dict(XDG_RUNTIME_DIR=str(runtime), RGDS_TOUCH_DEV='/dev/input/event1',
                                       RGDS_KEYS_DEV='/dev/input/event4', RGDS_DEVICE_MODEL='stale'), clear=True), \
             patch('device_profile.detect', side_effect=lambda: detect(self.sys)), \
             patch('input_devices.discover', side_effect=lambda: discover(self.sys / 'class/input', self.dev)), \
             patch.object(Path, 'is_socket', side_effect=lambda: True), \
             patch.object(Path, 'is_file', autospec=True, side_effect=lambda path: path == library):
            environment = session.environment(self.root)
        self.assertEqual(environment['RGDS_DEVICE_MODEL'], 'rg-ds-plus')
        self.assertEqual(environment['RGDS_TOUCH_DEV'], str(self.dev / 'event9'))
        self.assertEqual(environment['RGDS_KEYS_DEV'], str(self.dev / 'event6'))
        self.assertEqual(environment['RGDS_SDL_LIBRARY'], str(library))

    def test_multiple_touch_candidates_do_not_guess_raw_plus_input(self):
        self.model('RG DS Plus')
        self.device('event0', 'Goodix one', touch=True)
        self.device('event1', 'Goodix two', touch=True)
        environment = self.environment()
        self.assertEqual(environment['RGDS_TOUCH_DEV'], 'none')
        self.assertNotIn('RGDS_KEYS_DEV', environment)
        self.assertEqual(environment['RGDS_INPUT_AMBIGUOUS'], '1')

    def test_vendor_touch_without_direct_property_is_selected_but_trackpad_is_not(self):
        self.model('RG DS Plus')
        self.device('event9', 'Goodix lower', touch=True)
        lower = self.sys / 'class/input/event9/device'
        (lower / 'properties').write_text('0')
        self.device('event2', 'USB touchpad', touch=True)
        pad = self.sys / 'class/input/event2/device'
        (pad / 'properties').write_text('0')
        (pad / 'capabilities/key').write_text(bitmap((1 << 330) | (1 << 325)))
        self.assertEqual(self.environment()['RGDS_TOUCH_DEV'], str(self.dev / 'event9'))

    def test_independent_plus_profile_keeps_distinct_screen_requirement(self):
        path = self.root / 'hardware.json'
        profile = dict(schema=1, model='rg-ds-plus', top_display='DSI-1', bottom_display='DSI-2')
        path.write_text(json.dumps(profile))
        self.assertEqual(hardware_profile(path), profile)
        profile['bottom_display'] = 'DSI-1'
        path.write_text(json.dumps(profile))
        with self.assertRaises(ValueError):
            hardware_profile(path)

    def test_plus_captures_keep_logical_texture_budget_and_top_panel(self):
        from menu_snapshot import top_panel
        for width, height in ((1024, 768), (1024, 1536), (2048, 768)):
            stride = width * 3
            pixels = b''.join(bytes((y % 256, 10, 20)) * width for y in range(height))
            header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
            header += struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
            source = self.root / 'native.bmp'
            source.write_bytes(header + pixels)
            output = self.root / 'logical.bmp'
            top_panel(source, output)
            result = output.read_bytes()
            self.assertEqual(struct.unpack_from('<ii', result, 18), (640, -480))
            self.assertEqual(result[54:57], bytes((0, 10, 20)))
            self.assertEqual(result[-3:], bytes(((479 * 768 // 480) % 256, 10, 20)))


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'C++/SDL headers required')
class PlusBridgeTests(unittest.TestCase):
    def test_stock_plus_uses_runtime_guid_and_actual_display_indices(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'plus.cpp'
            source.write_text(
                r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
std::string mapping;bool registered=false;
int init(Uint32){return 0;}
int count(){return 2;}
const char* name(int index){return index?"DSI-1":"DSI-2";}
const char* joystick_name(int index){return index?"USB gamepad":"ANBERNIC-rk3568-keys";}
SDL_JoystickGUID guid(int){return {};}
void guid_text(SDL_JoystickGUID,char* out,int){std::strcpy(out,"03000000aabbccdd1122334400000000");}
int add(const char* value){mapping=value;registered=true;return 1;}
SDL_bool controller(int){return registered?SDL_TRUE:SDL_FALSE;}
extern "C" void* dlopen(const char*,int){return reinterpret_cast<void*>(1);}
extern "C" void* dlsym(void*,const char* symbol){
 if(!std::strcmp(symbol,"SDL_Init"))return reinterpret_cast<void*>(&init);
 if(!std::strcmp(symbol,"SDL_GetNumVideoDisplays"))return reinterpret_cast<void*>(&count);
 if(!std::strcmp(symbol,"SDL_GetDisplayName"))return reinterpret_cast<void*>(&name);
 if(!std::strcmp(symbol,"SDL_JoystickNameForIndex"))return reinterpret_cast<void*>(&joystick_name);
 if(!std::strcmp(symbol,"SDL_JoystickGetDeviceGUID"))return reinterpret_cast<void*>(&guid);
 if(!std::strcmp(symbol,"SDL_JoystickGetGUIDString"))return reinterpret_cast<void*>(&guid_text);
 if(!std::strcmp(symbol,"SDL_GameControllerAddMapping"))return reinterpret_cast<void*>(&add);
 if(!std::strcmp(symbol,"SDL_IsGameController"))return reinterpret_cast<void*>(&controller);
 return nullptr;
}
int main(){
 setenv("RGDS_DEVICE_MODEL","rg-ds-plus",1);unsetenv("RGDS_SESSION_PLATFORM");
 setenv("RGDS_CORE","dsperate",1);setenv("RGDS_TOP_DISPLAY","DSI-1",1);setenv("RGDS_BOTTOM_DISPLAY","DSI-2",1);
 assert(SDL_Init(SDL_INIT_VIDEO)==0);
 assert(std::getenv("DS_DUAL_SCREENS") && std::string(std::getenv("DS_DUAL_SCREENS"))=="upper=1,lower=0");
 assert(SDL_IsGameController(0)==SDL_TRUE);
 assert(mapping.rfind("03000000aabbccdd1122334400000000,",0)==0);
 assert(mapping.find("guide:b8,start:b7")!=std::string::npos);
 registered=false;mapping.clear();SDL_IsGameController(1);assert(mapping.empty());
}
'''
            )
            flags = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
            compiled = subprocess.run(
                [
                    'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags, '-I' + str(ROOT / 'src'),
                    str(source), '-o',
                    str(root / 'plus')
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(root / 'plus')], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
