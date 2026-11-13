#!/usr/bin/env bash
# WP-C1: export the player-hangar object containers for cooking (decomposed GLB + DDS, LOD 0, full-resolution
# textures; the DDS path the viewer imports, decoded to editable sources by the cook) into one root that shares Data/ (meshes and textures written once). Game-derived output: out/ only.
# Usage: scripts/export_hangars.sh [out/hangars]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/out/hangars}"
mkdir -p "$OUT"
# Patterns are raw p4k path substrings (backslash separators). The base hangar of each style and size (modern = hightech_a, utility = util_a); city/dressing variants are not cooked.
HANGARS=(
  hightech_a/hangar_smltop_001.socpak hightech_a/hangar_smlfrnt_001.socpak hightech_a/hangar_medtop_001.socpak
  hightech_a/hangar_medfrnt_001.socpak hightech_a/hangar_lrgtop_001.socpak hightech_a/hangar_lrgfront_001.socpak
  hightech_a/hangar_xltop_001.socpak
  util_a/hangar_smltop_001.socpak util_a/hangar_smlfrnt_001.socpak util_a/hangar_medtop_001.socpak
  util_a/hangar_medfrnt_001.socpak util_a/hangar_lrgtop_001.socpak util_a/hangar_lrgfrnt_001.socpak
  util_a/hangar_xltop_001.socpak
)
for H in "${HANGARS[@]}"; do
  start=$(date +%s)
  "$ROOT/scripts/starbreaker.sh" socpak export "hangar\\${H/\//\\}" "$OUT" --kind decomposed --format glb --dds \
    --lod 0 --mip 0 --skip-existing-assets > "$OUT/export-$(basename "$H" .socpak)-$(dirname "$H").log" 2>&1 \
    && echo "EXPORTED $H seconds=$(( $(date +%s) - start ))" || echo "FAILED $H (see log)"
done
