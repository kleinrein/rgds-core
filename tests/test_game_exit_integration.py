"""Real runner/menu/engine subprocess exits, using stand-ins without audio or ROMs."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class GameExitIntegrationTests(unittest.TestCase):
    def exercise(self, core, scenario):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);app=root/"app";data=root/"userdata";rom=root/"fixture.nds"
            rom.write_bytes(b"fixture")
            (app/"bin").mkdir(parents=True)
            engine=root/"engine.py"
            engine.write_text('''import os, pathlib, sys, time
sys.path.insert(0, os.environ["TEST_RUNTIME"])
from library import read_fields, write_fields
data=pathlib.Path(os.environ["TEST_DATA"])
scenario=os.environ["TEST_SCENARIO"]
control=data/"session/emulator.control"
deadline=time.monotonic()+5
while time.monotonic()<deadline:
    try: token, action=control.read_text().split()
    except (OSError, ValueError): time.sleep(.01);continue
    if action=="quit":
        if scenario=="failed-save":
            write_fields(pathlib.Path(str(control)+".result"),dict(token=token,state="save-timeout"))
            time.sleep(.02);continue
        if scenario!="no-autosave":
            checkpoint=pathlib.Path(os.environ["TEST_CHECKPOINT"])
            checkpoint.parent.mkdir(parents=True,exist_ok=True)
            checkpoint.write_bytes((os.environ["TEST_CORE"]+" checkpoint").encode())
            write_fields(pathlib.Path(str(control)+".result"),dict(token=token,state="saved"))
            time.sleep(.1)
        sys.exit(0)
sys.exit(99)
''')
            launcher=app/"bin/rgds-launcher"
            launcher.write_text('#!'+sys.executable+'''\nimport os, pathlib, sys, time
sys.path.insert(0, os.environ["TEST_RUNTIME"])
from library import read_fields, write_fields
data=pathlib.Path(sys.argv[sys.argv.index("--data")+1])
write_fields(data/"session/game-menu.request",dict(token="ui-quit",action="resume" if os.environ["TEST_SCENARIO"]=="close-error" else "quit"))
deadline=time.monotonic()+5
while time.monotonic()<deadline:
    fields=read_fields(data/"session/game-menu.status")
    if fields.get("state")=="error":
        (data/"saw-save-error").write_text("yes")
        write_fields(data/"session/game-menu.request",dict(token="ui-confirm-exit",action="force-quit"))
    if fields.get("state")=="home":sys.exit(0)
    time.sleep(.01)
sys.exit(98)
''')
            launcher.chmod(0o755)
            harness=root/"runner.py"
            harness.write_text('''import os, pathlib, sys
from unittest.mock import patch
sys.path.insert(0, os.environ["TEST_RUNTIME"])
import emulators
from game_menu import GameMenu
from library import game_key, write_fields
data=pathlib.Path(os.environ["TEST_DATA"]);app=pathlib.Path(os.environ["TEST_APP"]);rom=pathlib.Path(os.environ["TEST_ROM"])
core=os.environ["TEST_CORE"]
if os.environ["TEST_SCENARIO"]=="no-autosave":write_fields(data/"preferences.ini",dict(auto_save=0))
checkpoint=data/"library"/game_key(rom)/("auto-"+core+".dss")
os.environ["TEST_CHECKPOINT"]=str(checkpoint)
checkpoint.parent.mkdir(parents=True,exist_ok=True);checkpoint.write_bytes(b"previous checkpoint")
class Inputs:
    def poll(self):return False
    def close(self):pass
class Session(GameMenu):
    instance=None
    def __init__(self,*args,**kwargs):
        supplied=kwargs.pop("inputs",None)
        if supplied:supplied.close()
        super().__init__(*args,inputs=Inputs())
        Session.instance=self
        if os.environ["TEST_SCENARIO"]=="menu-error":(app/"bin/rgds-launcher").unlink()
        self.trigger.touch()
    def close_menu(self):
        super().close_menu()
        if os.environ["TEST_SCENARIO"]=="close-error":raise OSError("Simulated recovery write failure")
plan=dict(command=[sys.executable,os.environ["TEST_ENGINE"]],cwd=str(app),env=os.environ.copy())
with patch.object(emulators,"launch_plan",return_value=plan),patch.object(emulators,"VolumeWatch"),patch.object(emulators,"GameMenu",Session):
    try:result=emulators.run(core,app,data,rom)
    except OSError:
        if os.environ["TEST_SCENARIO"] not in ("menu-error","close-error"):raise
        assert Session.instance.child.poll() is not None
        result="menu error recovered"
(data/"launcher-resumed").write_text(str(result))
''')
            environment=dict(os.environ,TEST_RUNTIME=str(ROOT/"runtime"),TEST_CORE=core,TEST_SCENARIO=scenario,
                TEST_ENGINE=str(engine),TEST_DATA=str(data),TEST_APP=str(app),TEST_ROM=str(rom))
            result=subprocess.run([sys.executable,str(harness)],env=environment,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertTrue((data/"launcher-resumed").is_file())
            self.assertFalse((data/"session/core.json").exists())
            self.assertFalse((data/"session/emulator.control.blocked").exists())
            if scenario not in ("menu-error","close-error"):self.assertIn("state=home",(data/"session/game-menu.status").read_text())
            checkpoint=next((data/"library").glob("*/auto-*.dss"))
            if scenario=="normal":
                self.assertEqual((data/"launcher-resumed").read_text(),"0")
                self.assertEqual(checkpoint.read_bytes(),(core+" checkpoint").encode())
            else:
                self.assertEqual(checkpoint.read_bytes(),b"previous checkpoint")
            if scenario=="failed-save":self.assertTrue((data/"saw-save-error").exists())

    def test_all_adapters_return_to_the_caller_after_checkpoint_and_engine_exit(self):
        for core in ("drastic","dsperate","nnddss"):
            with self.subTest(core=core):self.exercise(core,"normal")

    def test_failed_save_requires_explicit_exit_and_keeps_previous_checkpoint(self):
        self.exercise("drastic","failed-save")

    def test_auto_save_disabled_returns_without_replacing_checkpoint(self):
        self.exercise("dsperate","no-autosave")

    def test_menu_start_failure_does_not_leave_an_orphan_engine(self):
        self.exercise("nnddss","menu-error")

    def test_cleanup_write_failure_still_reaps_engine_and_clears_activity(self):
        self.exercise("dsperate","close-error")


if __name__=="__main__":unittest.main()
