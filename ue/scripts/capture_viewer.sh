#!/usr/bin/env bash
# WP-V1 GPU review helper for the viewer app (UI included): launch the Editor -game host with extra arguments,
# wait for a log pattern, let shaders settle, screenshot the window (niri), stop UE by PID.
# Usage: capture_viewer.sh <tag> <log-regex> [host args...]   e.g.
#   capture_viewer.sh gladius 'VIEWER_LOAD result' -SCDefaults -SCLoad=AEGS_Gladius
# Screenshot: out/smoke/<tag>.png; log: ue/SCViewer/Saved/Viewer/<tag>.log. SC_SETTLE (s) waits after the pattern.
set -u
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$SC_REPO" || exit 1
TAG=$1; PATTERN=$2; shift 2
SC_ENGINE=${UE_ENGINE_ROOT:-"$HOME/UnrealEngine/5.8"}
OUT=out/smoke; LOGDIR=ue/SCViewer/Saved/Viewer; mkdir -p "$OUT" "$LOGDIR"
LOG="$LOGDIR/$TAG.log"; rm -f "$LOG"
"$SC_ENGINE/Engine/Binaries/Linux/UnrealEditor" "$SC_REPO/ue/SCViewer/SCViewer.uproject" -game -windowed \
  -resx=1600 -resy=900 -vulkan -ddc=SCViewerDDC -nosplash -nop4 -abslog="$SC_REPO/$LOG" "$@" >/dev/null 2>&1 &
pid=$! start=$(date +%s)
for _ in $(seq 1 ${SC_TIMEOUT:-900}); do
  [ -f "$LOG" ] && grep -q -E "$PATTERN" "$LOG" && break
  kill -0 $pid 2>/dev/null || { echo "$TAG: UE exited early"; break; }
  sleep 1
done
echo "$TAG: pattern after $(( $(date +%s) - start )) s"
idle=0; while [ $idle -lt 4 ]; do if pgrep ShaderCompile >/dev/null; then idle=0; else idle=$((idle+1)); fi; sleep 2; done
sleep "${SC_SETTLE:-8}"
# niri reports a proxy PID from this environment, so match the UE window by app id / title like capture_views.sh.
wid=$(niri msg windows | awk '/^Window ID/{id=$3} /App ID:|Title:/{ l=tolower($0); if (l ~ /unrealeditor|scviewer/ && l !~ /crash/) {print id; exit} }' | tr -d ':')
if [ -n "$wid" ]; then
  niri msg action screenshot-window --id "$wid" --write-to-disk true >/dev/null 2>&1; sleep 2
  cp "$(ls -t ~/Pictures/Screenshots/*.png | head -1)" "$OUT/$TAG.png" && echo "$TAG: $OUT/$TAG.png"
else echo "$TAG: no UE window"; fi
grep -E "VIEWER_|FFI_EXPORT|AUTOMATION" "$LOG" | sed 's/^.*\]\[ *[0-9]*\]//' | cut -c1-400
kill "$pid" 2>/dev/null; sleep 5; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
