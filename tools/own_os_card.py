#!/usr/bin/env python3
"""Second-card helper for the own-OS work (docs/OWN_OS_PLAN.md), macOS only.

  disks                      list external disks and whether flash would accept them
  flash IMAGE[.gz] DISK      write an image to a spare microSD card (asks for sudo)
  logs [VOLUME] [--label X]  copy banana/logs from a test card into build/os/

flash refuses anything that is not an external, removable disk of at most 256 GB, and anything that looks like the
stock card (more than five partitions, or a volume with Roms/, rgds/ or MUOS/ on it): the stock card must never be
written by this work; --erase-stock-looking lifts only that check, for a spare that once held a stock image. It asks for the disk name to be typed back before writing.
"""
import argparse
from datetime import datetime
import plistlib
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 256 * 1000**3
MAX_PARTITIONS = 5
STOCK_MARKERS = ("Roms", "rgds", "MUOS", "muos")


def diskutil(verb, *args):
    return plistlib.loads(subprocess.run(["diskutil", verb, "-plist", *args], check=True, capture_output=True).stdout)


def describe(disk):
    info = diskutil("info", disk)
    listing = diskutil("list", disk)
    parts = [p for d in listing.get("AllDisksAndPartitions", []) for p in d.get("Partitions", [])]
    mounts = [Path(p["MountPoint"]) for p in parts if p.get("MountPoint")]
    problems = []
    if info.get("Internal", True):
        problems.append("internal disk")
    if not (info.get("RemovableMedia") or info.get("Ejectable") or info.get("Removable")):
        problems.append("not removable")
    if info.get("WholeDisk") is not True:
        problems.append("not a whole disk (pass diskN, not diskNsM)")
    if info.get("TotalSize", 0) > MAX_BYTES:
        problems.append("larger than 256 GB")
    stock = []
    if len(parts) > MAX_PARTITIONS:
        stock.append(f"{len(parts)} partitions: looks like the stock card")
    for mount in mounts:
        found = [m for m in STOCK_MARKERS if (mount / m).exists()]
        if found:
            stock.append(f"{mount} has {', '.join(found)}: looks like the stock card")
    return info, parts, problems, stock


def external_disks():
    listing = diskutil("list", "external", "physical")
    return listing.get("WholeDisks", [])


def cmd_disks(_):
    disks = external_disks()
    if not disks:
        print("No external disks. Put the spare microSD card in the reader.")
    for disk in disks:
        info, parts, problems, stock = describe(disk)
        problems += stock
        size = info.get("TotalSize", 0) / 1000**3
        verdict = "ok" if not problems else "refused: " + "; ".join(problems)
        print(f"/dev/{disk}  {size:.1f} GB  {info.get('MediaName', '?')}  {len(parts)} partitions  {verdict}")


def cmd_flash(args):
    image = Path(args.image).resolve()
    disk = args.disk.removeprefix("/dev/").removeprefix("r")
    if not image.is_file():
        sys.exit(f"{image}: no such image")
    info, parts, problems, stock = describe(disk)
    print(f"/dev/{disk}: {info.get('MediaName', '?')}, {info.get('TotalSize', 0) / 1000**3:.1f} GB, "
          f"{len(parts)} partitions ({', '.join(p.get('VolumeName') or p.get('Content', '?') for p in parts) or 'none'})")
    if stock and args.erase_stock_looking:
        print("Erasing anyway (--erase-stock-looking): " + "; ".join(stock))
    else:
        problems += stock
    if problems:
        sys.exit("Refusing: " + "; ".join(problems))
    if input(f"Everything on /dev/{disk} will be erased. Type {disk} to write {image.name}: ").strip() != disk:
        sys.exit("Not confirmed; nothing written.")
    subprocess.run(["diskutil", "unmountDisk", f"/dev/{disk}"], check=True)
    reader = f"gzip -dc {sh(image)}" if image.suffix == ".gz" else f"cat {sh(image)}"
    subprocess.run(["sh", "-c", f"{reader} | sudo dd of=/dev/r{disk} bs=4m status=progress && sync"], check=True)
    if args.keep_mounted:
        subprocess.run(["diskutil", "mountDisk", f"/dev/{disk}"], check=False)
        print("Written and mounted again (its boot partition can take setup files). Eject it before removing it.")
        return
    subprocess.run(["diskutil", "eject", f"/dev/{disk}"], check=False)
    print("Written and ejected. Swap it for the stock card only with the handheld powered off.")


def cmd_logs(args):
    volume = Path(args.volume) if args.volume else Path("/Volumes/BANANA")
    source = volume / "banana" / "logs"
    if not source.is_dir():
        sys.exit(f"{source}: no logs (is the test card in the reader and mounted?)")
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    target = ROOT / "build" / "os" / f"banana-{args.label}-{stamp}"
    shutil.copytree(source, target)
    for extra in ("panel-tune.txt", "bananad.conf"):
        if (volume / "banana" / extra).is_file():
            shutil.copy2(volume / "banana" / extra, target / extra)
    print(target)


def sh(path):
    return "'" + str(path).replace("'", "'\\''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("disks").set_defaults(run=cmd_disks)
    flash = sub.add_parser("flash")
    flash.add_argument("image")
    flash.add_argument("disk")
    flash.add_argument("--erase-stock-looking", action="store_true")
    flash.add_argument("--keep-mounted", action="store_true")
    flash.set_defaults(run=cmd_flash)
    logs = sub.add_parser("logs")
    logs.add_argument("volume", nargs="?")
    logs.add_argument("--label", default="test")
    logs.set_defaults(run=cmd_logs)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
