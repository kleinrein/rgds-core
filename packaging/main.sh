#!/bin/sh
# Stock-APPS launcher. Startup routing is an explicit choice in System settings.
set -eu
export PYTHONDONTWRITEBYTECODE=1
RGDS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$RGDS_ROOT/userdata/boot_stock" ]; then
    echo "RGDS test launch disabled by userdata/boot_stock. Stock remains available."
    exit 0
fi
if ! mkdir -p "$RGDS_ROOT/userdata"; then
    echo "RGDS cannot write userdata. Check that the development card is writable." >&2
    exit 1
fi
RGDS_LOG="$RGDS_ROOT/userdata/launch.log"
if ! printf '\nRGDS launch: %s\n' "$RGDS_ROOT" >> "$RGDS_LOG"; then
    echo "RGDS cannot write $RGDS_LOG. Check the card's free space and permissions." >&2
    exit 1
fi
fail() {
    printf '%s\n' "$1" >&2
    printf '%s\n' "$1" >> "$RGDS_LOG"
    exit 1
}
if ! command -v python3 >/dev/null 2>&1; then
    fail "RGDS needs Python 3.8+ on the stock card."
fi
python3 -B -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' >> "$RGDS_LOG" 2>&1 || {
    fail "RGDS needs Python 3.8+. Keep stock active and share the inventory."
}
if [ -f "$RGDS_ROOT/update_guard.py" ]; then
    exec python3 -B "$RGDS_ROOT/update_guard.py" --root "$RGDS_ROOT" "$@" >> "$RGDS_LOG" 2>&1
fi
if ! RGDS_BUILD=$(cat "$RGDS_ROOT/state/active" 2>> "$RGDS_LOG"); then
    fail "RGDS has no readable active build. Copy the complete test package beside Roms/."
fi
case "$RGDS_BUILD" in
    ''|*[!a-z0-9.-]*|.*) fail "RGDS active build ID is invalid. Copy the complete test package.";;
esac
RGDS_SESSION="$RGDS_ROOT/releases/$RGDS_BUILD/runtime/session.py"
if [ ! -f "$RGDS_SESSION" ]; then
    fail "RGDS active release is incomplete. Copy the complete test package; do not copy only the APPS entry."
fi
# Capture import failures, display discovery errors and session-lock diagnostics
# that occur before session.py creates its separate launcher/session log.
exec python3 -B "$RGDS_SESSION" --root "$RGDS_ROOT" "$@" >> "$RGDS_LOG" 2>&1
