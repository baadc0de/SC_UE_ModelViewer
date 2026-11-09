#!/usr/bin/env bash
# WP-U3b many-light measurement helper: import a package in the -game host on the GPU,
# take one window screenshot, sample the steady-state GPU time from a CSV profile, then stop
# UE by PID. Usage:
#   measure_lights.sh <package> <tag> [extra host args, e.g. -SCView=180,2,0.25]
#   measure_lights.sh --analyse <csv> [sample seconds]     (re-analyse a saved CSV)
# Env: SC_SP (r.ScreenPercentage, default 100), SC_EXEC (comma-separated console commands
# appended to "stat unit"), SC_SETTLE (s after shaders go idle, default 20), SC_SAMPLE (s of
# frames sampled after the screenshot, default 15), SC_OUT (screenshot/CSV dir).
# The boot CSV capture streams rows until UE is stopped (SIGTERM leaves the file unfinalised,
# the last row may be cut); statistics use only the frames of the last SC_SAMPLE seconds.
set -u
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$SC_REPO" || exit 1

analyse() {
python3 -I - "$1" "$2" <<'EOF'
import csv, sys, statistics as st
csv.field_size_limit(10**9)
rows = list(csv.reader(open(sys.argv[1]))); head = rows[0]
data = [r for r in rows[1:] if len(r) >= len(head)]  # stats first seen later append columns; the last row may be cut
f = lambda r, i: float(r[i]) if r[i] else 0.0
fi = head.index("FrameTime")
# walk back from the end: skip ~1 s of shutdown, keep SAMPLE-2 s of frames
t = 0; end = len(data)
while end > 0 and t < 1000: end -= 1; t += f(data[end], fi)
t = 0; lo = end
while lo > 0 and t < (float(sys.argv[2]) - 2) * 1000: lo -= 1; t += f(data[lo], fi)
win = data[lo:end]
def stat(name):
    i = head.index(name); v = sorted(f(r, i) for r in win); return st.median(v), v[int(len(v) * 0.9)]
for name in ("GPUTime", "FrameTime", "RenderThreadTime", "GameThreadTime"):
    if name in head: m, p = stat(name); print(f"{name}: median {m:.2f} ms  p90 {p:.2f} ms  frames {len(win)}")
gpu = sorted(((stat(h)[0], h) for h in head if h.startswith("GPU/")), reverse=True)
print("top GPU passes: " + ", ".join(f"{h[4:]} {m:.2f}" for m, h in gpu[:12]))
for h in ("LightCount/All", "LightCount/UpdatedShadowMaps", "LightCount/Batched", "LightCount/Unbatched"):
    if h in head: print(f"{h}: {stat(h)[0]:.0f}")
EOF
}
if [ "${1:-}" = "--analyse" ]; then analyse "$2" "${3:-15}"; exit; fi

PKG="$1"; TAG="$2"; shift 2
OUT=${SC_OUT:-ue/SCViewer/Saved/Import/wp-u3b}; mkdir -p "$OUT"
LOG=ue/SCViewer/Saved/Import/import.log
CSVDIR="$HOME/.config/Epic/UnrealEngine/5.8/Saved/Profiling/CSV"  # boot captures use the engine user dir
SETTLE=${SC_SETTLE:-20}; SAMPLE=${SC_SAMPLE:-15}
EXEC="stat unit, r.ScreenPercentage ${SC_SP:-100}, r.DynamicRes.OperationMode 0${SC_EXEC:+, $SC_EXEC}"
rm -f "$LOG"; before=$(ls "$CSVDIR" 2>/dev/null | wc -l)
ue/scripts/run_import.sh "$PKG" -vulkan -SCStay -csvGpuStats -csvCaptureFrames=1000000 \
    "-ExecCmds=$EXEC" "$@" >/dev/null 2>&1 &
pid=$!; start=$(date +%s)
until [ -f "$LOG" ] && grep -q -E "IMPORT_OK|IMPORT_FAILED" "$LOG"; do
  kill -0 $pid 2>/dev/null || { echo "$TAG: UE exited early"; exit 1; }
  [ $(( $(date +%s) - start )) -gt 1800 ] && { echo "$TAG: timeout"; kill $pid; exit 1; }
  sleep 1
done
idle=0; while [ $idle -lt 5 ]; do if pgrep ShaderCompile >/dev/null; then idle=0; else idle=$((idle+1)); fi; sleep 2; done
sleep "$SETTLE"
wid=$(niri msg windows | awk '/^Window ID/{id=$3} /App ID:|Title:/{ if (tolower($0) ~ /unreal|scviewer/) {print id; exit} }' | tr -d ':')
if [ -n "$wid" ]; then
  niri msg action screenshot-window --id "$wid" --write-to-disk true >/dev/null 2>&1; sleep 2
  cp "$(ls -t ~/Pictures/Screenshots/*.png | head -1)" "$OUT/$TAG.png" && echo "$TAG: $OUT/$TAG.png"
fi
sleep "$SAMPLE"
grep -E "IMPORT_OK" "$LOG" | sed 's/^.*IMPORT_OK/IMPORT_OK/' | cut -c1-80
kill "$pid" 2>/dev/null
i=0; while [ $i -lt 60 ] && kill -0 "$pid" 2>/dev/null; do sleep 1; i=$((i+1)); done
kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ "$(ls "$CSVDIR" 2>/dev/null | wc -l)" -gt "$before" ] || { echo "$TAG: no CSV written"; exit 1; }
cp "$(ls -t "$CSVDIR"/*.csv | head -1)" "$OUT/$TAG.csv" && analyse "$OUT/$TAG.csv" "$SAMPLE"
echo DONE
