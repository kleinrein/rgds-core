#!/usr/bin/env python3
"""Build an immutable OTA archive and Ed25519-signed feed; never uploads anything."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import zipfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime'))
from ota import TARGET, https_url

def build(args):
    https_url(args.url)
    subprocess.run([sys.executable,str(ROOT/'packaging/install_package.py'),str(ROOT/'build/install-check'),str(args.package),'--check'],check=True,capture_output=True)

    if args.sequence<1:raise ValueError('Sequence must increase for every published update.')
    with zipfile.ZipFile(args.package) as source:
        version=source.read('rgds/state/active').decode().strip()
        prefix='rgds/releases/'+version+'/'
        selected=[entry for entry in source.infolist() if entry.filename.startswith(prefix)]
        if not selected:raise ValueError('No application release in package.')
        output=args.output or args.package.parent/('RG-DS-Core-'+version+'.ota.zip')
        with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
            for entry in selected:
                info=zipfile.ZipInfo(version+'/'+entry.filename[len(prefix):]);info.external_attr=entry.external_attr;info.compress_type=zipfile.ZIP_DEFLATED
                archive.writestr(info,source.read(entry))
    with zipfile.ZipFile(output) as archive:unpacked=sum(e.file_size for e in archive.infolist())
    now=int(time.time())
    metadata=dict(schema=1,target=TARGET,base='anbernic-buildroot-2024.02',minimum_updater=1,channel=args.channel,
                  version=args.version,build=version,sequence=args.sequence,issued=now,expires=now+args.days*86400,
                  url=args.url,size=output.stat().st_size,unpacked_size=unpacked,sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                  notes=args.notes.read_text() if args.notes else '')
    signed=json.dumps(metadata,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()
    with tempfile.TemporaryDirectory() as folder:
        folder=Path(folder);(folder/'metadata').write_bytes(signed)
        subprocess.run(['openssl','pkeyutl','-sign','-rawin','-inkey',str(args.key),'-in',str(folder/'metadata'),'-out',str(folder/'signature')],check=True,capture_output=True)
        signature=(folder/'signature').read_bytes()
        public=subprocess.run(['openssl','pkey','-in',str(args.key),'-pubout','-outform','DER'],check=True,capture_output=True).stdout
    if len(signature)!=64 or public[:12]!=bytes.fromhex('302a300506032b6570032100') or len(public)!=44:raise ValueError('Use an Ed25519 signing key.')
    envelope=dict(key_id=args.key_id,signed=base64.b64encode(signed).decode(),signature=base64.b64encode(signature).decode())
    args.feed.write_text(json.dumps(envelope,indent=2)+'\n')
    print(json.dumps(dict(archive=str(output),feed=str(args.feed),key_id=args.key_id,public_key=public[-32:].hex(),sha256=metadata['sha256']),indent=2))
    return metadata

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True);p.add_argument('--output',type=Path);p.add_argument('--url',required=True)
    p.add_argument('--key',type=Path,required=True);p.add_argument('--key-id',required=True);p.add_argument('--sequence',type=int,required=True)
    p.add_argument('--version',required=True);p.add_argument('--channel',choices=('stable','testing'),default='stable')
    p.add_argument('--days',type=int,choices=range(1,91),default=14);p.add_argument('--notes',type=Path);p.add_argument('--feed',type=Path,required=True)
    build(p.parse_args())
