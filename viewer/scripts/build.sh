#!/usr/bin/env bash
# WP-U0: serial UBT invocation with scratch kept inside the project.
set -euo pipefail
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SC_ENGINE=${UE_ENGINE_ROOT:-"$HOME/UnrealEngine-5.9-src"}  # the UE 5.9 source fork (plan D17)
SC_TARGET=${1:-SCViewerEditor}
shift "$(( $# > 0 ? 1 : 0 ))"
mkdir -p "$SC_REPO/viewer/Saved/BuildConfig" "$SC_REPO/viewer/Saved/Spikes"
export XDG_CONFIG_HOME="$SC_REPO/viewer/Saved/BuildConfig"
exec "$SC_ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" "$SC_TARGET" Linux Development \
    -Project="$SC_REPO/viewer/SCViewer.uproject" -WaitMutex -NoHotReload \
    -NoUBA -NoUBALocal -UBARootDir="$SC_REPO/viewer/Saved/UBA" \
    -Log="$SC_REPO/viewer/Saved/Spikes/build-$SC_TARGET.log" "$@"
