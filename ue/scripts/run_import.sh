#!/usr/bin/env bash
# Run the chosen Editor -game host with a decomposed folder. Add -SCStay for visual review.
set -euo pipefail
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SC_ENGINE=${UE_ENGINE_ROOT:-"$HOME/UnrealEngine/5.9"}
SC_PACKAGE=$(realpath "${1:-$SC_REPO/out/dec}")
shift "$(( $# > 0 ? 1 : 0 ))"
mkdir -p "$SC_REPO/ue/SCViewer/Saved/Import"
exec "$SC_ENGINE/Engine/Binaries/Linux/UnrealEditor" "$SC_REPO/ue/SCViewer/SCViewer.uproject" \
    -game -windowed -resx=1600 -resy=900 -unattended -nosplash -nop4 \
    -ddc=SCViewerDDC -SCPackage="$SC_PACKAGE" \
    -abslog="$SC_REPO/ue/SCViewer/Saved/Import/import.log" "$@"
