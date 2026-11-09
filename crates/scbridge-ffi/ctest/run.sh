#!/usr/bin/env bash
# Explicit integration step: consumes prebuilt artifacts; never starts Cargo.
set -euo pipefail
# Explicit status checks also work when sourced from an if/|| context, where
# Bash disables errexit even after `set -e`. A caller piping to tail still needs
# pipefail in its own shell; the final marker makes failures visible regardless.
run() {
    if "$@"; then return 0; else
        local status=$?
        printf 'C test step failed (%s): %s\n' "$status" "$*" >&2
        exit "$status"
    fi
}
run cd "$(dirname "$0")/../../.."
ffi_profile=${SC_FFI_PROFILE:-debug}
[[ "$ffi_profile" == debug || "$ffi_profile" == release ]] || {
    echo 'SC_FFI_PROFILE must be debug or release' >&2; exit 2;
}
probe="${SCBRIDGE_CTEST_DISCOVER:-$PWD/target/$ffi_profile/examples/discover}"
if [[ ! -x "$probe" ]]; then
    echo 'Build the discovery probe and cdylib first: scripts/build_ffi.sh' >&2
    exit 1
fi
if archive=$("$probe"); then :; else
    status=$?
    if [[ "$status" == 77 ]]; then exit 0; fi
    exit "$status"
fi
library="${SCBRIDGE_CTEST_LIB:-$PWD/target/$ffi_profile/libscbridge.so}"
cli="${SCBRIDGE_CTEST_CLI:-$PWD/target/$ffi_profile/scviewer}"
[[ -f "$library" && -x "$cli" ]] || {
    echo 'Missing artifacts: run scripts/build_ffi.sh (builds both library and CLI)' >&2
    exit 1
}
run mkdir -p out cache
work="" cache=""
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$cache" ]]; then rm -rf "$cache" || status=1; fi
    if [[ -n "$work" ]]; then
        if [[ "${SCBRIDGE_CTEST_KEEP:-0}" == 1 ]]; then
            echo "C test artifacts retained: $work"
        else
            rm -rf "$work" || status=1
        fi
    fi
    if [[ "$status" == 0 ]]; then echo 'C test: PASS';
    else echo "C test: FAIL (exit $status)" >&2; fi
    exit "$status"
}
trap cleanup EXIT
work=$(mktemp -d "$PWD/out/wp-r4-ctest.XXXXXX") || exit 1
cache=$(mktemp -d "$PWD/cache/wp-r4-ctest.XXXXXX") || exit 1
run cc -std=c11 -Wall -Wextra -Werror -I crates/scbridge-ffi/include \
    crates/scbridge-ffi/ctest/smoke.c "$library" -Wl,-rpath,"$(dirname "$library")" -o "$work/smoke"
run cc -std=c11 -Wall -Wextra -Werror -I crates/scbridge-ffi/include \
    crates/scbridge-ffi/ctest/cancel.c "$library" -Wl,-rpath,"$(dirname "$library")" -o "$work/cancel"
run "$work/cancel" "$archive" "$cache"
# Both sides assemble independently with persistence disabled.
run "$work/smoke" "$work" - smoke "$archive"
run mkdir "$work/extracted"
run tar -xf "$work/ffi.tar" -C "$work/extracted"
run "$cli" validate "$work/extracted"
run "$cli" --p4k "$archive" --no-cache decomposed-tar AEGS_Gladius "$work/cli.tar"
run cmp "$work/ffi.tar" "$work/cli.tar"
echo 'FFI / CLI independent uncached decomposed-tar: byte-identical'
run python3 -I crates/scbridge-ffi/ctest/verify.py "$work"
run python3 -I crates/scbridge-ffi/ctest/verify_blender.py "$work/extracted"
run "$cli" --p4k "$archive" --no-cache decomposed-tar AEGS_Gladius "$work/cli-no-animations.tar" --no-animations
run cmp "$work/no-animations.tar" "$work/cli-no-animations.tar"
run python3 -I crates/scbridge-ffi/ctest/verify_no_animations.py "$work/no-animations.tar"
run mkdir "$work/no-animations"
run tar -xf "$work/no-animations.tar" -C "$work/no-animations"
run "$cli" validate "$work/no-animations"
# Separately guard options/key parity: a CLI default export must reuse the FFI entry.
run mkdir "$work/cached"
run "$work/smoke" "$work/cached" "$cache" bench "$archive"
if "$cli" --p4k "$archive" --cache "$cache" decomposed-tar AEGS_Gladius "$work/cached/cli.tar" 2> "$work/cache.log"; then
    :
else
    status=$?
    cat "$work/cache.log" >&2 || true
    exit "$status"
fi
run cat "$work/cache.log"
run rg -q '^cache: hit ' "$work/cache.log"
run cmp "$work/cached/ffi.tar" "$work/cached/cli.tar"
echo 'FFI defaults / CLI defaults: same cache key, CLI hit, byte-identical'
run "$work/smoke" "$work" "$cache" evict "$archive"
