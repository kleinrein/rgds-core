#!/usr/bin/env python3
"""Run isolated device comparisons (muted by default) using one ROM and the same settings.

ROM bytes are copied into a private test directory; existing saves are not used. The device must be at stock/home
with no RGDS session. Each core runs normal/fast-forward/normal phases. Presentation FPS is not a measure of emulation accuracy.
"""
import argparse
import json
from pathlib import Path
import shlex
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from runtime.shaders import SHADERS

EXPERIMENT_SETTINGS = frozenset((
    'RGDS_MELONDS_PROFILE', 'RGDS_MELONDS_PRESENT_EVERY_FRAME', 'RGDS_MELONDS_POSIX_SEMAPHORE',
    'RGDS_LAUNCH_PROFILE', 'NNDDSS_CPU_AFFINITY',
    'DS_PROFILE', 'DS_FF_RASTER', 'DS_FRAME_STATS', 'DS_FPS', 'DS_VERBOSE',
    'DS_NO_THREAD_LAYOUT', 'DS_PIN', 'DS_PRESENT_SYNC', 'DS_HOST_CORES',
    'DS_IDLE_SKIP', 'DS_TIMING', 'RGDS_VK_SCALE', 'DS_GPU3D', 'DS_GPU_TRACE', 'RGDS_CAPTURE_GPU',
    'DS_DEBUG_NOLAG', 'DS_DEBUG_NOLAZY', 'DS_VK_TIMING', 'RGDS_COMPOSE_THREADS', 'DS_HIRES_DEBUG', 'DS_HIRES_DUMP', 'DS_STARTUP_TRACE', 'DS_LEAN_D24', 'DS_COMPOSE_THREADS', 'DS_VERIFY_HIRES',
    'VK_ICD_FILENAMES', 'PANVK_ATOM_STRIDE', 'PANVK_KBASE_DVFS', 'PANVK_KBASE_WARMUP_MS', 'PANVK_FORCE_EARLYZS', 'PANVK_SKIP_FS', 'DS_LEAN_MSAA', 'DS_RENDERER_SYNC', 'DS_PRESENT_SYNC', 'DS_JIT_CHURN', 'DS_HANDOFF_STATS', 'DS_LEAN_FLAT', 'DS_PROFILE_THREADS', 'DS_LEAN_NOSTENCIL', 'BIFROST_MESA_DEBUG', 'PAN_MESA_DEBUG', 'PANVK_DEBUG', 'DS_LINE_SPIN', 'DS_THREADS', 'DS_LINE_THREADS', 'DS_ADAPTIVE', 'DS_LEAN_SKIP', 'DS_HIRES_DUMP', 'DS_VK_TIMING', 'DS_JIT_WEEDS', 'DS_JIT_STRICT', 'DS_IDLE_SKIP', 'DS_PRESENT_PROFILE', 'DS_DUAL_SCREENS',
    'DS_KMS_FLIPS', 'DS_PRESENT_LANES', 'DS_PRESENT_QUEUE', 'DS_ADAPTIVE_GRACE_MS',
    'DS_LEAN_HALFPX', 'DS_LEAN_NODISCARD',
    'DS_CADENCE_LOG', 'DS_PACE', 'DS_PACE_BAND', 'DS_PRESENT_FINISH', 'DS_PRESENT_TEST',
    'DS_RGDS_SINK', 'DS_DIRECT_PRESENT', 'DS_DMA_HEAP', 'DS_PRESENT_DROP', 'DS_KMS_DROP',
))

DEVICE_SCRIPT = r'''
import hashlib,json,os,shutil,signal,struct,subprocess,sys,time
from pathlib import Path
options=json.loads(sys.argv[1]);real=Path(options['root']);build=(real/'state/active').read_text().strip();app=real/'releases'/build
sys.path.insert(0,str(app/'runtime'))
from fields import read_fields,write_fields
from library import game_key
from emulator_profiles import section_config
from records import process_identity
from session import group_members
if Path('/tmp/rgds-session.lock').exists():raise SystemExit('Close the current RGDS session before benchmarking.')
rom=Path(options['rom'])
if not rom.is_file():raise SystemExit('ROM is unavailable')
base=real.parent/('benchmark-'+str(time.time_ns()));base.mkdir()
# A private ROM filename prevents importing any existing game's battery save.
# All cores receive identical ROM bytes; source ROM and adjacent saves are untouched.
original_rom=rom
rom=base/'RGDS-benchmark.nds';shutil.copyfile(original_rom,rom)
rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest()
results=[]
if options.get('probe_clock'):
 namespace={};exec(options['probe_clock'],namespace);EmulatedClock=namespace['EmulatedClock']
seeds={}
if options.get('resume_from'):
 source=Path(options['resume_from'])
 if not str(source).startswith('/mnt/mmc/benchmark-') or source.name!='results.json':raise SystemExit('Resume source must be an isolated benchmark results.json')
 for record in json.loads(source.read_text()):
  if record['rom_sha256']!=rom_sha256:raise SystemExit('Checkpoint ROM bytes do not match this workload')
  if len(record.get('checkpoints',[]))!=1:raise SystemExit('Benchmark has no unique checkpoint')
  state=record['checkpoints'][0];path=Path(state['path'])
  if not path.is_relative_to(source.parent) or hashlib.sha256(path.read_bytes()).hexdigest()!=state['sha256']:raise SystemExit('Checkpoint source changed or is outside the isolated benchmark')
  seeds[record['backend']]=path
def wait_for(predicate,timeout=5):
 deadline=time.monotonic()+timeout
 while child.poll() is None and time.monotonic()<deadline:
  if predicate():return True
  time.sleep(.005)
 return False

def native_command(action):
 token='benchmark-'+str(time.monotonic_ns());path=native/'command'
 pending=path.with_suffix('.pending');pending.write_text(token+' '+action+'\n');pending.replace(path)
 ack=wait_for(lambda:read_fields(native/'result').get('token')==token)
 return dict(acknowledged=ack,result=read_fields(native/'result'))

def speed(enabled):
 if native:return native_command('speed-on' if enabled else 'speed-off')
 write_fields(folder/'emulator.control.speed-request',dict(token=str(time.monotonic_ns()),action='fast-forward-on' if enabled else 'fast-forward-off'),durable=False)
 ack=wait_for(lambda:read_fields(folder/'emulator.control.speed-state').get('active')==str(int(enabled)))
 return dict(acknowledged=ack,result=read_fields(folder/'emulator.control.speed-state'))

def benchmark_environment(inherited,overrides):
 # Explicit experiments may override telemetry defaults without duplicate-key errors.
 return {**inherited,'DS_FRAME_STATS':'1','DS_FPS':'1',**overrides}

def measurement_settings(options,has_seed):
 phase=options.get('phase','all')
 return dict(mode='wall_time',requested_phase=phase,volume_percent=options.get('volume',0),
             phases=['normal','fast_forward','normal_restored'] if phase=='all' else [phase],
             seconds_per_phase=options['seconds'],sample_interval_seconds=1,
             initial_warmup_seconds=8 if has_seed else 2,phase_settle_seconds=2,
             menu_cycles=options.get('menu_cycles',3),repeats=options.get('repeats',1),
             save_on_exit=bool(options.get('checkpoints')),clock_probe=bool(options.get('probe_clock')),
             capture_scene=bool(options.get('capture_scene')))

def capture_scene(backend,folder,child):
 # The bridge acknowledges by removing this unique request after both panels
 # (or one combined surface) have been atomically published. Never request GL
 # or SuperDrastic capture; those backends do not implement this protocol.
 record=dict(supported=backend in ('drastic','dsperate','melonds','rgds-experimental'),ready=False,files=[])
 if not record['supported']:
  record['reason']='Backend has no bridge SDL-renderer capture';return record
 request=folder/'emulator.control.capture'
 paths=[folder/('emulator.control.capture-'+panel+'.bmp') for panel in ('top','bottom','combined')]
 for path in paths:path.unlink(missing_ok=True)
 started=time.monotonic();ack=False
 request.write_text(str(time.monotonic_ns())+'\n')
 try:
  deadline=started+3
  while child.poll() is None and time.monotonic()<deadline:
   if not request.exists():ack=True;break
   time.sleep(.005)
 finally:
  # An unserviced request must not trigger a readback during a measured phase.
  # The ordinary two-second phase settle also drains the bridge's 100 ms poll.
  request.unlink(missing_ok=True)
 for path in paths:
  if not path.is_file():continue
  # Menu/checkpoint capture reuses the bridge filenames later in this run.
  # Preserve these pixels now so the recorded hash still identifies the scene.
  snapshot=folder/'benchmark-scene'/path.name
  snapshot.parent.mkdir(exist_ok=True)
  snapshot.write_bytes(path.read_bytes())
  digest=hashlib.sha256()
  with snapshot.open('rb') as stream:
   for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
  record['files'].append(dict(path=str(snapshot),bytes=snapshot.stat().st_size,sha256=digest.hexdigest()))
 present={Path(item['path']).name for item in record['files'] if item['bytes']>0}
 record['acknowledged']=ack
 record['ready']=ack and ('emulator.control.capture-combined.bmp' in present or
                         {'emulator.control.capture-top.bmp','emulator.control.capture-bottom.bmp'}<=present)
 record['finished_monotonic']=time.monotonic();record['elapsed_seconds']=record['finished_monotonic']-started
 return record

def menu_helpers(owner,folder,proc=Path('/proc')):
 # Read-only ownership checks. A numeric ready file alone is not an acknowledgement.
 def identity(pid):
  raw=(proc/str(pid)/'stat').read_text();fields=raw.rsplit(')',1)[1].split()
  return dict(pid=int(pid),start=fields[19],parent=int(fields[1]),state=fields[0],name=raw.split('(',1)[1].rsplit(')',1)[0])
 result=[]
 if not owner:return result
 try:
  if identity(owner['pid'])['start']!=str(owner['start']):return result
  entries=list(proc.iterdir())
 except (OSError,ValueError,IndexError):return result
 for entry in entries:
  if not entry.name.isdigit():continue
  try:
   candidate=identity(entry.name)
   if candidate['name']!='rgds-launcher':continue
   args=(entry/'cmdline').read_bytes().split(b'\0')
   if b'--game-menu' not in args or b'--data' not in args:continue
   if args[args.index(b'--data')+1]!=os.fsencode(folder.parent):continue
   ancestor=candidate;seen=set()
   for _ in range(16):
    if ancestor['parent']==owner['pid']:
     result.append(candidate);break
    if ancestor['parent']<=1 or ancestor['parent'] in seen:break
    seen.add(ancestor['parent']);ancestor=identity(ancestor['parent'])
  except (OSError,ValueError,IndexError):continue
 try:
  if identity(owner['pid'])['start']!=str(owner['start']):return []
 except (OSError,ValueError,IndexError):return []
 return result

def menu_state(folder,owner,overlay):
 helpers=menu_helpers(owner,folder)
 record=dict(blocked=(folder/'emulator.control.blocked').exists(),helpers=helpers,
             ready_file=(folder/'game-menu.ready').exists(),presented=False)
 if len(helpers)!=1 or helpers[0]['state'] in ('Z','X'):return record
 try:record['presented']=(folder/'game-menu.ready').read_text().strip()==str(helpers[0]['pid'])
 except OSError:pass
 if not record['presented'] and overlay is not None:
  try:
   with overlay.open('rb') as stream:header=struct.unpack('7I',stream.read(28))
   record['presented']=header[0]==0x52474453 and bool(header[4] and header[6])
  except (OSError,struct.error):pass
 record['presented']=record['presented'] and record['blocked']
 return record

def open_menu(folder,owner,overlay,child,reuse=False,timeout=5):
 started=time.monotonic();state=menu_state(folder,owner,overlay)
 record=dict(presented=False,reused=bool(state['blocked'] or state['helpers']))
 if record['reused'] and not reuse:
  record.update(seconds=0,error='Previous menu is still open; no toggle sent',state=state);return record
 if not record['reused']:
  (folder/'game-menu.ready').unlink(missing_ok=True)
  (folder/'emulator.control.game-menu').touch()
 while child.poll() is None and time.monotonic()-started<timeout:
  state=menu_state(folder,owner,overlay)
  if state['presented']:
   record.update(presented=True,helper={key:state['helpers'][0][key] for key in ('pid','start')});break
  time.sleep(.01)
 record.update(seconds=round(time.monotonic()-started,3),state=state)
 if not record['presented']:record['error']='No presented frame from an owned live menu helper before timeout'
 return record

def resume_menu(folder,owner,overlay,child,helper,timeout=3):
 started=time.monotonic();token='benchmark-resume-'+str(time.monotonic_ns())
 write_fields(folder/'game-menu.request',dict(token=token,action='resume'),durable=False)
 resumed=False;state={}
 while child.poll() is None and time.monotonic()-started<timeout:
  state=menu_state(folder,owner,overlay)
  reaped=process_identity(helper['pid'])!=helper['start']
  if reaped and not state['helpers'] and not state['blocked'] and not state['ready_file']:
   resumed=True;break
  time.sleep(.01)
 record=dict(resumed=resumed,resume_seconds=round(time.monotonic()-started,3),resume_token=token,resume_state=state)
 if not resumed:record['resume_error']='Menu helper did not finish closing and release input before timeout'
 return record

def hardware_sample(owner):
 sample=dict(monotonic=time.monotonic(),clocks={},thermal={},processes=[],audio_queue=[])
 def pcm_fields(path):
  try:return dict((k.strip(),v.strip()) for k,sep,v in (line.partition(":") for line in path.read_text().splitlines()) if sep)
  except OSError:return {}
 for status in Path("/proc/asound").glob("card*/pcm*p/sub*/status"):
  fields=pcm_fields(status);params=pcm_fields(status.with_name("hw_params"))
  try:
   rate=int(params["rate"].split()[0]);delay=int(fields["delay"]);sample["audio_queue"].append(dict(device=str(status.parent),rate=rate,delay_frames=delay,queued_ms=round(delay*1000/rate,3),state=fields.get("state")))
  except (KeyError,ValueError,ZeroDivisionError):pass
 patterns=('/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq',
           '/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq',
           '/sys/devices/system/cpu/cpufreq/policy*/scaling_governor',
           '/sys/class/devfreq/*/cur_freq','/sys/class/devfreq/*/max_freq',
           '/sys/class/devfreq/*/governor','/sys/class/thermal/thermal_zone*/temp')
 import glob
 for pattern in patterns:
  for path in glob.glob(pattern):
   try:sample['thermal' if '/thermal/' in path else 'clocks'][path]=Path(path).read_text().strip()
   except OSError:pass
 if owner and process_identity(owner['pid'])==owner['start']:
  # Some stock kernels omit /proc/PID/task/PID/children; walk PPIDs instead.
  processes={}
  for proc in Path('/proc').iterdir():
   if not proc.name.isdigit():continue
   try:
    raw=(proc/'stat').read_text();fields=raw.rsplit(')',1)[1].split()
    processes[int(proc.name)]=dict(pid=int(proc.name),parent=int(fields[1]),name=raw.split('(',1)[1].rsplit(')',1)[0],state=fields[0],cpu_ticks=int(fields[11])+int(fields[12]),start=fields[19])
   except (OSError,ValueError,IndexError):pass
  owned={owner['pid']}
  while True:
   children={pid for pid,p in processes.items() if p['parent'] in owned}-owned
   if not children:break
   owned.update(children)
  sample['processes']=[processes[p] for p in sorted(owned) if p in processes]

 return sample

def phase_frame_times(path,phases):
 # DS_FRAME_SERIES uses CLOCK_MONOTONIC nanoseconds, matching phase bounds.
 # Only whole emulation+presentation work intervals inside a phase count.
 import math,statistics
 buckets={p['phase']:dict(emu_ms=[],work_ms=[]) for p in phases}
 if not path.is_file():return {}
 with path.open() as stream:
  for line in stream:
   try:
    emu,work,start,end=map(float,line.split())
   except ValueError:continue
   if not all(math.isfinite(v) for v in (emu,work,start,end)) or emu<0 or work<emu or start>end:continue
   work_end=start/1e9+work/1000
   for phase in phases:
    if phase['started_monotonic']<=start/1e9 and work_end<=phase['ended_monotonic']:
     buckets[phase['phase']]['emu_ms'].append(emu)
     buckets[phase['phase']]['work_ms'].append(work)
     break
 def summary(values):
  if not values:return dict(frames=0)
  values.sort()
  def percentile(f):return values[max(0,math.ceil(len(values)*f)-1)]
  return dict(frames=len(values),mean=statistics.fmean(values),median=statistics.median(values),
              p95=percentile(.95),p99=percentile(.99),maximum=values[-1],
              over_budget=sum(v>1000/59.8261 for v in values))
 return {phase:{kind:summary(values) for kind,values in kinds.items()} for phase,kinds in buckets.items()}
for repeat,variant in [(r,c) for r in range(options.get('repeats',1)) for c in options['cores']]:
 core='drastic' if variant=='superdrastic' else variant
 root=base/(variant+'-'+str(repeat))/'rgds';(root/'state').mkdir(parents=True)
 try:(root/'releases').symlink_to(real/'releases',target_is_directory=True)
 except OSError as error:
  # The Mac-readable games partition may be FAT/exFAT (no symlinks).
  if error.errno not in (1,95):raise
  shutil.copytree(app,root/'releases'/build)
 (root/'state/active').write_text(build+'\n');data=root/'userdata';data.mkdir()
 seed=seeds.get(variant)
 if options.get('resume_from') and seed is None:raise SystemExit('Missing checkpoint for '+variant)
 if seed:
  destination=data/'library'/game_key(rom)/('auto-'+core+'.dss');destination.parent.mkdir(parents=True);shutil.copyfile(seed,destination)
 write_fields(data/'preferences.ini',dict(schema=1,mode='light',font='rounded',core=1,auto_load=int(bool(seed)),auto_save=int(options.get('checkpoints',False))))
 if options.get('feature_checks'):write_fields(data/'experience.ini',dict(quick_resume=1,auto_update=0))
 write_fields(data/'system.ini',dict(schema=1,power=0,brightness=20,idle=0,lid=0,battery_percent=1,timezone='Europe/Oslo'))
 scale=options.get('resolution',1);gengis=options.get('rasterizer','standard')=='gengis'
 tuning=data/'library'/game_key(rom)/'game.ini';tuning.parent.mkdir(parents=True,exist_ok=True);write_fields(tuning,dict(schema=1,rgds_scale=options.get('rgds_scale',1),rgds_smooth=options.get('rgds_smooth',1),rgds_2d=options.get('rgds_2d',0),rgds_direct=options.get('rgds_direct',0),threaded_3d=options.get('threaded_3d',1),hires_3d=int(scale>1),gengis_scale=scale if gengis else 0))
 write_fields(data/'volume.ini',dict(schema=1,volume=options.get('volume',0)));write_fields(data/'ui.ini',dict(schema=1,filter=options['shader'],drastic_backend=int(variant=='superdrastic')));write_fields(data/'performance.ini',dict(schema=1,fps=1))
 # Explicit maximum-speed settings belong only to this disposable profile.
 if options.get('max_speed'):
  profile=data/'emulators'/core
  profile.mkdir(parents=True,exist_ok=True)
  if core in ('dsperate','rgds-experimental'):
   shutil.copyfile(app/'assets/emulator-profiles/dsperate.ini',profile/'config.ini')
   section_config(profile/'config.ini',{'emu':{'ff_speed':0,'ff_skip':0}})
 child_environment=benchmark_environment(os.environ,options.get('engine_env',{}))
 # Keep per-frame timing inside the disposable profile. Phase boundaries below
 # let analysis exclude launch, checkpoint IO and menu pauses from gameplay.
 if core in ('dsperate','rgds-experimental'):
  child_environment['DS_FRAME_SERIES']=str(data/'frame-series.csv')
 result=dict(samples=[],clock_ticks=os.sysconf('SC_CLK_TCK'),core=core,backend=variant,repeat=repeat,build=build,rom=str(original_rom),test_rom=str(rom),rom_sha256=rom_sha256,root=str(root),metric='presentation',rgds_scale=options.get('rgds_scale',1),rgds_smooth=options.get('rgds_smooth',1),rgds_direct=options.get('rgds_direct',0),shader=options['shader'],threaded_3d=options.get('threaded_3d',1),resolution=('Native' if options.get('rgds_scale',1)==1 else str(options['rgds_scale'])+'x') if core=='rgds-experimental' else ('Native' if scale==1 else str(scale)+'x'),rasterizer=options.get('rasterizer','standard'),frameskip=0,seed_sha256=hashlib.sha256(seed.read_bytes()).hexdigest() if seed else None,
             fast_forward_config={'limit_multiplier':0,'render_skip':0} if options.get('max_speed') else 'profile defaults',
             engine_env={key:child_environment[key] for key in options['experiment_settings'] if key in child_environment},measurement=measurement_settings(options,bool(seed)),phases=[])
 output=(data/'runner.log').open('w');began=time.monotonic()
 child=subprocess.Popen([sys.executable,'-B',str(app/'runtime/session.py'),'--root',str(root),'--core',core,'--rom',str(rom)],env=child_environment,stdout=output,stderr=output,start_new_session=True)
 folder=data/'session';engine=None;owner=None;frame=None;native=None;probe_clock=None;shared_native=False;native_overlay=None
 try:
  while time.monotonic()-began<60 and child.poll() is None:
   try:owner=json.loads((folder/'core.json').read_text())
   except (OSError,ValueError):time.sleep(.02);continue
   if variant=='superdrastic':
    candidates=list(folder.glob('superdrastic-*/ready'))
    if candidates and candidates[0].read_text().strip()=='ready':
     native=candidates[0].parent;result['native_ready_seconds']=round(time.monotonic()-began,3);break
   candidates=list(Path('/dev/shm').glob('rgds-menu-'+str(owner['pid'])+'-*.frame.stats'))
   if candidates:
    frame=candidates[0];result['first_stats_seconds']=round(time.monotonic()-began,3);break
   time.sleep(.02)
  if frame or native:
   result['metric']='native readiness only' if native else 'presentation'
   if native:
    engines=[p['pid'] for p in hardware_sample(owner)['processes'] if p['name']=='drastic']
    if len(engines)==1:
     environment=dict(item.split('=',1) for item in Path('/proc/'+str(engines[0])+'/environ').read_bytes().decode().split('\0') if '=' in item)
     shared_native=environment.get('RGDS_SD_SHARED_MENU')=='1'
     if shared_native:native_overlay=Path(environment['RGDS_OVERLAY_FILE'])
   result['shared_menu']=bool(shared_native or not native)
   time.sleep(result['measurement']['initial_warmup_seconds']) # Permit native/bridge checkpoint loads to finish.
   if options.get('capture_scene'):result['scene_capture']=capture_scene(variant,folder,child)
   if options.get('probe_clock'):
    processes=hardware_sample(owner)['processes'];names=('drastic',) if core=='drastic' else (core,)
    engines=[p['pid'] for p in processes if p['name'] in names]
    if len(engines)!=1:raise ValueError('Could not identify benchmark engine for original-ROM clock')
    probe_clock=EmulatedClock(engines[0],root,rom,allow_zero_mapping=(core=='drastic'),allow_drastic_mapping=(core=='drastic'))
    result['clock_probe']=probe_clock.find()
   for phase,enabled in [('normal',False),('fast_forward',True),('normal_restored',False)]:
    if options.get('phase','all')!='all' and phase!=options['phase']:continue
    acknowledgement=speed(enabled);time.sleep(result['measurement']['phase_settle_seconds'])
    result.setdefault('speed',{})[phase]=acknowledgement
    phase_started=time.monotonic();first_sample=len(result['samples'])
    deadline=phase_started+options['seconds']
    while time.monotonic()<deadline and child.poll() is None:
     sample=hardware_sample(owner);sample['phase']=phase
     if frame:sample['stats']=read_fields(frame)
     if probe_clock:sample['emulated_clock']=probe_clock.sample()
     result['samples'].append(sample);time.sleep(min(result['measurement']['sample_interval_seconds'],max(0,deadline-time.monotonic())))
    ended=time.monotonic()
    result['phases'].append(dict(phase=phase,started_monotonic=phase_started,ended_monotonic=ended,
                                elapsed_seconds=ended-phase_started,samples=len(result['samples'])-first_sample,
                                completed=child.poll() is None and ended>=deadline,speed_acknowledged=acknowledgement['acknowledged']))
   if frame:
    result['stats']=read_fields(frame)
    if 'game_fps' in result['stats']:result['metric']='game + presentation'
    if result['stats'].get('first_present_us'):result['first_present_seconds']=round(int(result['stats']['first_present_us'])/1000000-began,3)
   result['speed_after_phases']=speed(False);time.sleep(.25)
   # Wait for a newly presented menu frame in either renderer mode.
   cycles=[]
   for cycle in range(options.get('menu_cycles',3)):
    opening=time.monotonic();ack=False
    if native and not shared_native:
     native_command('menu')
     ack=wait_for(lambda:read_fields(native/'menu').get('open')=='1')
     cycles.append(dict(seconds=round(time.monotonic()-opening,3),presented=False,native_menu_ack=ack))
     if not ack:break
     time.sleep(.3);native_command('menu')
     if not wait_for(lambda:read_fields(native/'menu').get('open')=='0'):break
     time.sleep(.3);continue
    overlay=native_overlay if shared_native else Path(str(frame).removesuffix('.stats'))
    cycle_result=open_menu(folder,owner,overlay,child);cycles.append(cycle_result)
    if not cycle_result['presented']:break
    if shared_native and cycle==0:
     before=native_overlay.read_bytes();header=struct.unpack('7I',before[:28])
     if header[0]==0x52474453 and header[3]%2==0 and header[5]<2 and header[6]>0:
      capture=root/'menu-frame.rgba';start=64+header[5]*640*480*4*2
      capture.write_bytes(before[start:start+640*480*4*2])
      result['menu_capture']=dict(path=str(capture),width=640,height=960,sha256=hashlib.sha256(capture.read_bytes()).hexdigest())
    if probe_clock:
     first=probe_clock.sample();time.sleep(.35);last=probe_clock.sample()
     cycle_result['paused_emulated_frames']=last['vblank_count']-first['vblank_count']
    else:time.sleep(.3)
    cycle_result.update(resume_menu(folder,owner,overlay,child,cycle_result['helper']))
    if not cycle_result['resumed']:break
    time.sleep(.3)
   result['menus']=cycles
  else:result['error']='No presentation telemetry/native readiness within 60 seconds'
  if options.get('switch_to') and frame and child.poll() is None:
   # In-game 3D resolution change through the real menu request: a live
   # change between 2x-5x, or a checkpoint + relaunch across native.
   switch=dict(target=options['switch_to'])
   overlay=Path(str(frame).removesuffix('.stats'))
   opened=open_menu(folder,owner,overlay,child);switch['menu']=opened.get('presented',False)
   if switch['menu']:
    before=set(Path('/dev/shm').glob('rgds-menu-*.frame.stats'))
    token='benchmark-switch-'+str(time.monotonic_ns());requested=time.monotonic()
    write_fields(folder/'game-menu.request',dict(token=token,action='graphics:%d:%d:0'%(options['switch_to'],options.get('rgds_smooth',1))),durable=False)
    while time.monotonic()-requested<60 and child.poll() is None:
     fresh=sorted(set(Path('/dev/shm').glob('rgds-menu-*.frame.stats'))-before)
     if fresh and read_fields(fresh[0]).get('first_present_us'):
      frame=fresh[0];switch.update(mode='relaunch',seconds=round(time.monotonic()-requested,3));break
     status=read_fields(folder/'game-menu.status')
     if status.get('request_token')==token and status.get('state') in ('paused','error'):
      switch.update(mode='live',seconds=round(time.monotonic()-requested,3),status=status)
      helper=menu_state(folder,owner,overlay)['helpers'];switch['resume']=resume_menu(folder,owner,overlay,child,helper[0]) if helper else None
      break
     time.sleep(.02)
    else:switch['error']='No relaunch or live acknowledgement within 60 seconds'
    if 'mode' in switch:
     time.sleep(4)
     switch['stats']=read_fields(frame)
     if options.get('capture_scene'):switch['capture']=capture_scene(variant,folder,child)
     snapshot=folder/'benchmark-scene'
     for name in ('emulator.control.capture-top.bmp','emulator.control.capture-bottom.bmp'):
      if (snapshot/name).is_file():(snapshot/name).replace(snapshot/('switch-'+name))
   result['switch']=switch
  if child.poll() is None:
   exiting=time.monotonic();quit_sent=False
   if native and not shared_native:
    engines=[p['pid'] for p in hardware_sample(owner)['processes'] if p['name']=='drastic']
    if len(engines)!=1:raise RuntimeError('Could not identify owned DraStic process')
    os.kill(engines[0],signal.SIGUSR1);quit_sent=True
   else:
    # An earlier open failure may have left a menu alive. Never toggle it closed
    # immediately before requesting quit; retain the failed cycle as evidence.
    overlay=native_overlay if shared_native else Path(str(frame).removesuffix('.stats')) if frame else None
    closing=bool(result.get('menus') and result['menus'][-1].get('resumed') is False)
    if closing:
     def close_finished():
      state=menu_state(folder,owner,overlay)
      return not state['helpers'] and not state['blocked'] and not state['ready_file'] and process_identity(result['menus'][-1]['helper']['pid'])!=result['menus'][-1]['helper']['start']
     closing=not wait_for(close_finished,timeout=3)
    result['quit_menu']=(dict(presented=False,error='Previous resume is still closing; no toggle or quit request sent')
                         if closing else open_menu(folder,owner,overlay,child,reuse=True))
    if result['quit_menu']['presented'] and options.get('feature_checks'):
     checks=[]
     for action in ['slot-save:0','slot-save:0:yes','slot-undo','slot-load:0','slot-undo-load','slot-export:0']:
      request='feature-'+str(time.monotonic_ns());started=time.monotonic()
      write_fields(folder/'game-menu.request',dict(token=request,action=action),durable=False)
      ok=wait_for(lambda:read_fields(folder/'game-menu.status').get('request_token')==request and read_fields(folder/'game-menu.status').get('state')!='busy',timeout=12)
      state=read_fields(folder/'game-menu.status');checks.append(dict(action=action,acknowledged=ok,seconds=round(time.monotonic()-started,3),status=state))
      if not ok or state.get('state')=='error':break
     if core in ('dsperate','melonds','rgds-experimental'):
      for action in ['ds:blow','ds:lid']:
       request='feature-'+str(time.monotonic_ns());write_fields(folder/'game-menu.request',dict(token=request,action=action),durable=False)
       ok=wait_for(lambda:not menu_state(folder,owner,overlay)['blocked'],timeout=5);time.sleep(2.2)
       reopened=open_menu(folder,owner,overlay,child,reuse=True);checks.append(dict(action=action,resumed=ok,reopened=reopened['presented']))
     result['feature_checks']=checks
    if result['quit_menu']['presented']:
     token='benchmark-quit-'+str(time.monotonic_ns())
     result['quit_request_token']=token;quit_sent=True
     write_fields(folder/'game-menu.request',dict(token=token,action='quit'),durable=False)
    else:result['quit_error']='Quit was not requested because menu readiness was not confirmed'
   if quit_sent:
    try:child.wait(timeout=12)
    except subprocess.TimeoutExpired:result['quit_timeout']=True
   result['exit_seconds']=round(time.monotonic()-exiting,3)
 finally:
  if probe_clock:probe_clock.close()
  if child.poll() is None:
   child.terminate()
   try:child.wait(timeout=10)
   except subprocess.TimeoutExpired:raise SystemExit('Owned benchmark did not recover; stop before another core.')
  output.close();result['exit_code']=child.returncode
  if core in ('dsperate','rgds-experimental'):
   result['frame_times']=phase_frame_times(data/'frame-series.csv',result['phases'])
  try:result['direct_display']=json.loads((data/'session/direct-display.json').read_text())
  except (OSError,ValueError):pass
  result['checkpoints']=[dict(path=str(p),bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in (data/'library').glob('*/auto-*.dss')]
  if native:result['native_exit']=read_fields(native/'exit')
  result['native_log']=(data/'logs/superdrastic.log').read_text()[-18000:] if (data/'logs/superdrastic.log').exists() else ''
  result['engine_log']=(data/'logs'/(core+'.log')).read_text()[-18000:] if (data/'logs'/(core+'.log')).exists() else ''
  result['session_log']=(data/'session.log').read_text()[-32000:] if (data/'session.log').exists() else (data/'runner.log').read_text()[-32000:]
  results.append(result);(base/'results.json').write_text(json.dumps(results,indent=2)+'\n')
  print(json.dumps(result),flush=True)
 if Path('/tmp/rgds-session.lock').exists():raise SystemExit('Recovery is pending; stopped comparison.')
if not options.get('keep'):
 # Each run copies the ROM and a full profile (hundreds of MB, ~1k files).
 # Keep only the report and scene captures, so repeated runs never fill
 # the card (a full FAT card once went read-only after a bulk delete).
 for path in sorted(base.rglob('*'),key=lambda p:len(p.parts),reverse=True):
  if path.is_symlink() or path.is_file():
   if path.name!='results.json' and path.parent.name!='benchmark-scene':path.unlink()
  elif path.is_dir() and not any(path.iterdir()):path.rmdir()
 subprocess.run(['sync'],check=False)
print('RESULTS '+str(base/'results.json'),flush=True)
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device',required=True)
    parser.add_argument('--root',default='/mnt/mmc/rgds')
    parser.add_argument('--keep',action='store_true',help='Keep the run folder (ROM copy, profile, checkpoints) for --resume-from')
    parser.add_argument('--switch-to',type=int,choices=range(1,6),help='After the phases, change DSCore 3D resolution through the in-game menu and capture the result')
    parser.add_argument('--rom',required=True)
    parser.add_argument('--seconds',type=int,default=30)
    parser.add_argument('--volume',type=int,choices=range(101),default=0,help='Isolated test volume, 0-100; use audible audio for gameplay acceptance')
    parser.add_argument('--shader',type=int,choices=range(len(SHADERS)),default=0)
    parser.add_argument('--cores',nargs='+',choices=('drastic','dsperate','melonds','superdrastic','rgds-experimental'),default=['drastic','dsperate','melonds','superdrastic'])
    parser.add_argument('--feature-checks',action='store_true',help='Exercise slot save/load/undo/export and supported DS controls in isolated profiles')
    parser.add_argument('--checkpoints',action='store_true',help='Test save-on-exit in disposable profiles')
    parser.add_argument('--phase',choices=('all','normal','fast_forward'),default='all',help='Use one phase in fresh profiles for repeatable comparisons')
    parser.add_argument('--repeats',type=int,choices=range(1,6),default=1)
    parser.add_argument('--max-speed',action='store_true',help='Uncap DSperate/fork with no presentation skipping; set NNDDSS to its supported 10x maximum')
    parser.add_argument('--menu-cycles',type=int,choices=range(0,11),default=3)
    parser.add_argument('--resume-from',help='Remote results.json from an isolated benchmark with matching ROM bytes')
    parser.add_argument('--probe-clock',action='store_true',help='Read the original RGDS homebrew VBlank clock from the owned test process')
    parser.add_argument('--threaded-3d',type=int,choices=(0,1),default=1,help='Isolated per-game threading setting')
    parser.add_argument('--resolution',type=int,choices=(1,2,3),default=1,help='Internal 3D scale; 3 requires SuperDrastic and Gengis')
    parser.add_argument('--rasterizer',choices=('standard','gengis'),default='standard')
    parser.add_argument('--capture-scene',action='store_true',help='Capture supported bridge-renderer scenes after warmup, before measured phases')
    parser.add_argument('--rgds-smooth',type=int,choices=(0,1),default=1,help='RG DS Experimental 3D supersampling resolve')
    parser.add_argument('--rgds-2d',type=int,choices=(0,1,2,3),default=0,help='DSCore 2D scaling: 0 native pixels, 1 sharp, 2 smooth, 3 crisp')
    parser.add_argument('--rgds-scale',type=int,choices=range(1,6),default=1,help='RG DS Experimental internal 3D scale')
    parser.add_argument('--rgds-direct',type=int,choices=(0,1),default=0,help='DSCore direct display: Weston stops while the engine presents on KMS')
    parser.add_argument('--engine-env',action='append',default=[],metavar='KEY=VALUE',help='Opt-in profiling/render experiment setting')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if not 5<=args.seconds<=3600:parser.error('--seconds must be 5..3600')
    if args.rgds_scale>1 and set(args.cores)!={'rgds-experimental'}:parser.error('--rgds-scale requires RG DS Experimental')
    if args.rgds_direct and set(args.cores)!={'rgds-experimental'}:parser.error('--rgds-direct requires RG DS Experimental')
    if args.max_speed and not set(args.cores)<={'dsperate','rgds-experimental'}:parser.error('--max-speed supports DSperate and RG DS Experimental')
    if args.rasterizer=='gengis' and (set(args.cores)!={'superdrastic'} or args.resolution==1):
        parser.error('Gengis requires --cores superdrastic and --resolution 2 or 3')
    if args.resolution==3 and args.rasterizer!='gengis':parser.error('3x requires --rasterizer gengis')
    if args.resolution>1 and not set(args.cores)<={'drastic','superdrastic'}:
        parser.error('Selected core does not implement higher internal resolution')
    import device
    engine_env={}
    for assignment in args.engine_env:
        key,sep,value=assignment.partition('=')
        if not sep or key not in EXPERIMENT_SETTINGS:
            parser.error('Unsupported experiment environment setting')
        engine_env[key]=value
    options=dict(root=args.root,rom=args.rom,seconds=args.seconds,volume=args.volume,cores=args.cores,shader=args.shader,checkpoints=args.checkpoints,
                 max_speed=args.max_speed,feature_checks=args.feature_checks,phase=args.phase,repeats=args.repeats,menu_cycles=args.menu_cycles,resume_from=args.resume_from,
                 keep=args.keep,switch_to=args.switch_to,rgds_scale=args.rgds_scale,rgds_smooth=args.rgds_smooth,rgds_2d=args.rgds_2d,rgds_direct=args.rgds_direct,engine_env=engine_env,experiment_settings=sorted(EXPERIMENT_SETTINGS),capture_scene=args.capture_scene,threaded_3d=args.threaded_3d,resolution=args.resolution,rasterizer=args.rasterizer)
    if args.probe_clock:options['probe_clock']=(ROOT/'tools/probe_emulated_clock.py').read_text()
    import subprocess
    try:result=device.ssh(device.target(args.device),'python3 -B - '+shlex.quote(json.dumps(options)),batch=True,input=DEVICE_SCRIPT,text=True,capture_output=True)
    except subprocess.CalledProcessError as error:
        result=error  # Preserve failure evidence, too.
        result.stdout=error.stdout or '';result.stderr=error.stderr or ''
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(result.stdout+result.stderr)
    failed=False
    for line in result.stdout.splitlines():
        if line.startswith('{'):
            report=json.loads(line)
            # SSH completing successfully does not mean the emulator launched.
            # Keep the full evidence, but make callers stop on a failed run.
            failed |= bool(report.get('error')) or report.get('exit_code',0)!=0
            print(json.dumps({key:report[key] for key in ('backend','exit_code','first_present_seconds','native_ready_seconds','menus','error') if key in report}))
        else:print(line)
    print(result.stderr,end='',file=sys.stderr)
    return result.returncode or int(failed)

if __name__=='__main__':raise SystemExit(main())
