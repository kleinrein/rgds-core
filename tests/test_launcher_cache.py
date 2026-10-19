"""Bounded launcher caches and redraw I/O, using host files and software SDL only."""
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(shutil.which("clang++") and shutil.which("sdl2-config"), "Requires host C++ and SDL headers")
class LauncherCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        source = cls.root / "cache.cpp"
        source.write_text(r'''#include "launcher/library_cache.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
using namespace rgds;
static int decode_calls=0;
void write(fs::path path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream(path)<<value;}
void bitmap(Sdl& s,const fs::path& path,unsigned char red,unsigned char green,int size=16){
 auto* image=s.CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);assert(image);
 for(int y=0;y<size;y++)for(int x=0;x<size;x++){auto* p=static_cast<unsigned char*>(image->pixels)+y*image->pitch+x*4;p[0]=red;p[1]=green;p[2]=0;p[3]=255;}
 auto pending=path.string()+".pending";auto* stream=s.RWFromFile(pending.c_str(),"wb");assert(stream);assert(s.SaveBMP_RW(image,stream,1)==0);s.FreeSurface(image);fs::rename(pending,path);
}
void native(const fs::path& path,unsigned colour,const std::string& title){
 std::vector<unsigned char> bytes(0x640);bytes[0x69]=2;std::fill(bytes.begin()+0x220,bytes.begin()+0x420,0x11);bytes[0x422]=colour&255;bytes[0x423]=colour>>8;
 for(size_t i=0;i<title.size() && i<100;i++)bytes[0x540+i*2]=title[i];auto pending=path.string()+".pending";{std::ofstream file(pending,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}fs::rename(pending,path);
}
int main(int argc,char** argv){
 assert(argc==3);fs::path root=argv[1];std::string mode=argv[2];Game game{"Fixture",root/"fixture.nds",false};LibraryCache cache(root,8);auto folder=cache.folder(game);
 if(mode=="metadata"){
  write(folder/"info.ini","title=Before\nyear=2026\n");write(folder/"banner.ini","title=Native title\n");write(folder/"stats.ini","seconds=125\nlast_played=2026-10-07\n");
  assert(cache.metadata(game).info["title"]=="Before");assert(cache.stats(game).seconds==125);
  auto old_stamp=fs::last_write_time(folder/"info.ini");write(folder/"replacement","title=After!\nyear=2026\n");fs::last_write_time(folder/"replacement",old_stamp);fs::rename(folder/"replacement",folder/"info.ini");
  assert(cache.metadata(game).info["title"]=="Before");assert(cache.refresh(game));assert(cache.metadata(game).info["title"]=="After!");
  fs::remove(folder/"info.ini");fs::remove(folder/"stats.ini");assert(cache.refresh(game));assert(cache.metadata(game).info.empty());assert(cache.stats(game).seconds==0);assert(cache.stats(game).last_played.empty());
  write(folder/"info.ini","title=Created later\n");assert(cache.refresh(game));assert(cache.metadata(game).info["title"]=="Created later");
  write(folder/"stats.ini","seconds=not-a-number\n");assert(cache.refresh(game));assert(cache.stats(game).seconds==0);
  cache.invalidate_metadata();assert(cache.metadata(game).banner["title"]=="Native title");
 }else if(mode=="options"){
  assert(!cache.options(game).favourite);GameOptions value;value.favourite=true;value.filter=14;save_game_options(folder/"game.ini",value);cache.saved_options(game,value);
  auto reads=cache.counters.file_reads;assert(cache.options(game).favourite && cache.options(game).filter==14);assert(cache.counters.file_reads==reads);
  value.favourite=false;save_game_options(folder/"game.ini",value);assert(cache.options(game,true).favourite==false);
  fs::remove(folder/"game.ini");assert(cache.options(game,true).filter==-1);assert(!cache.options(game).favourite);
 }else if(mode=="bounded"){
  for(int i=0;i<2000;i++){Game item{"Game "+std::to_string(i),root/(std::to_string(i)+".nds"),false};cache.metadata(item);cache.stats(item);cache.options(item);assert(cache.size()<=8);}
  assert(cache.counters.evictions>=1992);cache.clear();assert(cache.size()==0);
 }else if(mode=="hot"){
  write(folder/"info.ini","title=Measured title\nyear=2026\n");write(folder/"banner.ini","title=Measured banner\n");write(folder/"stats.ini","seconds=125\nlast_played=2026-10-07\n");cache.metadata(game);cache.stats(game);
  auto reads=cache.counters.file_reads,checks=cache.counters.stamp_checks;using Clock=std::chrono::steady_clock;constexpr int count=5000;
  auto began=Clock::now();size_t total=0;
  for(int i=0;i<count;i++){auto info=fields(folder/"info.ini");auto banner=fields(folder/"banner.ini");auto stats=fields(folder/"stats.ini");total+=info["title"].size()+banner["title"].size()+std::stoull(stats["seconds"]);}
  auto uncached=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count();began=Clock::now();
  for(int i=0;i<count;i++)total+=cache.metadata(game).info["title"].size()+cache.metadata(game).banner["title"].size()+cache.stats(game).seconds;
  auto cached=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count();assert(total>0);assert(cache.counters.file_reads==reads && cache.counters.stamp_checks==checks);
  std::cout<<"{\"iterations\":"<<count<<",\"uncached_reads\":"<<count*3<<",\"warm_cache_reads\":0,\"warm_cache_stamp_checks\":0,\"uncached_us\":"<<uncached<<",\"cached_us\":"<<cached<<"}\n";
 }else if(mode=="bmp-budget"){
  Sdl s;assert(s.Init(SDL_INIT_VIDEO)==0);auto* canvas=s.CreateRGBSurfaceWithFormat(0,32,32,32,SDL_PIXELFORMAT_RGBA32);assert(canvas);auto* renderer=s.CreateSoftwareRenderer(canvas);assert(renderer);
  // The decode hook must never see a resource-exhausting header. Valid files
  // still reach the decoder; existing media tests check their rendered pixels.
  s.LoadBMP_RW=[](SDL_RWops* file,int close)->SDL_Surface*{++decode_calls;if(close)file->close(file);return nullptr;};
  {
   Media media(s);int serial=0;
   auto header=[&](uint32_t width,uint32_t height,uint32_t dib=40,unsigned depth=32){
    std::array<unsigned char,54> bytes{};bytes[0]='B';bytes[1]='M';
    auto put=[&](int n,uint32_t v){for(int i=0;i<4;i++)bytes[n+i]=(v>>(i*8))&255;};
    put(14,dib);put(18,width);put(22,height);bytes[26]=1;bytes[28]=depth;
    auto path=root/("header"+std::to_string(serial++)+".bmp");std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<char*>(bytes.data()),bytes.size());file.close();return path;
   };
   for(auto path:{header(65535,65535),header(4096,4096),header(0,480),header(640,0),header(0xffffffff,1),header(1,0x80000000),header(640,480,0x7fffffff),header(640,480,40,0)})assert(!media.warm(renderer,path));
   auto truncated=root/"truncated.bmp";write(truncated,"BM");assert(!media.warm(renderer,truncated));
   auto huge=header(640,480);fs::resize_file(huge,33*1024*1024);assert(!media.warm(renderer,huge));
   assert(decode_calls==0 && media.bytes()==0);
   for(auto path:{header(640,480),header(640,uint32_t(-480)),header(256,192,108,8)})media.warm(renderer,path);
   assert(decode_calls==3);
   auto valid=root/"valid.bmp";bitmap(s,valid,10,20);media.warm(renderer,valid);assert(decode_calls==4);
  }s.DestroyRenderer(renderer);s.FreeSurface(canvas);s.Quit();
 }else if(mode=="media"){
  Sdl s;assert(s.Init(SDL_INIT_VIDEO)==0);auto* surface=s.CreateRGBSurfaceWithFormat(0,32,32,32,SDL_PIXELFORMAT_RGBA32);assert(surface);auto* renderer=s.CreateSoftwareRenderer(surface);assert(renderer);
  {Media media(s);auto path=root/"later.bmp";assert(!media.warm(renderer,path));bitmap(s,path,255,0);assert(!media.warm(renderer,path));assert(media.refresh());assert(media.warm(renderer,path));
   auto reads=media.counters.file_reads,checks=media.counters.stamp_checks;
   for(int i=0;i<1000;i++){assert(media.warm(renderer,path));assert(media.draw(renderer,path,{0,0,16,16}));}assert(media.counters.file_reads==reads && media.counters.stamp_checks==checks);
   std::array<unsigned char,32*32*4> pixels{};assert(s.RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),32*4)==0);assert(pixels[0]==255 && pixels[1]==0);
   auto old_stamp=fs::last_write_time(path);bitmap(s,path,0,255);fs::last_write_time(path,old_stamp);assert(media.refresh());assert(media.warm(renderer,path));assert(media.draw(renderer,path,{0,0,16,16}));assert(s.RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),32*4)==0);assert(pixels[0]==0 && pixels[1]==255);
   fs::remove(path);assert(media.refresh());assert(!media.warm(renderer,path));assert(!media.draw(renderer,path,{0,0,16,16}));
   auto rom=root/"native.nds";native(rom,31,"Before");assert(media.banner(rom).title=="Before");assert(media.warm(renderer,rom,true));auto rom_stamp=fs::last_write_time(rom);
   native(rom,31<<5,"After!");fs::last_write_time(rom,rom_stamp);assert(media.banner(rom).title=="Before");assert(media.refresh());assert(media.banner(rom).title=="After!");assert(media.banner(rom).pixels[0]==0 && media.banner(rom).pixels[1]==255);assert(media.warm(renderer,rom,true));
   for(int i=0;i<150;i++){auto file=root/(std::to_string(i)+".bmp");bitmap(s,file,20,30);assert(media.warm(renderer,file));media.banner(root/(std::to_string(i)+".nds"));assert(media.image_count()<=192 && media.banner_count()<=256 && media.bytes()<=16*1024*1024);}
   for(int i=0;i<6;i++){auto file=root/("large"+std::to_string(i)+".bmp");bitmap(s,file,20,30,1024);assert(media.warm(renderer,file));assert(media.bytes()<=16*1024*1024);}
   media.clear();assert(media.image_count()==0 && media.banner_count()==0 && media.bytes()==0);
  }s.DestroyRenderer(renderer);s.FreeSurface(surface);s.Quit();
 }else return 2;
}
''')
        cls.binary = cls.root / "cache"
        flags = [flag for flag in shlex.split(subprocess.check_output(["sdl2-config", "--cflags"], text=True)) if flag.startswith("-I")]
        command = ["clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", *flags,
                   "-I" + str(ROOT / "src"), str(source), "-o", str(cls.binary)]
        if sys.platform != "darwin":
            command.append("-ldl")
        subprocess.run(command, check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, mode):
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run([str(self.binary), folder, mode], capture_output=True, text=True,
                                    env=dict(os.environ, SDL_VIDEODRIVER="dummy"), timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_metadata_atomic_replacement_deletion_and_creation_refresh(self):
        self.invoke("metadata")

    def test_saved_and_external_options_refresh_without_reparsing_hot_values(self):
        self.invoke("options")

    def test_large_collection_has_a_bounded_detail_cache(self):
        self.invoke("bounded")

    def test_warm_metadata_lookups_perform_no_file_reads_or_stats(self):
        report = json.loads(self.invoke("hot"))
        self.assertEqual(report["uncached_reads"], 15000)
        self.assertEqual(report["warm_cache_reads"], 0)
        self.assertEqual(report["warm_cache_stamp_checks"], 0)

    def test_media_refreshes_negative_entries_replacement_deletion_and_bounds(self):
        self.invoke("media")

    def test_oversized_and_malformed_artwork_is_rejected_before_sdl_allocates(self):
        self.invoke("bmp-budget")


@unittest.skipUnless(BINARY.exists(), "Build host launcher first")
class RedrawCacheTests(unittest.TestCase):
    @staticmethod
    def fixture(root):
        roms = root / "roms";roms.mkdir()
        rom = roms / "fixture.nds"
        content = bytearray(0x640);struct.pack_into("<I", content, 0x68, 0x200)
        content[0x220:0x420] = bytes([0x11]) * 512;struct.pack_into("<H", content, 0x422, 31)
        rom.write_bytes(content)
        key = 14695981039346656037
        for byte in str(rom).encode():
            key = ((key ^ byte) * 1099511628211) & 0xffffffffffffffff
        item = root / "data/library" / f"{key:016x}"
        item.mkdir(parents=True)
        (item / "info.ini").write_text("title=Cached fixture\nyear=2026\n")
        (item / "stats.ini").write_text("seconds=125\nlast_played=2026-10-07\n")
        return roms, item

    def test_real_rom_redraws_do_not_read_or_stat_library_assets(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            roms, _ = self.fixture(root)
            report = root / "report.json"
            result = subprocess.run([str(BINARY), "--preview", "--library", "--data", str(root / "data"),
                                     "--rom-dir", str(roms), "--frames", "40", "--cache-report", str(report)],
                                    cwd=ROOT, capture_output=True, text=True,
                                    env=dict(os.environ, SDL_VIDEODRIVER="dummy"), timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            counts = json.loads(report.read_text())
            self.assertEqual(counts["frames"], 40)
            self.assertEqual(counts["render_file_reads"], 0)
            self.assertEqual(counts["render_stamp_checks"], 0)
            self.assertGreater(counts["metadata_hits"], 100)
            self.assertLess(counts["metadata_reads"], 12)
            self.assertLessEqual(counts["metadata_entries"], 128)
            self.assertLessEqual(counts["texture_bytes"], 16 * 1024 * 1024)

    def test_recents_reads_stats_once_per_game_before_sorting_a_large_collection(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            roms = root / "roms";roms.mkdir()
            for index in range(200):
                rom = roms / f"game{index:04}.nds";rom.write_bytes(b"fixture")
                key = 14695981039346656037
                for byte in str(rom).encode():
                    key = ((key ^ byte) * 1099511628211) & 0xffffffffffffffff
                item = root / "data/library" / f"{key:016x}";item.mkdir(parents=True)
                (item / "stats.ini").write_text(f"seconds=125\nlast_played=2026-10-07T00:{index // 60:02}:{index % 60:02}\n")
            preferences = root / "preferences.ini"
            preferences.write_text("schema=1\nmode=light\nfont=pixel\ncore=0\nview=2\n")
            report = root / "report.json"
            result = subprocess.run([str(BINARY), "--preview", "--library", "--data", str(root / "data"),
                "--preferences", str(preferences), "--rom-dir", str(roms), "--frames", "3", "--cache-report", str(report)],
                cwd=ROOT, capture_output=True, text=True, env=dict(os.environ, SDL_VIDEODRIVER="dummy"), timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            counts = json.loads(report.read_text())
            self.assertGreaterEqual(counts["metadata_reads"], 200)
            self.assertLessEqual(counts["metadata_reads"], 202)
            self.assertEqual(counts["render_file_reads"], 0)
            self.assertLessEqual(counts["metadata_entries"], 128)
            self.assertGreaterEqual(counts["evictions"], 72)

    def test_running_launcher_refreshes_created_replaced_and_deleted_artwork(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            roms, item = self.fixture(root)
            screenshot = root / "screen.bmp"
            child = subprocess.Popen([str(BINARY), "--preview", "--library", "--data", str(root / "data"),
                                      "--rom-dir", str(roms), "--seconds", "8", "--screenshot", str(screenshot)],
                                     cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     env=dict(os.environ, SDL_VIDEODRIVER="dummy"))
            def wait_color(expected):
                end = time.monotonic() + 2.5
                while time.monotonic() < end:
                    if child.poll() is not None:
                        self.fail(child.communicate()[0])
                    if screenshot.exists():
                        data = screenshot.read_bytes();offset = struct.unpack_from("<I", data, 10)[0]
                        width, height = struct.unpack_from("<ii", data, 18)
                        # Centre of the top screen: the cover, or the DS icon without one.
                        row = height - 1 - 240 if height > 0 else 240
                        pixel = data[offset + (row * width + 320) * 4:offset + (row * width + 320) * 4 + 3]
                        if pixel == expected:
                            return
                    time.sleep(0.03)
                self.fail("Updated artwork was not rendered: " + repr(expected))
            def write_box(bgr):
                pixels = bgr * (16 * 16)
                header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
                header += struct.pack("<IiiHHIIiiII", 40, 16, 16, 1, 24, 0, len(pixels), 0, 0, 0, 0)
                pending = item / "box.pending.bmp";pending.write_bytes(header + pixels);pending.replace(item / "box.bmp")
            try:
                wait_color(b"\x00\x00\xff")
                write_box(b"\x00\xff\x00");wait_color(b"\x00\xff\x00")
                write_box(b"\xff\x00\x00");wait_color(b"\xff\x00\x00")
                (item / "box.bmp").unlink();wait_color(b"\x00\x00\xff")
            finally:
                if child.poll() is None:
                    child.terminate()
                output, _ = child.communicate(timeout=3)
                self.assertEqual(child.returncode, 0, output)


if __name__ == "__main__":
    unittest.main()
