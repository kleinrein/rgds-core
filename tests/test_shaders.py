"""Check the shared catalog, original shader mappings and honest engine support."""
import configparser
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"runtime"))
from shaders import SHADERS, FILTERS, supported
from emulator_profiles import display_filter
from emulators import launch_plan
from library import read_fields,write_fields,game_key


class ShaderTests(unittest.TestCase):
    def test_catalog_preserves_previous_ids_and_every_original_preset(self):
        self.assertEqual(FILTERS[:3],("off","sharp_bilinear","lcd1x"))
        self.assertEqual(len(SHADERS),21)
        self.assertEqual([entry["asset_id"] for entry in SHADERS],[0,2,1,*range(3,21)])
        pack=ROOT/"vendor/engines/shaders"
        if pack.exists():
            for entry in SHADERS[1:15]:
                self.assertTrue((pack/(entry["file"]+".dsd")).is_file(),entry["file"])

    def test_presets_are_asset_data_and_native_profiles_have_no_legacy_control_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=root/"userdata";app=root/"app"
            binary=app/"emulators/dsperate/dsperate";binary.parent.mkdir(parents=True);binary.write_text("fixture");binary.chmod(0o755)
            bridge=app/"emulators/hooks/librgdsbridge.so";bridge.parent.mkdir();bridge.write_bytes(b"fixture")
            rom=root/"game.nds";content=bytearray(128);content[12:16]=b"RGDS";rom.write_bytes(content)
            plan=launch_plan("dsperate",app,data,rom)
            self.assertEqual(plan["env"]["LD_PRELOAD"],str(bridge))
            self.assertFalse(any(key.startswith("SIMPLEOS_") for key in plan["env"]))
            self.assertFalse((data/"emulators/simpleos-compat").exists())

    def test_extended_ids_roundtrip_and_dsperate_uses_the_shared_gpu_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=root/"data";app=root/"app";rom=root/"game.nds"
            binary=app/"emulators/dsperate/dsperate";binary.parent.mkdir(parents=True);binary.write_text("fixture");binary.chmod(0o755)
            write_fields(data/"ui.ini",dict(schema=1,filter=14))
            self.assertEqual(display_filter(data,rom),14)
            plan=launch_plan("dsperate",app,data,rom)
            config=configparser.ConfigParser();config.read(data/"emulators/dsperate/config.ini")
            self.assertEqual(config["video"]["lcd_grid"],"0")
            self.assertFalse(config["video"].getboolean("linear"))
            self.assertEqual(plan["env"]["RGDS_SHADER"],"14")
            self.assertEqual(plan["env"]["SDL_RENDER_DRIVER"],"opengles2")
            write_fields(data/"ui.ini",dict(schema=1,filter=1))
            self.assertEqual(launch_plan("dsperate",app,data,rom)["env"]["RGDS_SHADER"],"1")
            config.read(data/"emulators/dsperate/config.ini")
            self.assertFalse(config["video"].getboolean("linear"),"The shared Sharp shader owns interpolation")
            write_fields(data/"library"/game_key(rom)/"game.ini",dict(schema=1,filter=13))
            self.assertEqual(display_filter(data,rom),13)
        self.assertEqual([index for index in range(len(SHADERS)) if supported("dsperate",index)],list(range(len(SHADERS))))
        self.assertTrue(all(supported("drastic",index) and supported("nnddss",index) for index in range(len(SHADERS))))
        self.assertFalse(supported("unknown",1))


if __name__=="__main__":unittest.main()
