#!/bin/sh
set -u
# Manual launch follows the configured installation even when startup is stock.
# The enabled marker controls automatic startup only.
RGDS_CONFIGURED=$(python3 -c '
import json
from pathlib import Path
try:
    control=Path("/userdata/rgds-boot")
    config=json.loads((control/"config.json").read_text())
    if not isinstance(config, dict): raise ValueError("Invalid configuration")
    root=Path(config.get("root", ""))
    if config.get("mode") in ("rgds", "stock") and root.is_absolute() and ".." not in root.parts and "\n" not in str(root) and (root/"main.sh").is_file():
        print(root)
except (OSError,ValueError,TypeError): pass
' 2>/dev/null) || RGDS_CONFIGURED=
if [ -n "$RGDS_CONFIGURED" ]; then
    exec sh "$RGDS_CONFIGURED/main.sh" "$@"
fi
# Stock can launch APPS from either card. Prefer the package beside this entry,
# so a second card cannot silently select an older development installation.
RGDS_APPS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
for RGDS_ROOT in "$RGDS_APPS/../../rgds" /mnt/mmc/rgds /mnt/SDCARD/rgds; do
    [ -f "$RGDS_ROOT/main.sh" ] || continue
    exec sh "$RGDS_ROOT/main.sh" "$@"
done
echo "Copy the complete RGDS test package next to Roms/ first."
exit 1
