#!/usr/bin/env python3
"""Read-only SSH capture of the owner's DraStic application, not OS or user data."""
import argparse
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from device import target
from provision_drastic import OPTIONAL, REQUIRED, provision

ROOT=Path(__file__).resolve().parents[1]
REMOTE=r'''
import io,os,stat,struct,sys,zipfile
from pathlib import Path
roots=[Path('/mnt/vendor/deep/drastic_aarch64'),Path('/mnt/vendor/deep/drastic64')]
binary=None
for root in roots:
    for name in ('drastic','drastic_oga'):
        p=root/name
        if not p.is_file():continue
        with p.open('rb') as f:h=f.read(20)
        if h[:6]==b'\x7fELF\x02\x01' and len(h)==20 and struct.unpack_from('<H',h,18)[0]==183:
            binary=p;break
    if binary:break
if binary is None:raise SystemExit('No owned AArch64 DraStic executable found in the audited application directories.')
adapter=next((p for p in (binary.parent/'libs/libdrastouch.so',Path('/usr/lib/libdrastouch.so')) if p.is_file()),None)
if adapter is None:raise SystemExit('DraStic display/touch adapter is missing.')
files={'drastic':binary,'libs/libdrastouch.so':adapter}
for name in ('game_database.xml','usrcheat.dat','drastic_logo_0.raw','drastic_logo_1.raw'):
    p=binary.parent/name
    if p.is_file():files[name]=p
buffer=io.BytesIO();total=0
with zipfile.ZipFile(buffer,'w',zipfile.ZIP_DEFLATED) as archive:
    for name,p in files.items():
        info=p.stat()
        if not stat.S_ISREG(info.st_mode) or info.st_size>16*1024*1024:raise SystemExit('Unexpected application file size/type.')
        data=p.read_bytes();total+=len(data)
        if len(data)!=info.st_size or total>32*1024*1024:raise SystemExit('Application capture changed or exceeds its limit.')
        archive.writestr(name,data)
sys.stdout.buffer.write(buffer.getvalue())
'''


def capture(device,output):
    output=Path(output)
    if output.exists():raise ValueError('Choose a fresh private application destination.')
    command=['ssh','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','IPQoS=none',
             '-o','ConnectTimeout=10','-o','ServerAliveInterval=5','-o','ServerAliveCountMax=2',target(device),'python3 -B -']
    result=subprocess.run(command,input=REMOTE.encode(),capture_output=True,check=True,timeout=60)
    if len(result.stdout)>32*1024*1024:raise ValueError('Application capture exceeds its limit.')
    with tempfile.TemporaryDirectory(prefix='rgds-drastic-capture-') as folder:
        folder=Path(folder)
        with zipfile.ZipFile(io.BytesIO(result.stdout)) as archive:
            names=archive.namelist()
            if len(names)!=len(set(names)) or not REQUIRED<=set(names)<=REQUIRED|OPTIONAL:
                raise ValueError('Unexpected captured application inventory')
            for info in archive.infolist():
                if info.is_dir() or info.file_size>16*1024*1024:raise ValueError('Invalid captured application entry')
                destination=folder/info.filename;destination.parent.mkdir(parents=True,exist_ok=True)
                destination.write_bytes(archive.read(info))
        return provision(folder,output)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device',required=True);parser.add_argument('--output',type=Path,default=ROOT/'vendor/drastic')
    args=parser.parse_args()
    try:capture(args.device,args.output)
    except (OSError,ValueError,subprocess.SubprocessError) as error:parser.exit(1,str(error)+'\n')
