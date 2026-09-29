#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -eq 0 ]]; then
    echo "Run this script as your desktop user, not with sudo; it requests sudo itself." >&2
    exit 2
fi
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: bash $0 CAPTURE [INSPECTRUM_BINARY]" >&2
    exit 2
fi
debug_tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
debug_binary=$(realpath -e -- "${2:-$debug_tools_dir/../build/src/inspectrum}")
debug_capture=$(realpath -e -- "$1")
debug_output_dir=$(mktemp -d /tmp/inspectrum-memory.XXXXXX)
debug_unit="inspectrum-memory-$(basename "$debug_output_dir" | cut -d. -f2)"
echo "Unit: $debug_unit"
echo "Log: $debug_output_dir/trace.log"
sudo systemd-run --unit="$debug_unit" --uid="$(id -u)" --gid="$(id -g)" \
    --working-directory="$(dirname -- "$debug_capture")" \
    -p MemoryMax=4G -p MemorySwapMax=0 -p OOMPolicy=kill -p Restart=no -p LimitCORE=0 \
    --setenv="DISPLAY=${DISPLAY:?Run from your desktop terminal}" \
    --setenv="XAUTHORITY=${XAUTHORITY:-/run/user/$(id -u)/gdm/Xauthority}" \
    --setenv="XDG_RUNTIME_DIR=/run/user/$(id -u)" \
    --setenv="DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus" \
    /usr/bin/python3 "$debug_tools_dir/debug_memory.py" \
    --log "$debug_output_dir/trace.log" "$debug_binary" "$debug_capture"
