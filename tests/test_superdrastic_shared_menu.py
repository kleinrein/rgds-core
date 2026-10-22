"""Exercise the real shared-menu pause loop without a handheld or DRM device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SharedMenuAdapterTests(unittest.TestCase):
    def test_pause_present_save_receipt_and_side_channel_isolation(self):
        if not shutil.which('c++'): self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            source = folder / 'test.cpp'
            source.write_text(
                r'''
#include <cassert>
#include <thread>
#include <fstream>
#include <filesystem>
#include "experimental/superdrastic/shared_menu.cpp"
static std::atomic<int> shows{0}, pauses{0}, saves{0}, released{0};
static uint32_t canvas[2][640*480];
extern "C" {
volatile int dsflip_hold;
int dsflip_menu_canvas(int p,uint32_t** out,int* pitch,int* w,int* h){*out=canvas[p];*pitch=2560;*w=640;*h=480;return 0;}
void dsflip_menu_show(int,int){shows++;}
int rgds_sd_menu_presented(int,int){return shows>=2;}
void rgds_sd_release_inputs(){released++;}
int dsflip_menu_grab_top(uint32_t*,int,int,int){return 0;}
void SDL_PauseAudio(int on){pauses=on;}
void audio_mute(int){} void audio_mic_quiet(int){}
int resume_menu_save(const char*,const char*,const char*){saves++;return 0;}
int resume_menu_load(const char*,const char*,const char*,const char*){return 0;}
int resume_quit_pending(){return 0;} void resume_press(int){}
int rgds_sd_idle(){return 1;} void rgds_sd_menu_state(int){} void rgds_sd_exit(const char*){}
}
static std::string read(const std::string& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){
 assert(argc==2);std::string dir=argv[1],ctl=dir+"/control",frame=dir+"/frame";
 setenv("RGDS_SD_SHARED_MENU","1",1);setenv("RGDS_CONTROL_FILE",ctl.c_str(),1);setenv("RGDS_OVERLAY_FILE",frame.c_str(),1);
 setenv("RGDS_SD_RESUME",(dir+"/resume").c_str(),1);
 rgds::OverlayFrame writer;assert(writer.open(frame,true));auto* pixels=writer.begin();
 for(size_t n=0;n<rgds::overlay_panel_bytes*2;n+=4){pixels[n]=0x12;pixels[n+1]=0x34;pixels[n+2]=0x56;pixels[n+3]=255;}
 writer.publish();std::ofstream(ctl+".blocked")<<"1";
 std::atomic<bool> returned{false};std::thread guest([&]{rgds_shared_poll(nullptr);returned=true;});
 for(int n=0;n<200 && __atomic_load_n(&writer.header()->reserved[0],__ATOMIC_ACQUIRE)==0;n++)usleep(10000);
 assert(writer.header()->reserved[0]>0);assert(!returned);assert(pauses==1);assert(released==1);
 assert(canvas[0][13]==0x123456 && canvas[1][640*479]==0x123456);
 std::ofstream(ctl)<<"save-1 save\n";guest.join();assert(saves==1);assert(rgds_shared_blocked());
 rgds_shared_job_done(1,1);assert(read(ctl+".result").find("state=native-saved")!=std::string::npos);
 std::ofstream(ctl+".speed-request")<<"token=speed-2\naction=fast-forward-off\n";
 std::filesystem::remove(ctl+".blocked");rgds_shared_poll(nullptr);
 assert(read(ctl+".result").find("token=save-1")!=std::string::npos);
 assert(!rgds_shared_blocked());assert(dsflip_hold==0);assert(pauses==0);
}
'''
            )
            binary = folder / 'test'
            subprocess.run(
                ['c++', '-std=c++17', '-pthread', '-I' + str(ROOT / 'src'),
                 str(source), '-o',
                 str(binary)],
                check=True,
                capture_output=True
            )
            subprocess.run([str(binary), str(folder)], check=True, capture_output=True, timeout=8)
