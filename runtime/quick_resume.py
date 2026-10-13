"""Opt-in shutdown resume. One-shot tickets are consumed before any game starts."""
import json
from pathlib import Path
from save_library import atomic_json,digest
from fields import read_fields,write_fields
from library import game_key
from engines import CORES

def config(data):return read_fields(Path(data)/'experience.ini')
def read(path):
    try:v=json.loads(Path(path).read_text());return v if isinstance(v,dict) else {}
    except (OSError,ValueError):return {}
def checkpoint(data,rom,core):return Path(data)/'library'/game_key(Path(rom))/('auto-'+core+'.dss')
def invalidate(data):
    (Path(data)/"resume/candidate.json").unlink(missing_ok=True)

def record_exit(data,rom,core,began):
    invalidate(data)
    if config(data).get("quick_resume")!="1":return
    data=Path(data);p=checkpoint(data,rom,core)
    if core not in CORES or not p.is_file() or p.stat().st_mtime<began:return
    atomic_json(data/'resume/candidate.json',dict(schema=1,rom=str(Path(rom).resolve()),core=core,checkpoint=digest(p),rom_sha256=digest(rom)))
def arm(data):
    data=Path(data);ticket=data/'resume/armed.json';ticket.unlink(missing_ok=True)
    if config(data).get('quick_resume')!='1':return False
    c=read(data/'resume/candidate.json')
    if not valid(data,c):return False
    atomic_json(ticket,c);return True

def valid(data,c):
    if c.get('schema')!=1 or c.get('core') not in CORES or not isinstance(c.get('rom'),str):return False
    rom=Path(c['rom']);p=checkpoint(data,rom,c['core'])
    try:return rom.is_file() and p.is_file() and digest(p)==c.get('checkpoint') and digest(rom)==c.get('rom_sha256')
    except OSError:return False
def consume(data):
    data=Path(data);path=data/'resume/armed.json';c=read(path)
    # Rename to a durable consumed receipt before exposing a launch request.
    # An interrupted attempt cannot replay the original armed ticket.
    if path.exists():
        path.replace(data/'resume/attempted.json')
        from save_library import sync_dir
        sync_dir(path.parent)
    launch=data/'session/resume-launch.ini';launch.unlink(missing_ok=True)
    if config(data).get('quick_resume')!='1' or not valid(data,c):return False
    write_fields(launch,dict(rom=c['rom'],core=c['core']));return True
