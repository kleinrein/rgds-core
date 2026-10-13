#!/usr/bin/env python3
"""Exercise the ARM64 SDL frontend and actual RGDS bridge in a Linux VM.

Only original synthetic homebrew and SDL dummy drivers are used. This does not
measure RG DS game performance or validate physical screens, touch or sound.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
def read_fields(path):
    try:return dict(line.split('=',1) for line in path.read_text().splitlines() if '=' in line)
    except FileNotFoundError:return {}


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def probe(output):
    if platform.system()!='Linux' or platform.machine()!='aarch64':raise ValueError('Requires Linux ARM64')
    output.mkdir(parents=True,exist_ok=False)
    engine=ROOT/'build/melonds-experiment/melonds';bridge=ROOT/'build/device/librgdsbridge.so'
    sdk=ROOT/'build/engine-cross/sdl-sdk/lib'
    command=[str(engine),'--self-test','--save',str(output/'battery.sav'),'--state',str(output/'checkpoint.dss')]
    env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',LD_LIBRARY_PATH=str(sdk))
    for name in ('LD_PRELOAD','RGDS_CORE','RGDS_CONTROL_FILE','RGDS_AUTOLOAD','RGDS_NATIVE_AUTOLOAD'):env.pop(name,None)
    checks={};start=time.monotonic()
    result=subprocess.run(command+['--frames','60'],env=env,capture_output=True,text=True,timeout=30)
    (output/'frontend.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise ValueError('Frontend self-test failed; see frontend.log')
    checks['frontend']=json.loads(result.stdout.splitlines()[-1])
    # Repeat the exact original cartridge and emulated-frame count. Drawing
    # less often in FF must not reduce CPU progress or bypass save/input tests.
    speed_rows=[]
    for every_frame in (True,False):
        speed_env=dict(env,RGDS_MELONDS_PROFILE='1',RGDS_MELONDS_PRESENT_EVERY_FRAME='1' if every_frame else '0')
        speed_command=[str(engine),'--self-test-fast','--save',str(output/f'fast-{int(every_frame)}.sav'),
                       '--state',str(output/f'fast-{int(every_frame)}.dss'),'--frames','360']
        speed_result=subprocess.run(speed_command,env=speed_env,capture_output=True,text=True,timeout=30)
        (output/f'fast-{int(every_frame)}.log').write_text(speed_result.stdout+speed_result.stderr)
        if speed_result.returncode:raise ValueError('Fast-forward synthetic replay failed')
        row=json.loads(speed_result.stdout.splitlines()[-1]);row['present_every_frame']=every_frame
        row['profile']=[json.loads(line) for line in speed_result.stderr.splitlines() if line.startswith('{"type":"melonds-profile"')]
        assert row['frames']==360 and row['fast_forward'] and row['profile']
        speed_rows.append(row)
    assert speed_rows[0]['progress']==speed_rows[1]['progress'] and speed_rows[0]['progress']>0
    assert speed_rows[0]['presented']==360 and 1<speed_rows[1]['presented']<360
    checks['fast_forward_replay']=speed_rows
    control=output/'control';state=output/'checkpoint.dss';auto=output/'automatic.dss'
    env.update(LD_PRELOAD=str(bridge),RGDS_CORE='melonds',RGDS_SHARED_MENU='1',RGDS_MENU_LIVE='0',
        RGDS_CONTROL_FILE=str(control),RGDS_STATE_SLOT=str(state),RGDS_STATE_DIR=str(output),
        RGDS_AUTOSTATE_FILE=str(auto),RGDS_AUTOSAVE='0',RGDS_AUTOLOAD='0',RGDS_FPS='1',
        RGDS_FRAME_STATS=str(output/'frames.ini'),RGDS_VOLUME_EXTERNAL='1',RGDS_SHADER='0')
    results=[]
    with (output/'bridge.log').open('w') as log:
        child=subprocess.Popen(command+['--frames','100000'],env=env,stdout=log,stderr=log)
        def wait_until(predicate,timeout=8):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                if predicate():return
                if child.poll() is not None:raise ValueError('Frontend exited early; see bridge.log')
                time.sleep(.01)
            raise TimeoutError('Frontend/bridge acknowledgement timed out')
        def send(action,expected):
            token=str(len(results)+1);staged=output/'pending'
            staged.write_text(token+' '+action+'\n');staged.replace(control)
            began=time.monotonic()
            wait_until(lambda:read_fields(Path(str(control)+'.result'))==dict(token=token,state=expected))
            results.append(dict(action=action,state=expected,seconds=time.monotonic()-began))
        try:
            wait_until(lambda:(output/'frames.ini').is_file())
            telemetry=read_fields(output/'frames.ini')
            assert float(telemetry['game_fps'])>0 and abs(float(telemetry['game_fps'])-float(telemetry['fps']))<3
            checks['game_and_display_fps']=telemetry
            for _ in range(5):send('threaded:0','threaded-off');send('threaded:1','threaded-on')
            send('save','saved');assert digest(state)==digest(auto)
            send('load','loaded')
            # A malformed state must receive failure, not a delayed success guess.
            good=auto.read_bytes();auto.write_bytes(b'invalid state');send('load','load-failed');auto.write_bytes(good)
            send('load','loaded')
            # Pausing the owned process must stop its CPU; resume retains command service.
            child.send_signal(signal.SIGSTOP);time.sleep(.04)
            def ticks():return Path(f'/proc/{child.pid}/stat').read_text().rsplit(')',1)[1].split()[11:13]
            before=ticks();time.sleep(.15);assert before==ticks()
            child.send_signal(signal.SIGCONT);send('threaded:0','threaded-off')
            send('fast-forward','fast-forward-on');send('fast-forward','fast-forward-off')
            send('quit','quitting');assert child.wait(timeout=5)==0
        finally:
            if child.poll() is None:
                child.send_signal(signal.SIGCONT);child.terminate()
                try:child.wait(timeout=5)
                except subprocess.TimeoutExpired:child.kill();child.wait()
    checks['bridge']=results
    record=dict(schema=1,engine_sha256=digest(engine),bridge_sha256=digest(bridge),
        system=platform.platform(),elapsed_seconds=time.monotonic()-start,checks=checks,
        device_tested=False,limitations=['synthetic homebrew only','SDL dummy video/audio','not an RG DS performance comparison'])
    (output/'report.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(dict(report=str(output/'report.json'),bridge_commands=len(results),passed=True)))
    return record

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    probe(parser.parse_args().output.resolve())
