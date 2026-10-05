#!/bin/sh
# Temporary stock-APPS launcher. No autostart, init changes, or boot-device writes.
set -eu
export PYTHONDONTWRITEBYTECODE=1
RGDS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$RGDS_ROOT/userdata/boot_stock" ]; then
    echo "RGDS test launch disabled by userdata/boot_stock. Stock remains available."
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "RGDS needs Python 3.8+ on the stock card. Run RGDS Inventory and read its log."
    exit 1
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' || {
    echo "RGDS needs Python 3.8+. Keep stock active and share the inventory."
    exit 1
}
RGDS_BUILD=$(cat "$RGDS_ROOT/state/active")
case "$RGDS_BUILD" in
    ''|*[!a-z0-9.-]*|.*) echo "RGDS active build ID is invalid"; exit 1;;
esac
exec python3 -B "$RGDS_ROOT/releases/$RGDS_BUILD/runtime/session.py" --root "$RGDS_ROOT" "$@"
