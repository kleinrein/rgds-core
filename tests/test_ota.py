"""Real signatures, hostile feeds/archives, resumed downloads and boot recovery."""
import base64
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime'))
from ota import Updater, TARGET, guard_module, https_url, strict_json
from update_crypto import verify

def release(root,build,script='pass'):
    app=root/'releases'/build;(app/'runtime').mkdir(parents=True);(app/'bin').mkdir()
    (app/'bin/rgds-launcher').write_bytes(b'\x7fELF\x02\x01'+b'\0'*12+b'\xb7\x00fixture')
    (app/'runtime/session.py').write_text(script)
    files={str(p.relative_to(app)):hashlib.sha256(p.read_bytes()).hexdigest() for p in app.rglob('*') if p.is_file()}
    (app/'manifest.json').write_text(json.dumps(dict(schema=1,project='rgds',target='aarch64-linux-gnu',build=build,files=files)))
    return app

class Response(io.BytesIO):
    def __init__(self,body,url,status=200,headers=None):super().__init__(body);self.url=url;self.status=status;self.headers=headers or {}
    def geturl(self):return self.url

# macOS ships LibreSSL as /usr/bin/openssl, which lacks Ed25519; runtime
# update_crypto.py uses Homebrew's OpenSSL 3 there, and so do these keys.
OPENSSL=next((p for p in ('/opt/homebrew/opt/openssl@3/bin/openssl','/usr/local/opt/openssl@3/bin/openssl') if sys.platform=='darwin' and Path(p).exists()),'openssl')

class OTATests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.key=Path(cls.temp.name)/'key'
        subprocess.run([OPENSSL,'genpkey','-algorithm','ED25519','-out',str(cls.key)],check=True,capture_output=True)
        cls.public=subprocess.run([OPENSSL,'pkey','-in',str(cls.key),'-pubout','-outform','DER'],check=True,capture_output=True).stdout[-32:]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def setUp(self):
        self.tempdir=tempfile.TemporaryDirectory();self.addCleanup(self.tempdir.cleanup)
        self.root=Path(self.tempdir.name)/'rgds';(self.root/'state').mkdir(parents=True)
        shutil.copy2(ROOT/'packaging/update_guard.py',self.root/'update_guard.py')
        self.guard=guard_module(self.root);self.app=release(self.root,'old');self.guard.atomic(self.root/'state/active','old\n')
        self.config=dict(schema=1,feed='https://updates.example/feed.json',keys={'test':self.public.hex()},channel='stable')
        self.guard.save(self.root/'state/update-trust.json',self.config)
        self.updater=Updater(self.app,self.root/'userdata');self.updater.setup()
        new=release(self.root,'new');self.zip=self.root/'package.zip'
        with zipfile.ZipFile(self.zip,'w') as archive:
            for p in new.rglob('*'):
                if p.is_file():archive.write(p,p.relative_to(new.parent))
        with zipfile.ZipFile(self.zip) as z:unpacked=sum(e.file_size for e in z.infolist())
        now=int(time.time());self.meta=dict(schema=1,target=TARGET,base='anbernic-buildroot-2024.02',minimum_updater=1,channel='stable',version='0.1.0',build='new',sequence=1,issued=now,expires=now+3600,url='https://updates.example/new.zip',size=self.zip.stat().st_size,unpacked_size=unpacked,sha256=hashlib.sha256(self.zip.read_bytes()).hexdigest(),notes='New release')
        shutil.rmtree(new)
    def signed(self,value=None):
        data=json.dumps(value or self.meta,sort_keys=True).encode();folder=Path(self.tempdir.name)
        (folder/'message').write_bytes(data)
        subprocess.run([OPENSSL,'pkeyutl','-sign','-rawin','-inkey',str(self.key),'-in',str(folder/'message'),'-out',str(folder/'signature')],check=True,capture_output=True)
        return json.dumps(dict(key_id='test',signed=base64.b64encode(data).decode(),signature=base64.b64encode((folder/'signature').read_bytes()).decode())).encode()
    def offer(self):self.guard.atomic(self.updater.folder/'offer.json',self.signed().decode())
    def test_automatic_checks_are_rate_limited_and_do_not_install(self):
        from fields import write_fields
        def checked():self.updater.publish('available','Update available.')
        with patch('emulators.core_running',return_value=False),patch.object(self.updater,'check',side_effect=checked) as check,patch.object(self.updater,'download') as download,patch.object(self.updater,'apply') as apply:
            self.updater.auto();self.updater.auto();self.assertEqual(check.call_count,1);download.assert_not_called();apply.assert_not_called()
            (self.root/'state/update-auto.json').unlink();write_fields(self.updater.data/'experience.ini',dict(auto_update=2));self.updater.auto();download.assert_called_once();apply.assert_not_called()
    def test_automatic_checks_skip_disabled_unconfigured_and_gameplay(self):
        from fields import write_fields
        with patch.object(self.updater,'check') as check:
            write_fields(self.updater.data/'experience.ini',dict(auto_update=0));self.updater.auto()
            write_fields(self.updater.data/'experience.ini',dict(auto_update=1))
            with patch('emulators.core_running',return_value=True):self.updater.auto()
            self.config['feed']='';self.guard.save(self.root/'state/update-trust.json',self.config);self.updater.auto();check.assert_not_called()

    def test_rfc_vector_and_tampered_feed(self):
        public=bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
        signature=bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
        self.assertTrue(verify(public,signature,b''));self.assertFalse(verify(public,signature,b'changed'))
        raw=json.loads(self.signed());raw['signed']=base64.b64encode(b'changed').decode()
        with self.assertRaisesRegex(ValueError,'signature'):self.updater.authenticate(json.dumps(raw).encode(),self.config)
    def test_expiry_target_channel_and_replay(self):
        for change in [dict(expires=int(time.time())-1),dict(target='other'),dict(channel='testing'),dict(minimum_updater=2),dict(url='http://insecure.example')]:
            with self.subTest(change=change),self.assertRaises(ValueError):self.updater.authenticate(self.signed(dict(self.meta,**change)),self.config)
        self.guard.save(self.root/'state/update-sequence.json',dict(sequence=2,digest='x'))
        with self.assertRaisesRegex(ValueError,'older'):self.updater.authenticate(self.signed(),self.config)
        with self.assertRaises(ValueError):strict_json('{"schema":1,"schema":2}')
        for url in ['http://x','file:///tmp/x','https://user:password@x','https://x/#bad']:
            with self.assertRaises(ValueError):https_url(url)
    def test_check_is_read_only_for_installed_release(self):
        self.updater.request=lambda *a,**k:Response(self.signed(),self.config['feed'])
        self.updater.check();self.assertEqual(self.guard.pointer(self.root,'active'),'old')
        self.assertEqual(self.guard.read(self.root/'state/update-sequence.json')['sequence'],1)
    def test_resumed_download_stages_without_activating_and_preserves_saves(self):
        self.offer();body=self.zip.read_bytes();partial=self.updater.folder/(self.meta['sha256']+'.part');partial.write_bytes(body[:32])
        save=self.root/'userdata/save.sav';save.write_bytes(b'progress')
        def fetch(url,headers):
            self.assertEqual(headers,{'Range':'bytes=32-'})
            return Response(body[32:],url,206,{'Content-Range':f'bytes 32-{len(body)-1}/{len(body)}'})
        self.updater.request=fetch
        with patch.object(self.updater,'power_check'):self.updater.download()
        self.assertEqual(self.guard.pointer(self.root,'active'),'old');self.assertEqual(save.read_bytes(),b'progress')
        self.assertEqual(self.guard.read(self.root/'state/update-pending.json')['phase'],'staged')
        self.guard.verify(self.root,'new')
        self.updater.discard();self.assertFalse((self.root/'state/update-pending.json').exists())
    def test_corrupt_download_low_space_and_running_game(self):
        self.offer();body=bytearray(self.zip.read_bytes());body[-1]^=1
        self.updater.request=lambda *a,**k:Response(body,self.meta['url'])
        with patch.object(self.updater,'power_check'),self.assertRaisesRegex(ValueError,'verification'):self.updater.download()
        self.assertEqual(self.guard.pointer(self.root,'active'),'old')
        with patch.object(self.updater,'power_check'),patch('ota.shutil.disk_usage',return_value=shutil._ntuple_diskusage(100,99,1)),self.assertRaisesRegex(ValueError,'space'):self.updater.download()
        with patch('emulators.core_running',return_value=True),self.assertRaisesRegex(ValueError,'Exit your game'):self.updater.stage(self.zip,self.meta)
        self.assertFalse((self.root/'state/update-pending.json').exists())
    def test_paths_and_duplicates_never_escape_staging(self):
        for name in ['new/../../escape','/tmp/escape','new/../escape','new/a\\b']:
            path=self.root/'bad.zip'
            with zipfile.ZipFile(path,'w') as z:z.writestr(name,b'bad')
            with self.assertRaises(ValueError):self.updater.stage(path,dict(self.meta,unpacked_size=3))
        self.assertFalse((self.root/'escape').exists())
    def test_simultaneous_updater_is_rejected(self):
        with self.guard.update_lock(self.root),self.assertRaisesRegex(ValueError,'already running'):
            self.updater.check()
    def candidate(self,script):
        release(self.root,'new',script)
        pending=dict(schema=1,phase='staged',build='new',previous='old',sequence=1,version='0.1.0')
        self.guard.save(self.root/'state/update-pending.json',pending);self.guard.save(self.root/'state/update-apply.json',dict(build='new'))
    def test_crash_and_hang_roll_back_and_start_previous(self):
        for script in ['raise RuntimeError("boot failure")','import time;time.sleep(10)']:
            with self.subTest(script=script):
                if (self.root/'releases/new').exists():shutil.rmtree(self.root/'releases/new')
                self.candidate(script)
                self.assertEqual(self.guard.supervise(self.root,timeout=.15,grace=.05,interval=.005),0)
                self.assertEqual(self.guard.pointer(self.root,'active'),'old')
                self.assertEqual(self.guard.read(self.root/'state/update-result.json')['state'],'rolled-back')
    def test_healthy_first_frame_commits(self):
        script='''import json,os,time
from pathlib import Path
root=Path(__file__).parents[3];p=root/'userdata/session/update-health.json';p.parent.mkdir(parents=True,exist_ok=True)
p.write_text(json.dumps(dict(build='new',token=os.environ['RGDS_UPDATE_TOKEN'])))
time.sleep(.2)
'''
        self.candidate(script)
        self.assertEqual(self.guard.supervise(self.root,timeout=.4,grace=.04,interval=.005),0)
        self.assertEqual(self.guard.pointer(self.root,'active'),'new')
        self.assertFalse((self.root/'state/update-pending.json').exists())
        self.assertEqual(self.guard.read(self.root/'state/update-result.json')['state'],'installed')
    def test_old_nonce_and_stopped_heartbeat_cannot_commit(self):
        for token in ["'old-token'", "os.environ['RGDS_UPDATE_TOKEN']"]:
            with self.subTest(token=token):
                if (self.root/'releases/new').exists():shutil.rmtree(self.root/'releases/new')
                self.candidate("import json,os,time\nfrom pathlib import Path\np=Path(__file__).parents[3]/'userdata/session/update-health.json'\np.parent.mkdir(parents=True,exist_ok=True)\np.write_text(json.dumps(dict(build='new',token="+token+",pulse=1)))\ntime.sleep(10)\n")
                self.assertEqual(self.guard.supervise(self.root,timeout=.3,grace=.2,interval=.005,heartbeat_timeout=.05),0)
                self.assertEqual(self.guard.pointer(self.root,'active'),'old')
                self.assertEqual(self.guard.read(self.root/'state/update-result.json')['state'],'rolled-back')
    def test_corrupt_staged_release_is_never_activated(self):
        self.candidate('raise AssertionError("must never execute")')
        (self.root/'releases/new/runtime/session.py').write_text('tampered')
        self.assertFalse(self.guard.begin_trial(self.root))
        self.assertEqual(self.guard.pointer(self.root,'active'),'old')
        self.assertFalse((self.root/'state/update-pending.json').exists())
    def test_interrupted_trial_restores_before_loading_candidate(self):
        self.candidate('raise AssertionError("must not execute")');self.guard.begin_trial(self.root)
        pending=self.guard.read(self.root/'state/update-pending.json');pending['attempted']=True;self.guard.save(self.root/'state/update-pending.json',pending)
        self.assertTrue(self.guard.recover_interrupted(self.root));self.assertEqual(self.guard.pointer(self.root,'active'),'old')
    def test_boot_trial_fallback_uses_normal_stock_handoff(self):
        self.candidate('raise RuntimeError("controlled cold-boot failure")');commands=[]
        def launch(command,**kwargs):
            commands.append(command)
            return subprocess.Popen(command,**kwargs)
        self.assertEqual(self.guard.supervise(self.root,session_args=('--boot','--boot-warning'),launch=launch,timeout=.2,interval=.005),0)
        self.assertIn('--boot',commands[0]);self.assertNotIn('--boot-warning',commands[0])
        self.assertNotIn('--boot',commands[1])
    def test_unconfigured_makes_no_network_request(self):
        self.guard.save(self.root/'state/update-trust.json',dict(schema=1,feed='',keys={}))
        with patch.object(self.updater,'request',side_effect=AssertionError):
            self.assertFalse(self.updater.info())
            with self.assertRaisesRegex(ValueError,'channel opens'):self.updater.check()

if __name__=='__main__':unittest.main()
