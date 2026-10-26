#!/bin/bash
# The PortMaster entry in RG DS Core's Ports. The launcher starts it through
# runtime/emulators.py (engine "portmaster"), which installs the latest
# PortMaster on first use and opens it; this script does the same by hand.
here=$(cd -- "$(dirname -- "$0")" && pwd)
exec python3 -B "$here/../../portmaster.py" open --app "$here/../../.."
