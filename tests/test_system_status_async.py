"""Slow/failing status helpers cannot block input or replace good hardware data."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class SystemStatusAsyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        runtime = cls.root / "app/runtime"
        runtime.mkdir(parents=True)
        (runtime / "system_controls.py").write_text(
            '''import argparse, os, subprocess, sys, time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("command");p.add_argument("value",nargs="?",type=int);p.add_argument("--data",type=Path,required=True)
p.add_argument("--menu-wake",action="store_true");p.add_argument("--menu-close",action="store_true")
a=p.parse_args();a.data.mkdir(parents=True,exist_ok=True)
if a.command=="status":
 (a.data/"pid").write_text(str(os.getpid()))
 mode=(a.data/"mode").read_text() if (a.data/"mode").exists() else "ready"
 if mode=="empty":sys.exit(0)
 if mode=="oversize":print("x"*9000);sys.exit(0)
 print("brightness_supported=1",flush=True)
 if mode in ("slow","child"):
  if mode=="child":
   child=subprocess.Popen([sys.executable,"-c","import time;time.sleep(10)"])
   (a.data/"child-pid").write_text(str(child.pid))
  time.sleep(.3)
 print("brightness=75\\nbattery=82\\nvolume_supported=1\\nvolume=35",flush=True)
 if mode=="fail":sys.exit(1)
 if mode=="bad":print("battery=garbage",flush=True)
elif a.command in ("sleep","sleep_lid"):
 assert a.menu_wake
 assert a.menu_close == (a.command=="sleep_lid")
elif a.command=="brightness":(a.data/"brightness").write_text(str(a.value))
elif a.command=="volume":
 if a.value is None:
  assert not (a.data/"pid").exists(), "Must work before status exists"
  (a.data/"volume-restored").write_text("saved")
  if (a.data/"fail").exists():sys.exit(1)
'''
        )
        source = cls.root / "test.cpp"
        source.write_text(
            '''#include "platform/system_controls.hpp"
#include <cassert>
#include <thread>
using Clock=std::chrono::steady_clock;
void mode(const rgds::fs::path& data,const char* value){std::ofstream(data/"mode")<<value;}
rgds::SystemStatusUpdate settle(rgds::SystemControls& s){
 auto end=Clock::now()+std::chrono::seconds(3);
 while(Clock::now()<end){if(auto update=s.poll_status())return *update;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
 assert(false);return {};
}
int main(int argc,char** argv){
 assert(argc==4);rgds::fs::path root=argv[1],data=argv[2];std::string task=argv[3];
 rgds::fs::create_directories(data);rgds::SystemControls s(task=="preview",root/"app",data);
 if(task=="audio-owner"){assert(s.sleep(true,true));assert(s.sleep(false,false));return 0;}
 if(task=="audio-sync"){
  assert(s.sync_volume());assert(rgds::fs::exists(data/"volume-restored"));
  std::ofstream(data/"fail")<<1;assert(!s.sync_volume());return 0;
 }
 auto first=s.status();assert(first.battery==82 && first.brightness);
 if(task=="preview"){
  assert(s.request_status());auto update=s.poll_status();assert(update && update->success && update->value.battery==82);
  assert(s.request_status());assert(s.set("volume",55));assert(!s.poll_status());
  s.request_brightness(90);assert(s.flush_brightness());assert(s.request_status());auto fresh=s.poll_status();
  assert(fresh && fresh->success && fresh->value.brightness_percent==90 && fresh->value.volume_percent==55);return 0;
 }
 if(task=="slow"){
  mode(data,"slow");auto began=Clock::now();assert(s.request_status());assert(!s.request_status());
  assert(!s.poll_status());assert(Clock::now()-began<std::chrono::milliseconds(80));
  auto result=settle(s);assert(result.success && result.value.brightness_percent==75 && result.value.volume_percent==35);
 }else if(task=="fail" || task=="empty" || task=="bad" || task=="oversize"){
  mode(data,task.c_str());assert(s.request_status());auto result=settle(s);
  assert(!result.success && result.value.battery==first.battery && result.value.brightness_percent==first.brightness_percent);
  mode(data,"ready");assert(s.request_status());assert(settle(s).success);
 }else if(task=="cancel" || task=="cancel-volume"){
  mode(data,"slow");assert(s.request_status());auto began=Clock::now();
  if(task=="cancel")s.request_brightness(90);else s.request_volume(90);
  assert(Clock::now()-began<std::chrono::milliseconds(80));assert(!s.request_status());
  auto result=settle(s);assert(!result.success && result.value.battery==82);
  if(task=="cancel"){assert(s.flush_brightness());std::ifstream applied(data/"brightness");int value=0;applied>>value;assert(value==90);}
  else{assert(s.flush_volume());assert(s.cached().volume_percent==90);}
 }else if(task=="cleanup"){
  mode(data,"child");assert(s.request_status());auto end=Clock::now()+std::chrono::seconds(2);
  while(!rgds::fs::exists(data/"child-pid") && Clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(rgds::fs::exists(data/"child-pid"));
 }else return 2;
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

    def invoke(self, task):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            result = subprocess.run(
                [str(self.binary), str(self.root), str(data), task],
                capture_output=True,
                text=True,
                env=dict(os.environ, RGDS_SYSTEM_MANAGED="1"),
                timeout=6
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if task == "cleanup":
                helper = int((data / "pid").read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(helper, 0)
                # All group members were signalled. A child may briefly remain
                # as an init-owned zombie on Linux; it must not still be running.
                child = int((data / "child-pid").read_text())
                for _ in range(50):
                    try:
                        os.kill(child, 0)
                    except ProcessLookupError:
                        break
                    if Path(f"/proc/{child}/stat").exists():
                        state = Path(f"/proc/{child}/stat").read_text().rsplit(')', 1)[1].split()[0]
                        if state == "Z": break
                    import time
                    time.sleep(.01)
                else:
                    self.fail("Status helper's mixer child remained running after destruction")

    def test_sleep_transfers_only_the_requested_audio_ownership(self):
        self.invoke("audio-owner")

    def test_audio_restore_reads_saved_level_before_status_and_reports_failure(self):
        self.invoke("audio-sync")

    def test_slow_partial_output_does_not_block_and_requests_do_not_duplicate(self):
        self.invoke("slow")

    def test_failure_preserves_the_last_good_snapshot(self):
        self.invoke("fail")

    def test_empty_successful_output_is_rejected(self):
        self.invoke("empty")

    def test_malformed_status_is_rejected(self):
        self.invoke("bad")

    def test_oversize_output_is_bounded_and_can_recover(self):
        self.invoke("oversize")

    def test_interactive_brightness_cancels_slow_status_and_preserves_write_order(self):
        self.invoke("cancel")

    def test_interactive_volume_cancels_slow_status_without_blocking(self):
        self.invoke("cancel-volume")

    def test_destruction_stops_the_helper_and_its_mixer_process(self):
        self.invoke("cleanup")

    def test_preview_uses_the_cached_simulation_without_a_helper(self):
        self.invoke("preview")


if __name__ == "__main__": unittest.main()
