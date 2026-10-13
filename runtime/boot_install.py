#!/usr/bin/env python3
"""Inspect, configure or remove the RGDS startup option; no boot-partition writes."""
import argparse
import json
from pathlib import Path
import sys
from boot_control import configure, disable, inspect, plan, uninstall


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('inspect','plan','enable','stock','uninstall'))
    parser.add_argument('--root',type=Path)
    parser.add_argument('--system-root',type=Path,default=Path('/'))
    parser.add_argument('--no-warning',action='store_true')
    args=parser.parse_args()
    try:
        if args.action=='inspect':result=inspect(args.root,args.system_root)
        elif args.action=='uninstall':result=uninstall(args.system_root)
        else:
            if not args.root:parser.error('--root is required for plan/enable/stock')
            mode='rgds' if args.action in ('plan','enable') else 'stock'
            result=plan(args.root,mode,args.system_root,not args.no_warning) if args.action=='plan' else configure(args.root,mode,args.system_root,not args.no_warning)
        print(json.dumps(result,indent=2,sort_keys=True));return 0
    except (OSError,ValueError) as error:
        print('RGDS startup: '+str(error),file=sys.stderr);return 1


if __name__=='__main__':sys.exit(main())
