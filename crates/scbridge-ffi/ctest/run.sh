#!/usr/bin/env bash
# Explicit integration step: consumes prebuilt artifacts; never starts Cargo.
set -euo pipefail
cd "$(dirname "$0")/../../.."
probe="${SCBRIDGE_CTEST_DISCOVER:-$PWD/target/debug/examples/discover}"
if [[ ! -x "$probe" ]]; then
    echo 'Build the discovery probe and cdylib first: scripts/build_ffi.sh' >&2
    exit 1
fi
if archive=$("$probe"); then
    :
else
    status=$?
    if [[ "$status" == 77 ]]; then exit 0; fi
    exit "$status"
fi
library="${SCBRIDGE_CTEST_LIB:-$PWD/target/release/libscbridge.so}"
cli="${SCBRIDGE_CTEST_CLI:-$PWD/target/release/scviewer}"
[[ -f "$library" && -x "$cli" ]] || {
    echo 'Missing artifacts: run scripts/build_ffi.sh and CARGO_TARGET_DIR=./target cargo build --release -p scviewer-cli' >&2
    exit 1
}
mkdir -p out cache
work=$(mktemp -d "$PWD/out/wp-r4-ctest.XXXXXX")
cache=""
cleanup() {
    if [[ -n "$cache" ]]; then rm -rf "$cache"; fi
    if [[ "${SCBRIDGE_CTEST_KEEP:-0}" == 1 ]]; then
        echo "C test artifacts retained: $work"
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT
cache=$(mktemp -d "$PWD/cache/wp-r4-ctest.XXXXXX")
cc -std=c11 -Wall -Wextra -Werror -I crates/scbridge-ffi/include \
    crates/scbridge-ffi/ctest/smoke.c "$library" -Wl,-rpath,"$(dirname "$library")" -o "$work/smoke"
"$work/smoke" "$work" "$cache" smoke "$archive"
mkdir "$work/extracted"
tar -xf "$work/ffi.tar" -C "$work/extracted"
"$cli" validate "$work/extracted"
"$cli" --p4k "$archive" --cache "$cache" decomposed-tar AEGS_Gladius "$work/cli.tar"
cmp "$work/ffi.tar" "$work/cli.tar"
echo 'FFI / CLI decomposed-tar (same cached package): byte-identical'
python3 -I crates/scbridge-ffi/ctest/verify.py "$work"
"$work/smoke" "$work" "$cache" evict "$archive"
