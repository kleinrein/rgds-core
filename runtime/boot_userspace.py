#!/usr/bin/env python3
"""Reversible late-splash branding; never opens a boot partition."""
import argparse
import fcntl
import json
from pathlib import Path
import boot_branding as branding
from boot_control import atomic, location

STORE = Path('/userdata/rgds-boot/userspace-branding')


def inspect(system_root='/'):
    """Report file identity without implying the firmware actually displays it."""
    root = Path(system_root).resolve()
    store = location(root, STORE)
    manifest = store / 'manifest.json'
    record = json.loads(branding.regular(manifest)) if manifest.exists() or manifest.is_symlink() else {}
    known = {str(path) for path, _asset in branding.USER_LOGOS}
    results = []
    for item in record.get('files', []):
        if item.get('path') not in known:
            raise ValueError('Unexpected splash path.')
        path = location(root, Path(item['path']))
        if path.exists() or path.is_symlink():
            digest = branding.sha256(branding.regular(path))
            state = 'installed' if digest == item['installed_sha256'] else 'original' if digest == item['original_sha256'] else 'changed'
        else:
            state = 'missing'
        results.append(dict(path=item['path'], state=state))
    return dict(scope='userspace_splash_files', files=results,
                early_boot_logo_changed=False, visual_boot_verified=False,
                message='Later splash files only. The first logo is separate boot-image artwork; file installation does not prove it was displayed.')


def install(assets, system_root='/', restore=False):
    root=Path(system_root).resolve(); store=location(root,STORE)
    if store.is_symlink() or any(p.is_symlink() for p in store.parents):
        raise ValueError('Branding backup path contains a symlink.')
    branding.durable_store(store)
    with (store/'lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        manifest=store/'manifest.json'
        if manifest.exists():
            record=json.loads(branding.regular(manifest))
            allowed={str(path):('original-user-%02d.bin'%i,asset) for i,(path,asset) in enumerate(branding.USER_LOGOS)}
            if record.get('schema')!=1 or not isinstance(record.get('files'),list):raise ValueError('Invalid splash manifest.')
            for item in record['files']:
                if (item.get('backup'),item.get('asset'))!=allowed.get(item.get('path')):raise ValueError('Unexpected splash path.')
                raw=branding.regular(store/item['backup'])
                if branding.sha256(raw)!=item['original_sha256']:raise ValueError('Original splash backup changed.')
        elif restore:
            raise ValueError('No original splash backups available.')
        else:
            contents=branding.assets_content(assets)
            files=branding.user_files(root,contents)
            if not files:raise ValueError('No known userspace splash files on this firmware.')
            for item in files:atomic(store/item['backup'],item['original'])
            record=dict(schema=1,state='prepared',files=[{k:v for k,v in item.items() if k not in ('original','installed')} for item in files])
            atomic(manifest,(json.dumps(record,sort_keys=True)+'\n').encode())
        contents={} if restore else branding.assets_content(assets)
        prepared=[]
        for item in record['files']:
            path=location(root,Path(item['path']));current=branding.regular(path)
            original=branding.regular(store/item['backup'])
            target=original if restore else contents[item['asset']]
            if not restore and branding.sha256(target)!=item['installed_sha256']:raise ValueError('Restore old artwork before changing branding assets.')
            if branding.sha256(current) not in (item['original_sha256'],item['installed_sha256']):raise ValueError('Splash changed outside RGDS: '+str(path))
            prepared.append((path,current,target,item['mode']))
        written=[]
        try:
            for path,current,target,mode in prepared:
                if current!=target:
                    written.append((path,current,target,mode));branding.replace_user(path,target,mode,expected=current)
            record['state']='restored' if restore else 'installed'
            atomic(manifest,(json.dumps(record,sort_keys=True)+'\n').encode())
        except BaseException:
            for path,original,target,mode in reversed(written):
                current=branding.regular(path)
                if current==target:branding.replace_user(path,original,mode,expected=target)
            raise
        return dict(state=record['state'],files=len(record['files']),backup=str(store),boot_partition_written=False,
                    scope='userspace_splash_files', early_boot_logo_changed=False, visual_boot_verified=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('inspect','apply','restore'))
    parser.add_argument('--assets',type=Path,default=Path(__file__).resolve().parents[1]/'assets/boot')
    args=parser.parse_args()
    print(json.dumps(inspect() if args.action=='inspect' else install(args.assets,restore=args.action=='restore'),indent=2))

if __name__=='__main__':main()
