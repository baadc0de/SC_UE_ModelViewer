#!/usr/bin/env bash
# GPU review helper: import a package in the -game host, screenshot named views
# (niri Wayland), stop UE by PID. Usage: capture_views.sh <package> <tag> [lit|lumen|nanite ...]
# Screenshots land in out/smoke/<tag>-<view>.png; import log lines are echoed. SC_ARGS adds host
# arguments (e.g. SC_ARGS="-SCView=-40,35,1.3"); SC_SETTLE / SC_TIMEOUT tune the waits.
set -u
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$SC_REPO" || exit 1
PKG="${1:-out/dec-r1}"; TAG="${2:-shot}"; shift 2 2>/dev/null || true
VIEWS=("$@"); [ ${#VIEWS[@]} -eq 0 ] && VIEWS=(lit lumen)
OUT=out/smoke; mkdir -p "$OUT"
LOG=ue/SCViewer/Saved/Import/import.log
SETTLE=${SC_SETTLE:-20}
TIMEOUT=${SC_TIMEOUT:-1800}
view_args() {
  case "$1" in
    lit) ;;
    lumen) echo '-ExecCmds=ShowFlag.VisualizeLumen 1, r.Lumen.Visualize 1' ;;
    nanite) echo '-ExecCmds=ShowFlag.VisualizeNanite 1, r.Nanite.Visualize Overview' ;;
    *) echo "-ExecCmds=$1" ;;
  esac
}
for view in "${VIEWS[@]}"; do
  rm -f "$LOG"
  args=$(view_args "$view")
  start=$(date +%s)
  if [ -n "$args" ]; then ue/scripts/run_import.sh "$PKG" -vulkan -SCStay ${SC_ARGS:-} "$args" >/dev/null 2>&1 &
  else ue/scripts/run_import.sh "$PKG" -vulkan -SCStay ${SC_ARGS:-} >/dev/null 2>&1 & fi
  pid=$! i=0
  while [ $i -lt "$TIMEOUT" ]; do
    if [ -f "$LOG" ] && grep -q -E "IMPORT_OK|IMPORT_FAILED" "$LOG"; then break; fi
    kill -0 $pid 2>/dev/null || { echo "$view: UE exited early"; grep -i -E "error|fatal" "$LOG" | head -5; break; }
    sleep 1; i=$((i+1))
  done
  echo "$view: import reached after $(( $(date +%s) - start )) s"
  # Wait for on-demand shader compilation (new/changed materials) to drain before the shot.
  idle=0; while [ $idle -lt 5 ]; do
    if pgrep ShaderCompile >/dev/null; then idle=0; else idle=$((idle+1)); fi; sleep 2; done
  echo "$view: shaders idle after $(( $(date +%s) - start )) s"
  sleep "$SETTLE"
  wid=$(niri msg windows | awk '/^Window ID/{id=$3} /App ID:|Title:/{ if (tolower($0) ~ /unreal|scviewer/) {print id; exit} }' | tr -d ':')
  if [ -n "$wid" ]; then
    niri msg action focus-window --id "$wid" >/dev/null 2>&1; sleep 1
    niri msg action screenshot-window --id "$wid" --write-to-disk true >/dev/null 2>&1; sleep 2
    cp "$(ls -t ~/Pictures/Screenshots/*.png | head -1)" "$OUT/$TAG-$view.png" && echo "$view: $OUT/$TAG-$view.png"
  else echo "$view: no UE window"; fi
  grep -E "IMPORT_OK|IMPORT_FAILED|MATERIALS |TEXTURES |MATERIAL_WARNING|BOUNDS_CM" "$LOG" | sed 's/^.*\]\[ *[0-9]*\]//' | cut -c1-300 | tail -12
  kill "$pid" 2>/dev/null; sleep 6; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; sleep 2
done
echo ALL_DONE
