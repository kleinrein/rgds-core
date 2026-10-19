"""Early splash ownership, fixed ABI, bounded decoding and cleanup contracts."""
import ctypes
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime'))
import boot_splash as splash
import boot_splash_install as installer


class EarlySplashTests(unittest.TestCase):
    def test_linux_uapi_layouts(self):
        self.assertEqual([ctypes.sizeof(t) for t in (splash.Mode,splash.Resources,splash.Connector,splash.Encoder,splash.Crtc,splash.Dumb,splash.Mapping,splash.Framebuffer)],
                         [68,64,80,20,104,32,16,28])

    def test_shipped_animation_contains_real_motion(self):
        frames,fps=splash.load_frames(ROOT/'assets/boot/animation.rga')
        self.assertEqual(fps,15)
        self.assertEqual(len(frames),36)
        self.assertNotEqual(bytes(frames[0]),bytes(frames[9]))
        area=lambda frame,x,y,w,h:b''.join(bytes(frame[(row*640+x)*4:(row*640+x+w)*4]) for row in range(y,y+h))
        # The loader dots carry a wave and the floor shadow breathes, while
        # the wordmark stays still (the launcher's boot scene continues it).
        self.assertNotEqual(area(frames[0],280,363,96,11),area(frames[9],280,363,96,11))
        self.assertNotEqual(area(frames[0],270,236,100,10),area(frames[18],270,236,100,10))
        self.assertEqual(area(frames[0],176,268,289,48),area(frames[9],176,268,289,48))

    def test_corrupt_trailing_and_oversized_payloads_reject(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bad.rga'
            raw=(ROOT/'assets/boot/animation.rga').read_bytes()
            for data in (raw[:-20],raw+b'trailer',b'RGA1'+struct.pack('<4I',640,480,999,18),
                         struct.pack('<4s4I',b'RGA1',640,480,2,18)+zlib.compress(bytes(640*480*4*2+1))):
                path.write_bytes(data)
                with self.assertRaises(ValueError):splash.load_frames(path)

    def test_stock_disabled_and_recovery_modes_skip(self):
        with tempfile.TemporaryDirectory() as directory:
            control=Path(directory);root=control/'root';(root/'userdata').mkdir(parents=True)
            (control/'config.json').write_text(json.dumps(dict(schema=1,mode='rgds',root=str(root))))
            self.assertFalse(splash.enabled(control))
            (control/'enabled').touch();self.assertTrue(splash.enabled(control))
            (root/'userdata/boot_stock').touch();self.assertFalse(splash.enabled(control))
            (root/'userdata/boot_stock').unlink();(control/'installing').touch();self.assertFalse(splash.enabled(control))

    def test_running_compositor_is_never_interrupted(self):
        class Args:
            control=Path('/unused')
        with patch.object(splash,'enabled',return_value=True),patch.object(splash,'weston_running',return_value=True),patch.object(splash.os,'open') as opening:
            self.assertEqual(splash.run(Args()),0)
            opening.assert_not_called()

    def test_handoff_keeps_buffer_alive_until_both_crtcs_replace_it(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            args=SimpleNamespace(control=root,run_dir=root,assets=root,capture=False)
            saved=[(177,splash.Crtc(crtc_id=130,fb_id=10)),(179,splash.Crtc(crtc_id=90,fb_id=11))]
            operations=[]
            real_open=splash.os.open
            class Buffer(bytearray):
                def close(self):operations.append('unmap')
            buffer=Buffer(640*480*4)
            crtc_reads=0
            def ioctl(fd,number,value):
                nonlocal crtc_reads
                operations.append(number)
                if number==0xB2:value.pitch=2560;value.size=len(buffer);value.handle=1
                if number==0xAE:value.fb_id=123
                if number==0xA2 and operations.count(0xA2)==2:(root/'rgds-splash.stop').touch()
                if number==0xA1:
                    crtc_reads+=1
                    # One panel changes first; retain memory until BOTH switch.
                    value.fb_id=456 if crtc_reads>2 or value.crtc_id==130 else 123
                return value
            def kernel_ioctl(fd,request,*_):operations.append('drop-master')
            with patch.object(splash,'enabled',return_value=True),patch.object(splash,'weston_running',return_value=False), \
                 patch.object(splash,'load_frames',return_value=([bytes(buffer),bytes(buffer)],18)), \
                 patch.object(splash,'displays',return_value=saved),patch.object(splash,'ioctl',side_effect=ioctl), \
                 patch.object(splash.os,'open',side_effect=lambda path,*a,**kw:123 if str(path)=='/dev/dri/card0' else real_open(path,*a,**kw)),patch.object(splash.os,'close'), \
                 patch.object(splash.mmap,'mmap',return_value=buffer),patch.object(splash.fcntl,'ioctl',side_effect=kernel_ioctl), \
                 patch.object(splash,'uptime',return_value=6.0),patch.object(splash.time,'sleep'):
                splash.run(args)
            state=json.loads((root/'rgds-splash.json').read_text())
            self.assertEqual(state['reason'],'replaced')
            self.assertEqual(state['frames'],1)
            self.assertEqual(crtc_reads,4)
            self.assertEqual(operations.count(0xA2),2,'No CRTC restores after master handoff')
            self.assertTrue((root/'rgds-splash.released').exists())
            self.assertGreater(operations.index('unmap'),operations.index(0xA1))


class SplashInstallTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.root=Path(temp.name);self.weston=self.root/'etc/init.d/S49weston'
        self.weston.parent.mkdir(parents=True);self.weston.write_bytes(b'original vendor compositor\n')

    def install(self,**kwargs):
        return installer.install(ROOT/'runtime/boot_splash.py',ROOT/'assets/boot',self.root,**kwargs)

    def test_install_update_and_remove_leave_vendor_bytes_alone(self):
        before=self.weston.read_bytes()
        self.install(capture=True)
        start=installer.location(self.root,installer.START)
        self.assertIn('--capture',start.read_text())
        self.install()
        self.assertNotIn('--capture',start.read_text())
        self.assertEqual(start.stat().st_mode&0o777,0o755)
        self.install(remove=True)
        self.assertFalse(start.exists())
        self.assertEqual(self.weston.read_bytes(),before)

    def test_foreign_files_and_symlinks_are_preserved(self):
        start=installer.location(self.root,installer.START)
        start.write_text('foreign service')
        with self.assertRaisesRegex(ValueError,'not owned'):self.install()
        self.assertEqual(start.read_text(),'foreign service')
        start.unlink();start.symlink_to(self.weston)
        with self.assertRaisesRegex(ValueError,'symlink'):self.install()

    def test_partial_install_rolls_back(self):
        atomic=installer.atomic
        def failing(path,*args):
            if path.name=='S12rgds-splash':raise OSError('fixture write failure')
            return atomic(path,*args)
        with patch.object(installer,'atomic',side_effect=failing):
            with self.assertRaises(OSError):self.install()
        for path in (installer.START,installer.STOP,installer.STORE/'boot_splash.py',installer.STORE/'animation.rga'):
            self.assertFalse(installer.location(self.root,path).exists())
        self.assertEqual(self.weston.read_bytes(),b'original vendor compositor\n')

if __name__=='__main__':unittest.main()
