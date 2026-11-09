#!/usr/bin/env bash
# Build and stage the Linux FFI library/header. Default: debug (development).
# Use --release for shipping, or SC_FFI_PROFILE=release; --check-header does not build the library.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_TARGET_DIR=./target
ffi_profile=${SC_FFI_PROFILE:-debug}
check_header=false
for arg in "$@"; do
    case "$arg" in
        --release) ffi_profile=release ;;
        --check-header) check_header=true ;;
        *) echo "Usage: $0 [--release] [--check-header] (SC_FFI_PROFILE=debug|release)" >&2; exit 2 ;;
    esac
done
case "$ffi_profile" in
    debug) profile_args=() ;;
    release) profile_args=(--release) ;;
    *) echo "SC_FFI_PROFILE must be debug or release" >&2; exit 2 ;;
esac
ffi_target="target/$ffi_profile"
if $check_header; then
    cargo run --locked "${profile_args[@]}" -p scbridge-ffi --example header -- --check
    cmp crates/scbridge-ffi/include/scbridge.h ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib/include/scbridge.h
    exit
fi
echo "Building SCBridge FFI profile: $ffi_profile"
cargo run --locked "${profile_args[@]}" -p scbridge-ffi --example header
cargo build --locked "${profile_args[@]}" -p scbridge-ffi --example discover
# Build consumers together: a stale CLI embeds a different exporter/cache identity.
cargo build --locked "${profile_args[@]}" -p scbridge-ffi -p scviewer-cli
third_party=ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib
install -Dm644 crates/scbridge-ffi/include/scbridge.h "$third_party/include/scbridge.h"
install -Dm755 "$ffi_target/libscbridge.so" "$third_party/lib/Linux/libscbridge.so"
# UBT copies the library into the project binaries only when it builds; refresh a stale copy
# so a game launch without a rebuild picks up the new exporter (and its cache identity).
for bin in ue/SCViewer/Binaries/Linux ue/Plugins/SCBridge/Binaries/Linux; do
    [[ -f "$bin/libscbridge.so" ]] && install -m755 "$ffi_target/libscbridge.so" "$bin/libscbridge.so"
done
stat -c '%n: %s bytes' "$ffi_target/libscbridge.so"
nm -D --defined-only "$ffi_target/libscbridge.so" | awk '$2 == "T" { print; if ($3 !~ /^sc_/ && $3 != "scbridge_export_player") bad=1 } END { exit bad }'
