"""Slow/failing status helpers cannot block input or replace good hardware data."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
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
p.add_argument("--power",type=int);p.add_argument("--lid",type=int)
a=p.parse_args();a.data.mkdir(parents=True,exist_ok=True)
if a.command=="session-start":
 if (a.data/"stuck").exists():time.sleep(60)
 time.sleep(.3)
 (a.data/"session-start").write_text(f"{a.value} {a.power} {a.lid}")
 with (a.data/"order").open("a") as f:f.write("session-start\\n")
 print("brightness_supported=1\\nbrightness=40\\nbrightness_failed=1\\nbattery=64\\nvolume_supported=1\\nvolume=35",flush=True)
elif a.command in ("power","lid"):
 time.sleep(.1)
 with (a.data/"order").open("a") as f:f.write(f"{a.command} {a.value}\\n")
 if (a.data/"fail-control").exists():sys.exit(1)
elif a.command=="status":
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
elif a.command=="brightness":
 (a.data/"brightness").write_text(str(a.value))
 with (a.data/"order").open("a") as f:f.write(f"brightness {a.value}\\n")
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
#include <iostream>
#include <thread>
using Clock=std::chrono::steady_clock;
void mode(const rgds::fs::path& data,const char* value){std::ofstream(data/"mode")<<value;}
std::vector<std::string> lines(const rgds::fs::path& path){std::vector<std::string> out;std::ifstream file(path);std::string line;while(std::getline(file,line))out.push_back(line);return out;}
rgds::SystemStatusUpdate settle(rgds::SystemControls& s){
 auto end=Clock::now()+std::chrono::seconds(3);
 while(Clock::now()<end){if(auto update=s.poll_status())return *update;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
 assert(false);return {};
}
int main(int argc,char** argv){
 assert(argc==4);rgds::fs::path root=argv[1],data=argv[2];std::string task=argv[3];
 if(task.rfind("battery-at:",0)==0){
  rgds::SystemStatus at;rgds::SystemControls::read_battery(at,task.substr(11));
  std::cout<<at.battery<<" "<<at.charging<<" "<<at.external_power<<std::endl;return 0;
 }
 rgds::fs::create_directories(data);rgds::SystemControls s(task=="preview",root/"app",data);
 if(task=="session-start"){
  auto began=Clock::now();
  s.request_session_start(40,2,true);
  assert(Clock::now()-began<std::chrono::milliseconds(200));
  s.discard_status();
  auto update=settle(s);
  assert(update.success&&update.value.battery==64);
  assert(update.value.brightness_failed&&!update.value.volume_failed);
  std::ifstream written(data/"session-start");std::string text;std::getline(written,text);
  assert(text=="40 2 1");
  return 0;
 }
 if(task=="session-order"){
  s.request_session_start(40,0,false);
  s.request_brightness(90);
  s.flush_controls();
  assert(lines(data/"order")==(std::vector<std::string>{"session-start","brightness 90"}));
  assert(s.poll_status()->success);
  return 0;
 }
 if(task=="session-stuck"){
  std::ofstream(data/"stuck")<<1;
  s.request_session_start(40,0,false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto began=Clock::now();
  s.finish();
  assert(Clock::now()-began<std::chrono::seconds(1));
  return 0;
 }
 if(task=="session-killed"){
  std::ofstream(data/"stuck")<<1;
  s.request_session_start(40,0,false);
  auto began=Clock::now();
  s.flush_controls();
  assert(Clock::now()-began<std::chrono::seconds(5));
  auto update=s.poll_status();assert(update&&!update->success);
  return 0;
 }
 if(task=="control"){
  auto began=Clock::now();
  s.request_control("lid",1);s.request_control("power",1);s.request_control("power",2);
  assert(Clock::now()-began<std::chrono::milliseconds(50));
  assert(s.control_pending());
  std::vector<rgds::ControlUpdate> seen;
  auto end=Clock::now()+std::chrono::seconds(4);
  while(Clock::now()<end&&seen.size()<2){if(auto update=s.poll_control())seen.push_back(*update);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  assert(seen.size()==2&&seen[0].command=="lid"&&seen[0].success&&seen[1].command=="power"&&seen[1].value==2&&seen[1].success);
  assert(!s.control_pending());
  assert(lines(data/"order")==(std::vector<std::string>{"lid 1","power 2"}));
  std::ofstream(data/"fail-control")<<1;
  s.request_control("power",3);
  s.flush_controls();
  auto failed=s.poll_control();assert(failed&&failed->command=="power"&&failed->value==3&&!failed->success);
  return 0;
 }
 if(task=="battery"){
  auto sys=root/"sys";auto put=[&](const char* name,const char* value){auto path=sys/name;rgds::fs::create_directories(path.parent_path());std::ofstream(path)<<value<<"\\n";};
  rgds::SystemStatus none;rgds::SystemControls::read_battery(none,sys);assert(none.battery==-1&&!none.charging);
  put("class/power_supply/battery/type","Battery");put("class/power_supply/battery/capacity","73");put("class/power_supply/battery/status","Charging");
  put("class/power_supply/charger/type","USB");put("class/power_supply/charger/online","1");
  rgds::SystemStatus charging;rgds::SystemControls::read_battery(charging,sys);
  assert(charging.battery==73&&charging.charging&&charging.external_power);
  put("class/power_supply/charger/online","0");put("class/power_supply/battery/capacity","garbage");
  rgds::SystemStatus bad;rgds::SystemControls::read_battery(bad,sys);
  assert(bad.battery==-1&&!bad.charging&&!bad.external_power);
  return 0;
 }
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

    def test_session_start_does_not_block_and_cannot_be_cancelled(self):
        self.invoke("session-start")

    def test_battery_is_read_without_a_helper(self):
        self.invoke("battery")

    def test_a_slider_waits_for_session_start_so_the_newest_value_lands_last(self):
        self.invoke("session-order")

    def test_power_off_does_not_wait_for_a_stuck_session_start(self):
        self.invoke("session-stuck")

    def test_a_stuck_session_start_is_killed_at_a_lifecycle_boundary_and_reported(self):
        self.invoke("session-killed")

    def test_menu_rows_apply_off_the_input_thread_in_order_with_the_newest_value(self):
        self.invoke("control")

    def test_battery_reader_agrees_with_the_runtime(self):
        sys.path.insert(0, str(ROOT / "runtime"))
        from system_controls import battery_status
        cases = (
            {}, {
                "battery": {
                    "type": "Battery",
                    "capacity": "73",
                    "status": "Charging"
                },
                "charger": {
                    "type": "USB",
                    "online": "1"
                }
            }, {
                "battery": {
                    "type": "Battery",
                    "capacity": "73",
                    "status": "Charging"
                },
                "charger": {
                    "type": "USB_C",
                    "online": "0"
                }
            }, {
                "battery": {
                    "type": "Battery",
                    "capacity": "100",
                    "status": "Full"
                }
            }, {
                "battery": {
                    "type": "Battery",
                    "status": "Discharging",
                    "charge_now": "500",
                    "charge_full": "1000"
                }
            }, {
                "battery": {
                    "type": "Battery",
                    "status": "Not charging",
                    "energy_now": "1",
                    "energy_full": "3"
                }
            }, {
                "battery": {
                    "type": "Battery",
                    "capacity": "garbage",
                    "status": "Discharging"
                }
            }, {
                "mains": {
                    "type": "Mains",
                    "online": "1"
                }
            }
        )
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                for supply, fields in case.items():
                    path = Path(folder) / "class/power_supply" / supply
                    path.mkdir(parents=True)
                    for name, value in fields.items():
                        (path / name).write_text(value + "\n")
                expected = battery_status(folder)
                result = subprocess.run(
                    [str(self.binary),
                     str(self.root),
                     str(Path(folder) / "data"), "battery-at:" + folder],
                    capture_output=True,
                    text=True,
                    timeout=6
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout.split(),
                    [str(expected["battery"]),
                     str(expected["charging"]),
                     str(expected["external_power"])]
                )

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
