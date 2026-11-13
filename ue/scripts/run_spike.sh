#!/usr/bin/env bash
# Preserve the caller's working directory for the D10 cache before UE changes cwd.
set -euo pipefail
SC_LAUNCH_DIR=$PWD
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SC_ENGINE=${UE_ENGINE_ROOT:-"$HOME/UnrealEngine/5.9"}
SC_MODE=${1:-build}
shift "$(( $# > 0 ? 1 : 0 ))"
case "$SC_MODE" in build|save|load|save-raw|load-raw) ;; *) echo "Invalid spike mode: $SC_MODE" >&2; exit 2 ;; esac
mkdir -p "$SC_LAUNCH_DIR/cache/spike" "$SC_REPO/ue/SCViewer/Saved/Spikes"
exec "$SC_ENGINE/Engine/Binaries/Linux/UnrealEditor" "$SC_REPO/ue/SCViewer/SCViewer.uproject" \
    -game -windowed -resx=1600 -resy=900 -vulkan -unattended -nosplash -nop4 \
    -ddc=SCViewerDDC -SCSpike="$SC_MODE" -SCSpikeCacheDir="$SC_LAUNCH_DIR/cache/spike" \
    -abslog="$SC_REPO/ue/SCViewer/Saved/Spikes/$SC_MODE.log" "$@"
