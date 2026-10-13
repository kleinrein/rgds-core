"""Signed, staged application updates. Trust/bootstrap stay outside OTA releases."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import time
import urllib.parse
import urllib.request
import zipfile
from fields import read_fields, write_fields
from update_crypto import verify as verify_signature

MAX_PACKAGE=128*1024*1024
MAX_EXPANDED=256*1024*1024
TARGET='rgds-stock-aarch64'

def guard_module(root):
    path=root/'update_guard.py'
    if not path.is_file() or path.is_symlink(): raise ValueError('Install the latest SD installer to enable updates.')
    spec=importlib.util.spec_from_file_location('_rgds_update_guard',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def https_url(url):
    if not isinstance(url,str) or len(url)>4096: raise ValueError('Invalid update address.')
    p=urllib.parse.urlsplit(url)
    if p.scheme!='https' or not p.hostname or p.username or p.password or p.fragment: raise ValueError('Updates require a secure HTTPS address.')
    return url

class SecureRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl):
        https_url(newurl)
        return super().redirect_request(req,fp,code,msg,headers,newurl)

def strict_json(raw):
    def pairs(items):
        out={}
        for key,value in items:
            if key in out:raise ValueError('Duplicate update metadata field.')
            out[key]=value
        return out
    return json.loads(raw,object_pairs_hook=pairs)

class Updater:
    def __init__(self,app,data,preview=False,opener=None):
        self.app=Path(app);self.data=Path(data);self.root=self.data.parent
        self.preview=preview;self.folder=self.data/'update';self.folder.mkdir(parents=True,exist_ok=True)
        self.transport=opener or urllib.request.build_opener(SecureRedirect())
        self.guard=None
    def publish(self,state,message,**values):
        previous=read_fields(self.folder/'status.ini')
        previous.update(state=state,message=message,**{k:str(v) for k,v in values.items()})
        write_fields(self.folder/'status.ini',previous,durable=False)
    def config(self):
        try: config=strict_json((self.root/'state/update-trust.json').read_bytes())
        except FileNotFoundError:return {}
        if not isinstance(config,dict) or config.get('schema')!=1:raise ValueError('Update configuration is invalid.')
        return config
    def info(self):
        config=self.config();configured=bool(config.get('feed') and config.get('keys')) and not self.preview
        values=dict(configured=int(configured),current=self.app.name,channel=config.get('channel','stable'))
        existing=read_fields(self.folder/'status.ini')
        if configured:
            pending=guard_module(self.root).read(self.root/'state/update-pending.json')
            values['prepared']=int(pending.get('phase')=='staged')
            if values['prepared']:
                self.publish('staged','Ready to restart. Your previous version is kept for recovery.',version=pending.get('version',''),**values)
                return True
        if not configured:self.publish('unconfigured','Updates will be available when the release channel opens.',**values)
        elif existing.get('state') in ('checking','downloading','verifying','staging','restarting','testing'):
            self.publish('idle','Check for updates when you are ready.',**values)
        else:self.publish(existing.get('state','idle'),existing.get('message','Check for updates when you are ready.'),**values)
        return configured
    def setup(self):
        config=self.config()
        if self.preview or not config.get('feed') or not config.get('keys'): raise ValueError('Updates will be available when the release channel opens.')
        https_url(config['feed'])
        self.guard=guard_module(self.root)
        return config
    def request(self,url,headers=None):
        req=urllib.request.Request(https_url(url),headers={'User-Agent':'RG-DS-Core-Updater/1',**(headers or {})})
        response=self.transport.open(req,timeout=15)
        https_url(response.geturl())
        return response
    def authenticate(self,raw,config,now=None):
        if len(raw)>256*1024:raise ValueError('Update metadata is too large.')
        envelope=strict_json(raw)
        try:
            public=bytes.fromhex(config['keys'][envelope['key_id']])
            signed=base64.b64decode(envelope['signed'],validate=True)
            signature=base64.b64decode(envelope['signature'],validate=True)
        except (KeyError,TypeError,ValueError):raise ValueError('Update signature is not trusted.') from None
        if not verify_signature(public,signature,signed):raise ValueError('Update signature is not trusted.')
        value=strict_json(signed); now=int(time.time() if now is None else now)
        if not isinstance(value,dict) or value.get('schema')!=1 or value.get('target')!=TARGET or value.get('channel')!=config.get('channel','stable'):raise ValueError('This update is not for this device or channel.')
        for key in ('sequence','issued','expires','size','unpacked_size','minimum_updater'):
            if type(value.get(key)) is not int:raise ValueError('Invalid update metadata.')
        if not 0<value['sequence']<2**53 or not value['issued']-300<=now<value['expires'] or not value['issued']<value['expires']<=value['issued']+90*86400:raise ValueError('Update information has expired or the device clock is incorrect.')
        if value['minimum_updater']>self.guard.PROTOCOL:raise ValueError('Install the latest SD installer before this update.')
        if value.get('base')!='anbernic-buildroot-2024.02':raise ValueError('This update requires a different stock firmware.')
        if not isinstance(value.get('build'),str) or not self.guard.BUILD.fullmatch(value['build']):raise ValueError('Invalid update build.')
        if not isinstance(value.get('version'),str) or not re.fullmatch(r'[0-9A-Za-z.+-]{1,80}',value['version']):raise ValueError('Invalid update version.')
        if not isinstance(value.get('notes',''),str) or len(value.get('notes',''))>4000:raise ValueError('Invalid release notes.')
        if not 0<value['size']<=MAX_PACKAGE or not 0<value['unpacked_size']<=MAX_EXPANDED or not re.fullmatch('[0-9a-f]{64}',value.get('sha256','')):raise ValueError('Invalid update package.')
        https_url(value.get('url'))
        ledger=self.guard.read(self.root/'state/update-sequence.json'); digest=hashlib.sha256(signed).hexdigest()
        if value['sequence']<ledger.get('sequence',0) or (value['sequence']==ledger.get('sequence') and ledger.get('digest')!=digest):raise ValueError('An older or conflicting update was rejected.')
        return value,digest
    def check(self):
        config=self.setup()
        with self.guard.update_lock(self.root):
            if self.guard.read(self.root/'state/update-pending.json'):raise ValueError('An update is already prepared. Install it or discard it first.')
            self.publish('checking','Checking for updates…',progress=0)
            with self.request(config['feed']) as response:raw=response.read(256*1024+1)
            value,digest=self.authenticate(raw,config)
            self.guard.save(self.root/'state/update-sequence.json',dict(sequence=value['sequence'],digest=digest))
            self.guard.atomic(self.folder/'offer.json',raw.decode('utf-8'))
            result=self.guard.read(self.root/'state/update-result.json')
            state='current' if value['build']==self.app.name else 'failed-release' if result.get('state')=='rolled-back' and result.get('build')==value['build'] else 'available'
            self.publish(state,'You are up to date.' if state=='current' else 'This version failed to start. Waiting for a newer update.' if state=='failed-release' else 'A new version is ready to download.',version=value['version'],notes=value.get('notes',''),size=value['size'],channel=value['channel'],progress=0)
            return value
    def power_check(self):
        from system_controls import Hardware
        values=Hardware(self.data).status()
        if not values.get('external_power') and (values.get('battery',-1)<20):raise ValueError('Connect the charger before installing an update.')
        panels=list(Path('/sys/class/drm').glob('card*-DSI-*/modes'))
        if len(panels)!=2 or any(p.read_text().splitlines()!=['640x480'] for p in panels):raise ValueError('This stock firmware has not been validated for updates.')
        release=Path('/etc/os-release').read_text()
        if 'ID=buildroot' not in release or 'VERSION_ID=2024.02' not in release:raise ValueError('This stock firmware has not been validated for updates.')
    def download(self):
        config=self.setup()
        with self.guard.update_lock(self.root):
            if self.guard.read(self.root/'state/update-pending.json'):raise ValueError('An update is already prepared.')
            value,_=self.authenticate((self.folder/'offer.json').read_bytes(),config)
            if value['build']==self.app.name:raise ValueError('You are up to date.')
            result=self.guard.read(self.root/'state/update-result.json')
            if result.get('state')=='rolled-back' and result.get('build')==value['build']:raise ValueError('This version failed to start. Waiting for a newer update.')
            self.power_check()
            required=value['size']+value['unpacked_size']+16*1024*1024
            if shutil.disk_usage(self.root).free<required:raise ValueError('There is not enough free space for this update.')
            archive=self.folder/(value['sha256']+'.part');offset=archive.stat().st_size if archive.exists() else 0
            if offset>value['size']:archive.unlink();offset=0
            self.publish('downloading','Downloading update…',progress=0,version=value['version'])
            if offset<value['size']:
                with self.request(value['url'],{'Range':f'bytes={offset}-'} if offset else {}) as response:
                    if response.status==200:offset=0
                    elif response.status!=206 or response.headers.get('Content-Range')!=f'bytes {offset}-{value["size"]-1}/{value["size"]}':raise ValueError('The download could not be resumed. Please retry.')
                    received=offset;last=0;deadline=time.monotonic()+1800
                    with archive.open('ab' if offset else 'wb') as output:
                        while True:
                            block=response.read(128*1024)
                            if not block:break
                            received+=len(block)
                            if received>value['size'] or time.monotonic()>deadline:raise ValueError('Update download exceeded its limits.')
                            output.write(block)
                            if time.monotonic()-last>.2:
                                self.publish('downloading','Downloading update…',progress=received*100//value['size'],received=received);last=time.monotonic()
                        output.flush();os.fsync(output.fileno())
            if archive.stat().st_size!=value['size']:raise ValueError('Download interrupted. Retry to continue.')
            self.publish('verifying','Verifying update…',progress=100)
            h=hashlib.sha256()
            with archive.open('rb') as stream:
                for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
            if h.hexdigest()!=value['sha256']:
                archive.unlink();raise ValueError('The downloaded update failed verification. Please retry.')
            self.stage(archive,value)
            archive.unlink()
            self.publish('staged','Ready to restart. Your previous version is kept for recovery.',progress=100,version=value['version'],prepared=1)
    def stage(self,archive,value):
        from activate import verify_release
        from emulators import activity_lock,core_running
        stage=self.root/'inbox'/('ota-'+value['build']);app=stage/value['build']
        if stage.exists():shutil.rmtree(stage)
        self.publish('staging','Preparing update…',progress=0)
        try:
            with zipfile.ZipFile(archive) as z:
                entries=z.infolist();names=[e.filename for e in entries]
                if len(names)!=len(set(names)) or len(entries)>10000 or sum(e.file_size for e in entries)!=value['unpacked_size']:raise ValueError('Update archive inventory is invalid.')
                for entry in entries:
                    p=Path(entry.filename)
                    if p.is_absolute() or '..' in p.parts or '\\' in entry.filename or p.as_posix()!=entry.filename or len(p.parts)<2 or p.parts[0]!=value['build'] or entry.is_dir() or stat.S_ISLNK(entry.external_attr>>16):raise ValueError('Update contains an unsafe path.')
                for i,entry in enumerate(entries):
                    destination=stage/entry.filename;destination.parent.mkdir(parents=True,exist_ok=True)
                    with z.open(entry) as source,destination.open('wb') as output:
                        shutil.copyfileobj(source,output,1024*1024);output.flush();os.fsync(output.fileno())
                    destination.chmod(0o755 if (entry.external_attr>>16)&0o111 else 0o644)
                    if i%20==0:self.publish('staging','Preparing update…',progress=(i+1)*100//len(entries))
            verify_release(app)
            destination=self.root/'releases'/value['build']
            with activity_lock(self.data):
                if core_running(self.data):raise ValueError('Exit your game before installing an update.')
                current=self.guard.pointer(self.root,'active')
                if current!=self.app.name:raise ValueError('The installed version changed. Check for updates again.')
                self.guard.verify(self.root,current)
                if destination.exists():verify_release(destination)
                else:app.rename(destination)
                self.guard.save(self.root/'state/update-pending.json',dict(schema=1,phase='staged',build=value['build'],previous=current,sequence=value['sequence'],version=value['version']))
        finally:
            if stage.exists():shutil.rmtree(stage)
    def apply(self):
        self.setup()
        from emulators import activity_lock,core_running
        with self.guard.update_lock(self.root),activity_lock(self.data):
            if core_running(self.data):raise ValueError('Exit your game before installing an update.')
            self.power_check()
            pending=self.guard.read(self.root/'state/update-pending.json')
            if pending.get('phase')!='staged' or pending.get('previous')!=self.guard.pointer(self.root,'active'):raise ValueError('Check for updates again.')
            # A legacy unsupervised launch must never quit into an unhandled apply request.
            import fcntl
            with (self.root/'state/update-supervisor.lock').open('a') as lock:
                try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
                except BlockingIOError:pass
                else:raise ValueError('Restart RG DS Core before installing the update.')
            self.guard.save(self.root/'state/update-apply.json',dict(build=pending['build']))
            self.publish('restarting','Restarting into the update…',progress=100)
    def discard(self):
        self.setup()
        with self.guard.update_lock(self.root):
            pending=self.guard.read(self.root/'state/update-pending.json')
            if pending.get('phase')=='trial':raise ValueError('The new version is being checked.')
            (self.root/'state/update-pending.json').unlink(missing_ok=True)
            (self.root/'state/update-apply.json').unlink(missing_ok=True)
            for path in self.folder.glob('*.part'):path.unlink()
            self.publish('idle','Update discarded. Your installed version is unchanged.',progress=0,prepared=0)
    def auto(self):
        """Quiet, rate-limited background checks; installation remains explicit."""
        from emulators import core_running
        mode=read_fields(self.data/'experience.ini').get('auto_update','1')
        if mode=='0' or self.preview or not self.config().get('feed') or core_running(self.data):return
        self.setup()
        if self.guard.read(self.root/'state/update-pending.json'):return
        now=int(time.time());record=self.guard.read(self.root/'state/update-auto.json')
        if 0<=now-record.get('checked',0)<6*3600:return
        self.guard.save(self.root/'state/update-auto.json',dict(checked=now))
        self.check()
        if mode=='2' and read_fields(self.folder/'status.ini').get('state')=='available':self.download()

    def run(self,action):
        try:return getattr(self,action)()
        except KeyboardInterrupt:
            self.publish('cancelled','Download paused. Retry to continue.');raise
        except Exception as error:
            actionable={
                'Updates will be available when the release channel opens.',
                'Connect the charger before installing an update.',
                'There is not enough free space for this update.',
                'Exit your game before installing an update.',
                'Install the latest SD installer before this update.',
                'Restart RG DS Core before installing the update.',
                'Download interrupted. Retry to continue.',
                'This version failed to start. Waiting for a newer update.',
                'An update is already prepared. Install it or discard it first.',
                'Another update is already running.',
                'Update information has expired or the device clock is incorrect.',
                'This stock firmware has not been validated for updates.'}
            message=str(error) if str(error) in actionable else 'Update verification failed. Check for updates again.' if isinstance(error,ValueError) else 'Update failed. Check Wi-Fi and try again.'
            self.publish('error',message)
            raise
