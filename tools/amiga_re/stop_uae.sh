#!/bin/bash
# Stop the FS-UAE driver and the emulator by exact PID (never pkill -f, which
# would match this script's own command line).
cd "$(dirname "$0")" || exit 1
if [ -f amiga_re/uae.pid ]; then
    DPID=$(cat amiga_re/uae.pid)
    [ -n "$DPID" ] && kill -9 "$DPID" 2>/dev/null
    rm -f amiga_re/uae.pid
fi
pkill -9 -x fs-uae 2>/dev/null
pkill -9 -x fs-uae-launcher 2>/dev/null
echo "stopped"