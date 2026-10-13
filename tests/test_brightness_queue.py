"""Exercise asynchronous brightness with a fake helper, never sysfs or a device."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class BrightnessQueueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        runtime = cls.root / "app/runtime"
        runtime.mkdir(parents=True)
        (runtime / "system_controls.py").write_text('''import argparse, json, sys, time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("command");p.add_argument("value",nargs="?",type=int);p.add_argument("--data",type=Path,required=True)
a=p.parse_args();a.data.mkdir(parents=True,exist_ok=True)
busy=a.data/"busy"
if busy.exists():sys.exit(7)
busy.touch()
with (a.data/"trace").open("a") as f:f.write(json.dumps([a.command,a.value])+"\\n")
if a.command=="brightness" and not (a.data/"fast").exists():time.sleep(.15)
busy.unlink()
sys.exit(1 if a.command=="brightness" and a.value==65 else 0)
''')
        source = cls.root / "test.cpp"
        source.write_text('''#include "platform/system_controls.hpp"
#include <cassert>
#include <thread>
using Clock=std::chrono::steady_clock;
std::optional<rgds::BrightnessUpdate> settle(rgds::SystemControls& s){
 auto end=Clock::now()+std::chrono::seconds(4);
 std::optional<rgds::BrightnessUpdate> last;
 while(Clock::now()<end){
  if(auto update=s.poll_brightness())last=update;
  if(!s.brightness_pending())return last;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
 }
 assert(false);return std::nullopt;
}
int main(int argc,char** argv){
 assert(argc==4);rgds::fs::path root=argv[1],data=argv[2];std::string mode=argv[3];
 if(mode=="preview"){
  rgds::SystemControls s(true,{},{});s.request_brightness(0);auto first=s.poll_brightness();
  assert(first && first->value==5 && first->success && first->settled);
  s.request_brightness(110);assert(s.flush_brightness());auto last=s.poll_brightness();
  assert(last && last->value==100 && last->success && last->settled);return 0;
 }
 rgds::SystemControls s(false,root/"app",data);
 if(mode=="coalesce"){
  auto began=Clock::now();s.request_brightness(10);s.poll_brightness();
  for(int n=15;n<=95;n+=5)s.request_brightness(n);
  assert(Clock::now()-began<std::chrono::milliseconds(80));
  auto result=settle(s);assert(result && result->value==95 && result->success && result->settled);
 }else if(mode=="active"){
  s.request_brightness(20);s.poll_brightness();s.request_brightness(90);s.request_brightness(20);
  auto result=settle(s);assert(result && result->value==20 && result->success && result->settled);
 }else if(mode=="failure"){
  s.request_brightness(65);auto failed=settle(s);
  assert(failed && failed->value==65 && !failed->success && failed->settled);
  s.request_brightness(80);assert(s.flush_brightness());auto next=s.poll_brightness();
  assert(next && next->value==80 && next->success && next->settled);
 }else if(mode=="cadence"){
  rgds::fs::create_directories(data);std::ofstream(data/"fast").put('1');auto began=Clock::now();
  for(int n=10;n<=60;n+=10){s.request_brightness(n);auto result=settle(s);assert(result && result->success);}
  assert(Clock::now()-began>=std::chrono::milliseconds(400));
 }else if(mode=="serialize"){
  s.request_brightness(25);s.poll_brightness();s.request_brightness(85);
  assert(s.set("power",2));
  auto result=s.poll_brightness();assert(result && result->value==85 && result->success && result->settled);
 }else return 2;
}
''')
        cls.binary = cls.root / "probe"
        subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "src"), str(source), "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, mode):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            environment = os.environ.copy()
            environment.pop("RGDS_SYSTEM_MANAGED", None)
            result = subprocess.run([str(self.binary), str(self.root), str(data), mode],
                                    capture_output=True, text=True, env=environment, timeout=6)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            trace = data / "trace"
            return [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []

    def test_motion_is_immediate_and_only_the_latest_waiting_value_is_applied(self):
        self.assertEqual(self.invoke("coalesce"), [["brightness", 10], ["brightness", 95], ["restore", None]])

    def test_returning_to_the_active_value_discards_obsolete_waiting_motion(self):
        self.assertEqual(self.invoke("active"), [["brightness", 20], ["restore", None]])

    def test_failed_apply_reports_failure_and_the_queue_can_recover(self):
        self.assertEqual(self.invoke("failure"), [["brightness", 65], ["brightness", 80], ["restore", None]])

    def test_other_hardware_controls_and_restore_wait_for_latest_brightness(self):
        self.assertEqual(self.invoke("serialize"), [["brightness", 25], ["brightness", 85], ["power", 2], ["restore", None]])

    def test_fast_helpers_still_use_a_bounded_apply_cadence(self):
        self.assertEqual(self.invoke("cadence"), [["brightness", n] for n in range(10, 61, 10)] + [["restore", None]])

    def test_preview_clamps_levels_and_never_invokes_hardware(self):
        self.assertEqual(self.invoke("preview"), [])


if __name__ == "__main__":
    unittest.main()
