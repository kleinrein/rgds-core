"""Run the real bridge with fake audio sinks: no hardware or speaker is opened."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++") and shutil.which("sdl2-config"), "C++/SDL headers required")
class AudioBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / "audio-test"
        helper = cls.root / "helper.py"
        helper.write_text('''import pathlib, sys
root = pathlib.Path(sys.argv[-1])
with (root / "trace").open("a") as f: f.write("sync\\n")
sys.exit(1 if (root / "fail").exists() else 0)
''')
        source = cls.root / "test.cpp"
        source.write_text('''#include "platform/emulator_bridge.cpp"
void trace(const char* message){std::ofstream f(fs::path(std::getenv("RGDS_VOLUME_DATA"))/"trace",std::ios::app);f<<message<<'\\n';}
SDL_AudioDeviceID fake_open(const char*,int,const SDL_AudioSpec*,SDL_AudioSpec*,int){trace("open");return 7;}
void fake_pause(SDL_AudioDeviceID,int){trace("unpause");}
int fake_pcm(void** pcm,const char*,int,int){*pcm=reinterpret_cast<void*>(1);trace("pcm-open");return 0;}
int fake_close(void*){trace("pcm-close");return 0;}
extern "C" void* dlsym(void*,const char* name){
 if(!std::strcmp(name,"SDL_OpenAudioDevice"))return reinterpret_cast<void*>(&fake_open);
 if(!std::strcmp(name,"SDL_PauseAudioDevice"))return reinterpret_cast<void*>(&fake_pause);
 if(!std::strcmp(name,"snd_pcm_open"))return reinterpret_cast<void*>(&fake_pcm);
 if(!std::strcmp(name,"snd_pcm_close"))return reinterpret_cast<void*>(&fake_close);
 return nullptr;
}
int main(int argc,char** argv){
 if(argc!=4)return 2;
 setenv("RGDS_VOLUME_DATA",argv[1],1);setenv("RGDS_VOLUME_HELPER",argv[2],1);
 if(!std::strcmp(argv[3],"sdl")){auto id=SDL_OpenAudioDevice(nullptr,0,nullptr,nullptr,0);SDL_PauseAudioDevice(id,0);return 0;}
 void* pcm=nullptr;return snd_pcm_open(&pcm,"default",0,0)==0?0:5;
}
''')
        includes = subprocess.check_output(["sdl2-config", "--cflags"], text=True).split()
        subprocess.run(["clang++", "-std=c++17", "-O0", *includes, "-I" + str(ROOT / "src"),
                        str(source), "-o", str(cls.binary)], check=True, capture_output=True, text=True)
        cls.helper = helper

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, kind, fail=False):
        with tempfile.TemporaryDirectory() as temp:
            data = Path(temp)
            if fail:
                (data / "fail").touch()
            result = subprocess.run([str(self.binary), str(data), str(self.helper), kind],
                                    capture_output=True, text=True, timeout=5)
            return result, (data / "trace").read_text().splitlines()

    def test_sdl_syncs_after_open_and_before_unpausing(self):
        result, trace = self.invoke("sdl")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(trace, ["open", "sync", "sync", "unpause"])

    def test_failed_sync_leaves_sdl_paused(self):
        result, trace = self.invoke("sdl", fail=True)
        self.assertNotIn("unpause", trace)
        self.assertIn("held muted", result.stderr)

    def test_direct_alsa_sink_is_synced_before_return_and_closed_on_failure(self):
        result, trace = self.invoke("alsa")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(trace, ["pcm-open", "sync"])
        result, trace = self.invoke("alsa", fail=True)
        self.assertEqual(result.returncode, 5)
        self.assertEqual(trace, ["pcm-open", "sync", "pcm-close"])


if __name__ == "__main__":
    unittest.main()
