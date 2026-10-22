#!/usr/bin/env python3
"""Configure this project's ScreenScraper application credentials locally or over SSH."""
import argparse
import getpass
import json
from pathlib import Path
import shlex
import subprocess
import sys

import device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device")
    parser.add_argument("--sd-root", type=Path, default=Path("/mnt/mmc"))
    parser.add_argument("--data", type=Path, default=Path("build/dev"))
    args = parser.parse_args()
    config = {
        "devid": input("ScreenScraper project developer ID: "),
        "devpassword": getpass.getpass("ScreenScraper project developer password: ")
    }
    if args.device:
        if not args.sd_root.is_absolute() or ".." in args.sd_root.parts:
            raise ValueError("Invalid SD mount")
        root = args.sd_root / "rgds"
        command = "RGDS_ID=$(cat " + shlex.quote(
            str(root / "state/active")
        ) + "); case \"$RGDS_ID\" in ''|*[!a-z0-9.-]*|.*) exit 1;; esac; python3 -B "
        command += shlex.quote(
            str(root / "releases")
        ) + '/"$RGDS_ID"/runtime/scraper.py setup-app --stdin --data ' + shlex.quote(str(root / "userdata"))
        device.ssh(device.target(args.device), command, input=json.dumps(config), text=True, batch=True)
    else:
        subprocess.run(
            [
                sys.executable, "-B",
                str(device.ROOT / "runtime/scraper.py"), "setup-app", "--stdin", "--data",
                str(args.data)
            ],
            input=json.dumps(config),
            text=True,
            check=True
        )
    print("Project configured. Players can now sign in with their own ScreenScraper account in Library settings.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError):
        print("Could not configure ScreenScraper. Check the device connection and deployment.", file=sys.stderr)
        sys.exit(1)
