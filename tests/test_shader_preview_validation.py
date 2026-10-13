"""Packaging checks for exact preview pixels and current shader provenance."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from validate_shader_previews import validate_shader_previews


class ShaderPreviewValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.previews=self.root/'previews';self.shaders=self.root/'shaders';self.pipeline=self.root/'game_shader.hpp'
        shutil.copytree(ROOT/'assets/shader-previews',self.previews);shutil.copytree(ROOT/'vendor/engines/shaders',self.shaders);shutil.copyfile(ROOT/'src/platform/game_shader.hpp',self.pipeline)

    def validate(self):return validate_shader_previews(self.previews,self.shaders,self.pipeline)

    def edit_manifest(self,edit):
        path=self.previews/'manifest.json';manifest=json.loads(path.read_text());edit(manifest);path.write_text(json.dumps(manifest))

    def test_valid_previews_validate_without_optional_python_packages_or_graphics(self):
        manifest=self.validate();self.assertEqual(len(manifest['scenes']),3);self.assertGreater(manifest['packed_bytes'],0)
        result=subprocess.run([sys.executable,'-I','-S',str(ROOT/'tools/validate_shader_previews.py'),str(self.previews),'--shader-folder',str(self.shaders),'--pipeline',str(self.pipeline)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('63 exact GLES previews',result.stdout)

    def test_stale_pipeline_and_shader_sources_cannot_be_packaged(self):
        self.pipeline.write_bytes(self.pipeline.read_bytes()+b'\n// changed pipeline\n')
        with self.assertRaisesRegex(ValueError,'pipeline.*source changed'):self.validate()
        shutil.copyfile(ROOT/'src/platform/game_shader.hpp',self.pipeline);path=self.shaders/'lcd1x.dsd';path.write_bytes(path.read_bytes()+b'\n')
        with self.assertRaisesRegex(ValueError,'lcd1x.dsd.*source changed'):self.validate()

    def test_tampered_packed_file_and_byte_totals_are_rejected(self):
        path=self.previews/'grove/shader-7.rgsp';data=bytearray(path.read_bytes());data[20]^=1;path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'shader-7.rgsp.*packed file hash mismatch'):self.validate()
        shutil.copyfile(ROOT/'assets/shader-previews/grove/shader-7.rgsp',path)
        self.edit_manifest(lambda value:value.__setitem__('packed_bytes',value['packed_bytes']+1))
        with self.assertRaisesRegex(ValueError,'packed byte total mismatch'):self.validate()

    def test_declared_hash_does_not_bypass_tile_or_pixel_validation(self):
        path=self.previews/'grove/shader-0.rgsp';data=bytearray(path.read_bytes());w,h,side,count=struct.unpack_from('<HHHH',data,8);at=16+count*side*side*4;struct.pack_into('<H',data,at,count);path.write_bytes(data)
        self.edit_manifest(lambda value:value['scenes'][0]['presets'][0].__setitem__('sha256',hashlib.sha256(data).hexdigest()))
        with self.assertRaisesRegex(ValueError,'tile index is outside the atlas'):self.validate()
        shutil.copyfile(ROOT/'assets/shader-previews/grove/shader-0.rgsp',path)
        self.edit_manifest(lambda value:value['scenes'][0]['presets'][0].update(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),rgba_sha256='0'*64))
        with self.assertRaisesRegex(ValueError,'decoded pixel hash mismatch'):self.validate()

    def test_missing_extra_or_wrong_named_assets_and_presets_are_rejected(self):
        (self.previews/'extra.rgsp').write_bytes(b'unexpected')
        with self.assertRaisesRegex(ValueError,'inventory.*unexpected'):self.validate()
        (self.previews/'extra.rgsp').unlink();(self.previews/'starfall/shader-14.rgsp').unlink()
        with self.assertRaisesRegex(ValueError,'starfall/shader-14.rgsp.*missing file'):self.validate()
        shutil.copyfile(ROOT/'assets/shader-previews/starfall/shader-14.rgsp',self.previews/'starfall/shader-14.rgsp')
        self.edit_manifest(lambda value:value['scenes'][1]['presets'].pop())
        with self.assertRaisesRegex(ValueError,'circuit.*every preset ID'):self.validate()

    def test_native_scene_pixels_and_exact_shader_mapping_are_validated(self):
        self.edit_manifest(lambda value:value['scenes'][0].__setitem__('native_sha256','0'*64))
        with self.assertRaisesRegex(ValueError,'grove/source.png.*native scene pixel hash mismatch'):self.validate()
        shutil.copyfile(ROOT/'assets/shader-previews/manifest.json',self.previews/'manifest.json')
        self.edit_manifest(lambda value:value['shaders'].__setitem__('off.dsd','0'*64))
        with self.assertRaisesRegex(ValueError,'exact preset file mappings'):self.validate()

    def test_unsupported_schema_duplicate_keys_and_png_corruption_are_rejected(self):
        self.edit_manifest(lambda value:value.__setitem__('schema',True))
        with self.assertRaisesRegex(ValueError,'unsupported schema'):self.validate()
        manifest=self.previews/'manifest.json';manifest.write_text('{"schema":1,"schema":1}')
        with self.assertRaisesRegex(ValueError,'duplicate JSON key'):self.validate()
        shutil.copyfile(ROOT/'assets/shader-previews/manifest.json',manifest);source=self.previews/'starfall/source.png';data=bytearray(source.read_bytes());data[45]^=1;source.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'PNG chunk checksum mismatch'):self.validate()


if __name__=='__main__':unittest.main()
