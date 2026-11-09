#!/usr/bin/env bash
# Build (debug, cached) and run the vendored StarBreaker CLI.
# Usage: scripts/starbreaker.sh entity export Gladius out/gladius.glb --lod 1 --mip 2
# The CLI auto-discovers Data.p4k under ~/Games/star-citizen/... ; do NOT set SC_DATA_P4K.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SB_DIR="$ROOT/vendor/StarBreaker"
# Shared target dir so the vendored CLI and our own crates reuse one build cache.
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$ROOT/target}"
cargo build -q --manifest-path "$SB_DIR/Cargo.toml" -p starbreaker
exec "$CARGO_TARGET_DIR/debug/starbreaker" "$@"
