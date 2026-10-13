"""Idle-menu save management and additive experience preferences."""
from pathlib import Path
from fields import read_fields,write_fields
from save_library import SaveLibrary,backup_all

def run(service,payload):
    action=payload.get('action','')
    if action=='settings':
        values=read_fields(service.data/'experience.ini')
        for key in ('quick_resume','auto_update'):
            if key in payload:
                value=str(payload[key])
                if value not in (('0','1','2') if key=='auto_update' else ('0','1')):raise ValueError('Invalid setting.')
                values[key]=value
        write_fields(service.data/'experience.ini',values);service.publish(state='ready',message='Settings saved.');return
    if action=='backup':
        path=backup_all(service.data);service.publish(state='ready',message='Save backup created in userdata/exports.',export=str(path));return
    from emulators import activity_lock,core_running
    with activity_lock(service.data):
        if core_running(service.data):raise ValueError('Use the in-game save gallery while a game is running.')
        rom=Path(payload['rom']).resolve()
        if not rom.is_file() or rom.suffix.lower() not in ('.nds','.zip'):raise ValueError('Game file is unavailable.')
        vault=SaveLibrary(service.data,rom,payload['core'],service.app.name);slot=int(payload.get('slot',0))
        if action=='slots':vault.publish()
        elif action=='export':vault.export(slot)
        elif action=='undo':vault.undo_write()
        elif action=='import':
            name=payload.get('file','')
            if Path(name).name!=name or not name.endswith('.rgdssave.zip'):raise ValueError('Choose an RG DS Core save archive.')
            vault.import_slot(service.data/'imports'/name,slot,bool(payload.get('overwrite')))
        else:raise ValueError('Unknown save action.')
        service.publish(state='ready',message='Save library updated.' if action!='export' else 'Slot exported to userdata/exports.')
