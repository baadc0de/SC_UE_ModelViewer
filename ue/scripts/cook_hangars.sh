#!/usr/bin/env bash
# WP-C1: cook the exported hangars one editor process per hangar (memory is released between hangars; a single
# commandlet grew to ~26 GB over several hangars and was OOM-killed), each in its own memory-capped systemd scope so an
# overrun kills only that cook. Already-cooked hangars are skipped (see cook_hangars.py); SC_COOK_MEASURE=1 instead
# records the ship capacity of already-cooked hangars in the hangar index (and cooks nothing).
# Usage: ue/scripts/cook_hangars.sh [export root, default out/hangars] [hangar names...]
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXPORT="$(realpath "${1:-$ROOT_DIR/out/hangars}")"; shift || true
ENGINE="${UE_ENGINE_ROOT:-$HOME/UnrealEngine/5.9}"
LOGS="$ROOT_DIR/ue/SCViewer/Saved/Cook"; mkdir -p "$LOGS"
NAMES=("$@")
if [ ${#NAMES[@]} -eq 0 ]; then
  for P in "$EXPORT"/Packages/*/; do
    N=$(basename "$P" | sed -E 's/^(common)?hangar([a-z]+) ahangar ([a-z]+) ([0-9]+).*/\2_a_hangar_\3_\4/')
    NAMES+=("$N")
  done
fi
for N in "${NAMES[@]}"; do
  MEASURE=()
  if [ "${SC_COOK_MEASURE:-0}" = 1 ]; then MEASURE=(-SCCookMeasure)
  elif [ -f "$ROOT_DIR/ue/SCViewer/Content/SC/Hangars/Maps/$N.umap" ]; then echo "SKIP $N (cooked)"; continue; fi
  start=$(date +%s)
  LOG="$LOGS/$N${MEASURE:+-measure}.log"
  systemd-run --user --scope -q -p MemoryMax="${SC_COOK_MEMORY:-30G}" \
    "$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$ROOT_DIR/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
    -script="$ROOT_DIR/ue/scripts/cook_hangars.py" -vulkan -AllowCommandletRendering -unattended -nop4 -ddc=SCViewerDDC \
    -SCCookRoot="$EXPORT" -SCCookOnly="$N" "${MEASURE[@]}" -abslog="$LOG" >/dev/null 2>&1
  status=$?
  line=$(grep -h 'COOK_HANGAR' "$LOG" | sed 's/^.*COOK_HANGAR //' | tail -1)
  echo "COOKED $N status=$status seconds=$(( $(date +%s) - start )) ${line:-no result (see $LOG)}"
done
