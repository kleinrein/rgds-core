#!/usr/bin/env python3
"""Wrap a tested application ZIP in a drop-on-card installer (no flashing)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile
ROOT=Path(__file__).resolve().parents[1]

def build(package, output):
    # Run the same validation path that both SSH and SD installs execute.
    subprocess.run([sys.executable,str(ROOT/'packaging/install_package.py'),str(ROOT/'build/install-check'),str(package),'--check'],check=True)
    with zipfile.ZipFile(package) as z:
        version=z.read('rgds/state/active').decode().strip()
    digest=hashlib.sha256(package.read_bytes()).hexdigest()
    installer='''#!/bin/sh
set -eu
RGDS_APPS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RGDS_CARD=$(CDPATH= cd -- "$RGDS_APPS/../.." && pwd)
RGDS_SETUP="$RGDS_CARD/RGDS-Install"
if [ ! -f "$RGDS_SETUP/package.zip" ]; then
    echo "Installer payload is missing. Copy the complete installer ZIP to this card."
    exit 1
fi
if python3 -B "$RGDS_SETUP/install_package.py" "$RGDS_CARD/rgds" "$RGDS_SETUP/package.zip" --sha256 CHECKSUM > "$RGDS_SETUP/install.log" 2>&1; then
    exec sh "$RGDS_CARD/rgds/main.sh"
fi
echo "Installation failed. See RGDS-Install/install.log on the card."
exit 1
'''.replace('CHECKSUM',digest)
    readme=f'''RG DS Core — SD installer candidate {version}

For an RG DS or experimental RG DS Plus with its own working Anbernic Linux card. This is an application
installer, not a replacement OS image. No formatting, flashing or SSH is needed.

1. Extract this ZIP into the root of the games card, merging the Roms folder.
   Copy both Roms and RGDS-Install. Do not copy only the APPS entry.
2. Insert the card and open APPS → Install RG DS Core from the stock menu.
3. The verified install opens RG DS Core. English is the initial language.
4. Future boots open RG DS Core by default, with RG DS Core artwork from power-on
   (the stock boot image is backed up; Uninstall puts it back). The first-run
   guide explains ROM setup.
   Settings → System → Startup → Boot into can switch back to Anbernic.
   Updates preserve your existing startup choice.

To install a newer version, exit your game, return to the stock menu, and repeat
with its installer. Saves, ROMs, settings and cached artwork are retained.
Settings → System → Updates & recovery → Use previous build rolls back the app.
Keep free space for the new version alongside the previous one. The installer
checks space and file hashes before changing the active release. The payload ZIP
is consumed after installation; copy a complete installer again to reinstall.

If installation fails, read RGDS-Install/install.log from the card on a computer.
Settings → System → Startup → Boot into switches back to Anbernic startup.
APPS → Uninstall RG DS Core removes the app and its menu entries and restores the
stock startup and boot artwork; saves and settings stay in rgds/userdata.

Candidate status: private testing only. The payload includes supplied NNDDSS
binaries with unresolved public provenance; project-wide licensing and release
signing are not yet configured. Do not publish this as a public firmware image.
No games, user saves, Wi-Fi profiles or Nintendo BIOS dumps are included.
The OTA foundation is included; no public channel is configured yet.
This installer does not contact an update service.

RG DS Plus: physical testing is still needed. Capture an inventory over SSH,
then test DSperate. See rgds/docs/RG_DS_PLUS.md for the checklist and logs.
Use the Plus's own stock Linux card; do not flash an original RG DS OS image.
Plus OTA and boot-partition/logo modifications remain unvalidated.
'''
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,data,mode in [('Roms/APPS/Install RG DS Core.sh',installer.encode(),0o755),('RGDS-Install/install_package.py',(ROOT/'packaging/install_package.py').read_bytes(),0o644),('RGDS-Install/package.zip',package.read_bytes(),0o644),('RGDS-Install/README.txt',readme.encode(),0o644)]:
            info=zipfile.ZipInfo(name);info.external_attr=(0o100000|mode)<<16;info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,data)
    checksum=hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix+'.sha256').write_text(checksum+'  '+output.name+'\n')
    print(json.dumps(dict(installer=str(output),build=version,sha256=checksum)))
    return output

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--package',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    package=a.package or ROOT/'dist'/(ROOT/'dist/latest.txt').read_text().strip()
    build(package,a.output or ROOT/'dist'/('RG-DS-Core-Installer-'+package.stem.removeprefix('RGDS-')+'.zip'))
