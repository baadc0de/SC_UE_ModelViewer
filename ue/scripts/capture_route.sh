#!/usr/bin/env bash
# WP-P1 GPU route helper: launch the Editor -game host (like capture_viewer.sh) with -SCScript=<route>,
# screenshot the window (niri) at every SCRIPT_SHOT marker, stop UE after SCRIPT_DONE.
# Usage: capture_route.sh <tag> <route-file> [host args...]   e.g.
#   capture_route.sh p1-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt \
#     -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
#   capture_route.sh p1-gladius ue/scripts/routes/hull-seat.txt -SCPackage="$PWD/out/dec-r5b-gladius" -SCExplore
# Route lines: "<delay seconds> <console command>" (see ue/README.md). Screenshots: out/smoke/<tag>-<shot>.png;
# log: ue/SCViewer/Saved/Viewer/<tag>.log. A route ending in "quit" exits by itself (exit status printed);
# otherwise UE is stopped by PID. Env: SC_TIMEOUT (s, whole run), SC_EXIT_WAIT (s after SCRIPT_DONE).
set -u
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$SC_REPO" || exit 1
TAG=$1; ROUTE=$(realpath "$2"); shift 2
SC_ENGINE=${UE_ENGINE_ROOT:-"$HOME/UnrealEngine/5.8"}
OUT=out/smoke; LOGDIR=ue/SCViewer/Saved/Viewer; SHOTS="$HOME/Pictures/Screenshots"; mkdir -p "$OUT" "$LOGDIR" "$SHOTS"
LOG="$LOGDIR/$TAG.log"; rm -f "$LOG"
"$SC_ENGINE/Engine/Binaries/Linux/UnrealEditor" "$SC_REPO/ue/SCViewer/SCViewer.uproject" -game -windowed \
  -resx=1600 -resy=900 -vulkan -ddc=SCViewerDDC -nosplash -nop4 -abslog="$SC_REPO/$LOG" -SCScript="$ROUTE" "$@" \
  >/dev/null 2>&1 &
pid=$! start=$(date +%s) taken=0 status=""
shot() { # $1 = shot tag; niri reports a proxy PID, so match the UE window by app id / title
  local wid before after
  wid=$(niri msg windows | awk '/^Window ID/{id=$3} /App ID:|Title:/{ l=tolower($0); if (l ~ /unrealeditor|scviewer/ && l !~ /crash/) {print id; exit} }' | tr -d ':')
  [ -n "$wid" ] || { echo "$TAG: no UE window for $1"; return; }
  before=$(ls -t "$SHOTS"/*.png 2>/dev/null | head -1)
  niri msg action screenshot-window --id "$wid" --write-to-disk true >/dev/null 2>&1
  for _ in 1 2 3 4 5 6 7 8 9 10; do after=$(ls -t "$SHOTS"/*.png 2>/dev/null | head -1); [ "$after" != "$before" ] && break; sleep 0.2; done
  if [ -n "$after" ] && [ "$after" != "$before" ]; then mv "$after" "$OUT/$TAG-$1.png" && echo "$TAG: $OUT/$TAG-$1.png"
  else echo "$TAG: screenshot failed for $1"; fi
}
while kill -0 $pid 2>/dev/null; do
  if [ -f "$LOG" ]; then
    mapfile -t tags < <(grep -o 'SCRIPT_SHOT tag=[^ ]*' "$LOG" | cut -d= -f2)
    while [ "$taken" -lt "${#tags[@]}" ]; do sleep 1; shot "${tags[$taken]}"; taken=$((taken+1)); done
    grep -q 'SCRIPT_DONE' "$LOG" && break
  fi
  [ $(( $(date +%s) - start )) -gt "${SC_TIMEOUT:-1200}" ] && { echo "$TAG: timeout"; break; }
  sleep 0.5
done
for _ in $(seq 1 "${SC_EXIT_WAIT:-30}"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
if kill -0 $pid 2>/dev/null; then
  kill "$pid" 2>/dev/null; sleep 5; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; status="stopped by PID"
else wait "$pid"; status="exit $?"; fi
echo "$TAG: $status after $(( $(date +%s) - start )) s"
pkill -f "CrashReportClient.*$TAG" 2>/dev/null && echo "$TAG: closed a crash reporter (UE crashed)"
grep -E "SCRIPT|FP_|FIRST_PERSON|VIEWER_LOAD|EXPLORATION |Ensure|Assertion|Fatal" "$LOG" | sed 's/^.*\]\[ *[0-9]*\]//' | cut -c1-400
