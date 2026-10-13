#!/usr/bin/env python3
"""Owned adapter for the external loader's volume-helper command names."""
import os
from pathlib import Path
import sys


def main(arguments=None):
    arguments=sys.argv[1:] if arguments is None else arguments
    helper=os.environ.get('RGDS_VOLUME_HELPER');data=os.environ.get('RGDS_VOLUME_DATA')
    if not helper or not data:
        raise ValueError('Launch the engine through RGDS so its saved audio policy is available.')
    sys.path.insert(0,str(Path(helper).parent))
    from audio import Volume
    volume=Volume(data)
    if not arguments:return 2
    command=arguments[0]
    if command=='--watch':return 0  # The owned runner already watches physical keys.
    if command=='--get':
        print(round(volume.mixer.query()*255/100));return 0
    if command=='--set-once' and len(arguments)>1:
        raw=int(arguments[1])
        if not 0<=raw<=255:raise ValueError('DAC value is outside0..255.')
        # Native initialization must not override the shared saved level.
        volume.apply();return 0
    if command in ('--up','--down'):
        delta=0 if os.environ.get('RGDS_VOLUME_KEYS')=='1' else 5 if command=='--up' else -5
        volume.apply(delta=delta);print(round(volume.mixer.query()*255/100));return 0
    return 2


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError) as error:print('RGDS audio adapter: '+str(error),file=sys.stderr);sys.exit(1)
