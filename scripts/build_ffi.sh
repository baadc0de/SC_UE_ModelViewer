#!/usr/bin/env bash
# Build the Linux deployment library and generate/check its checked-in C header.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_TARGET_DIR=./target
if [[ "${1:-}" == --check-header ]]; then
    cargo run --locked -p scbridge-ffi --example header -- --check
    cmp crates/scbridge-ffi/include/scbridge.h ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib/include/scbridge.h
    exit
fi
cargo run --locked -p scbridge-ffi --example header
cargo build --locked -p scbridge-ffi --example discover
# Build consumers together: a stale CLI embeds a different exporter/cache identity.
cargo build --locked --release -p scbridge-ffi -p scviewer-cli
third_party=ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib
install -Dm644 crates/scbridge-ffi/include/scbridge.h "$third_party/include/scbridge.h"
install -Dm755 target/release/libscbridge.so "$third_party/lib/Linux/libscbridge.so"
stat -c '%n: %s bytes' target/release/libscbridge.so
nm -D --defined-only target/release/libscbridge.so | awk '$2 == "T" { print; if ($3 !~ /^sc_/) bad=1 } END { exit bad }'
