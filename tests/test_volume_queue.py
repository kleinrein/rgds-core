"""Volume motion stays responsive while journalled control helpers serialize."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class VolumeQueueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        runtime = cls.root / "app/runtime"
        runtime.mkdir(parents=True)
        (runtime / "system_controls.py").write_text(
            '''import argparse, json, os, sys, time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("command");p.add_argument("value",nargs="?",type=int);p.add_argument("--data",type=Path,required=True)
a=p.parse_args();a.data.mkdir(parents=True,exist_ok=True)
busy=a.data/"busy"
try:fd=os.open(busy,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600);os.close(fd)
except FileExistsError:sys.exit(7)
with (a.data/"trace").open("a") as f:f.write(json.dumps([a.command,a.value])+"\\n")
if a.command in ("volume","brightness") and not (a.data/"fast").exists():time.sleep(.15)
if a.command=="volume" and a.value!=65:(a.data/"volume.ini").write_text("schema=1\\nvolume="+str(a.value)+"\\n")
busy.unlink()
sys.exit(1 if a.command=="volume" and a.value==65 else 0)
'''
        )
        source = cls.root / "test.cpp"
        source.write_text(
            '''#include "platform/system_controls.hpp"
#include <cassert>
#include <thread>
using Clock=std::chrono::steady_clock;
rgds::VolumeUpdate settle(rgds::SystemControls& s){
 auto end=Clock::now()+std::chrono::seconds(4);std::optional<rgds::VolumeUpdate> last;
 while(Clock::now()<end){
  if(auto update=s.poll_volume())last=update;s.poll_brightness();
  if(!s.volume_pending() && !s.brightness_pending()){assert(last);return *last;}
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
 }assert(false);return {};
}
int main(int argc,char** argv){
 assert(argc==4);rgds::fs::path root=argv[1],data=argv[2];std::string mode=argv[3];
 if(mode=="preview"){
  rgds::SystemControls s(true,{},{});s.status();s.request_volume(-5);auto first=s.poll_volume();
  assert(first && first->value==0 && first->success && first->settled);
  s.request_volume(120);s.request_brightness(90);assert(s.flush_volume());
  auto result=s.poll_volume();assert(result && result->value==100 && result->success && result->settled);
  assert(s.cached().volume_percent==100 && s.cached().brightness_percent==90);return 0;
 }
 rgds::SystemControls s(false,root/"app",data);
 if(mode=="coalesce"){
  auto began=Clock::now();s.request_volume(10);s.poll_volume();for(int n=15;n<=95;n+=5)s.request_volume(n);
  assert(Clock::now()-began<std::chrono::milliseconds(80));auto result=settle(s);
  assert(result.value==95 && result.success && result.settled && s.cached().volume_percent==95);
 }else if(mode=="active"){
  s.request_volume(20);s.poll_volume();s.request_volume(90);s.request_volume(20);
  auto result=settle(s);assert(result.value==20 && result.success && result.settled);
 }else if(mode=="failure"){
  s.request_volume(80);assert(s.flush_volume());s.poll_volume();s.request_volume(65);
  auto result=settle(s);assert(result.value==65 && !result.success && result.settled && s.cached().volume_percent==80);
  s.request_volume(90);assert(s.flush_volume());auto recovered=s.poll_volume();
  assert(recovered && recovered->success && recovered->value==90 && recovered->settled);
 }else if(mode=="serialize"){
  s.request_volume(10);s.poll_volume();s.request_brightness(55);s.request_volume(20);s.request_brightness(90);s.request_volume(85);
  assert(!s.request_status());assert(s.set("power",2));
  assert(!s.volume_pending() && !s.brightness_pending());assert(s.cached().volume_percent==85 && s.cached().brightness_percent==90);
  auto volume=s.poll_volume();auto brightness=s.poll_brightness();
  assert(volume && volume->value==85 && volume->success && volume->settled);
  assert(brightness && brightness->value==90 && brightness->success && brightness->settled);
 }else if(mode=="fair"){
  s.request_brightness(50);s.request_volume(10);s.poll_brightness();s.request_brightness(60);
  auto result=settle(s);assert(result.value==10 && result.success && s.cached().brightness_percent==60);
 }else if(mode=="cadence"){
  rgds::fs::create_directories(data);std::ofstream(data/"fast").put('1');auto began=Clock::now();
  for(int n=10;n<=60;n+=10){s.request_volume(n);assert(settle(s).success);}
  assert(Clock::now()-began>=std::chrono::milliseconds(400));
 }else if(mode=="mute"){
  s.request_volume(70);s.poll_volume();s.request_volume(0);auto result=settle(s);
  assert(result.success && result.settled && result.value==0 && s.cached().volume_percent==0);
 }else if(mode=="cleanup")s.request_volume(90);else return 2;
}
'''
        )
        cls.binary = cls.root / "probe"
        subprocess.run(
            [
                "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                str(source), "-o",
                str(cls.binary)
            ],
            check=True,
            capture_output=True,
            text=True
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, mode):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            environment = os.environ.copy()
            environment.pop("RGDS_SYSTEM_MANAGED", None)
            result = subprocess.run(
                [str(self.binary), str(self.root), str(data), mode],
                capture_output=True,
                text=True,
                env=environment,
                timeout=6
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            trace = data / "trace"
            records = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
            successful = [value for command, value in records if command == "volume" and value != 65]
            if successful: self.assertIn(f"volume={successful[-1]}\n", (data / "volume.ini").read_text())
            return records

    def test_volume_motion_is_immediate_and_waiting_values_coalesce(self):
        self.assertEqual(self.invoke("coalesce"), [["volume", 10], ["volume", 95], ["restore", None]])

    def test_return_to_the_active_value_discards_obsolete_motion(self):
        self.assertEqual(self.invoke("active"), [["volume", 20], ["restore", None]])

    def test_failed_apply_retains_last_applied_volume_and_recovers(self):
        self.assertEqual(self.invoke("failure"), [["volume", 80], ["volume", 65], ["volume", 90], ["restore", None]])

    def test_brightness_volume_power_and_restore_never_overlap(self):
        self.assertEqual(
            self.invoke("serialize"),
            [["volume", 10], ["brightness", 90], ["volume", 85], ["power", 2], ["restore", None]]
        )

    def test_waiting_volume_is_not_starved_by_brightness_repeats(self):
        self.assertEqual(
            self.invoke("fair"), [["brightness", 50], ["volume", 10], ["brightness", 60], ["restore", None]]
        )

    def test_apply_cadence_is_bounded_even_when_helpers_are_fast(self):
        self.assertEqual(self.invoke("cadence"), [["volume", n] for n in range(10, 61, 10)] + [["restore", None]])

    def test_exit_applies_the_latest_volume_before_restoring_hardware(self):
        self.assertEqual(self.invoke("cleanup"), [["volume", 90], ["restore", None]])

    def test_mute_zero_replaces_a_pending_audible_level_and_reaches_the_helper(self):
        self.assertEqual(self.invoke("mute"), [["volume", 70], ["volume", 0], ["restore", None]])

    def test_preview_clamps_both_controls_without_running_a_helper(self):
        self.assertEqual(self.invoke("preview"), [])


if __name__ == "__main__": unittest.main()
