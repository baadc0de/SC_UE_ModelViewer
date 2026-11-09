#!/usr/bin/env bash
# No build.rs: cargo integration test invokes this C compiler + live verifier.
set -euo pipefail
cd "$(dirname "$0")/../../.."
export CARGO_TARGET_DIR=./target
if [[ -z "${SCBRIDGE_CTEST_LIB:-}" ]]; then
    cargo build --locked -p scbridge-ffi
fi
library="${SCBRIDGE_CTEST_LIB:-$PWD/target/debug/libscbridge.so}"
if [[ -z "${SCBRIDGE_CTEST_CLI:-}" ]]; then
    cargo build --locked -p scviewer-cli
fi
cli="${SCBRIDGE_CTEST_CLI:-$PWD/target/debug/scviewer}"
mkdir -p out cache
work=$(mktemp -d "$PWD/out/wp-r4-ctest.XXXXXX")
cache=$(mktemp -d "$PWD/cache/wp-r4-ctest.XXXXXX")
trap 'rm -rf "$cache"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I crates/scbridge-ffi/include \
    crates/scbridge-ffi/ctest/smoke.c "$library" -Wl,-rpath,"$(dirname "$library")" -o "$work/smoke"
"$work/smoke" "$work" "$cache" smoke
mkdir "$work/extracted"
tar -xf "$work/ffi.tar" -C "$work/extracted"
"$cli" validate "$work/extracted"
"$cli" --cache "$cache" decomposed-tar AEGS_Gladius "$work/cli.tar"
cmp "$work/ffi.tar" "$work/cli.tar"
echo 'FFI / CLI decomposed-tar (same cached package): byte-identical'
python3 -I crates/scbridge-ffi/ctest/verify.py "$work"
"$work/smoke" "$work" "$cache" evict
echo "C test artifacts: $work"
