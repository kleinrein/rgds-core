#!/bin/sh
# Remove RG DS Core from this card and the stock system. Saves, settings and
# rgds/games stay; run Install RG DS Core again to get the app back.
set -u
RGDS_APPS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
RGDS_CARD=$(CDPATH= cd -- "$RGDS_APPS/../.." && pwd) || exit 1
RGDS_ROOT="$RGDS_CARD/rgds"
RGDS_OK=1
# The release's own uninstaller puts the stock autostart hook, trim service,
# shutdown step and boot artwork back, then removes the application files.
RGDS_BUILD=$(cat "$RGDS_ROOT/state/active" 2>/dev/null) || RGDS_BUILD=
case "$RGDS_BUILD" in ''|*[!a-z0-9.-]*|.*) RGDS_BUILD=;; esac
RGDS_UNINSTALL="$RGDS_ROOT/releases/$RGDS_BUILD/runtime/uninstall.py"
if [ -n "$RGDS_BUILD" ] && [ -f "$RGDS_UNINSTALL" ] && command -v python3 >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    if python3 -B "$RGDS_UNINSTALL" --root "$RGDS_ROOT" ${RGDS_SYSTEM_ROOT:+--system-root "$RGDS_SYSTEM_ROOT"} > "$RGDS_CARD/rgds-uninstall.log" 2>&1; then
        echo "RG DS Core removed. Saves and settings stay in rgds/userdata."
    else
        RGDS_OK=0
        echo "The release's uninstaller failed; see rgds-uninstall.log on the card." >&2
    fi
else
    RGDS_OK=0
    echo "No usable RG DS Core release found; removing the application files only." >&2
fi
if [ "$RGDS_OK" = 0 ]; then
    # Without the marker the stock autostart hook exits before touching anything,
    # and without releases there is nothing for it to start.
    rm -f -- /userdata/rgds-boot/enabled
    if [ -d "$RGDS_ROOT" ]; then
        for RGDS_ITEM in releases state inbox docs __pycache__; do rm -rf -- "$RGDS_ROOT/$RGDS_ITEM"; done
        rm -f -- "$RGDS_ROOT/main.sh" "$RGDS_ROOT/inventory.sh" "$RGDS_ROOT/update_guard.py"
        echo "Application files removed. Saves and settings stay in rgds/userdata."
    fi
fi
# Menu entries on both cards; this entry goes last.
for RGDS_MENU in "$RGDS_APPS" /mnt/mmc/Roms/APPS /mnt/SDCARD/Roms/APPS; do
    [ -d "$RGDS_MENU" ] || continue
    for RGDS_ENTRY in "RG DS Core.sh" "Try RGDS.sh" "RGDS Probe.sh" "RGDS Inventory.sh" "RGDS Recovery.sh" "Install RG DS Core.sh"; do
        rm -f -- "$RGDS_MENU/$RGDS_ENTRY"
    done
done
for RGDS_MENU in /mnt/mmc/Roms/APPS /mnt/SDCARD/Roms/APPS "$RGDS_APPS"; do
    rm -f -- "$RGDS_MENU/Uninstall RG DS Core.sh"
done
echo "The RGDS entries have been removed from APPS."
exit 0
