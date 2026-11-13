#!/usr/bin/env bash
# WP-U0: serial UBT invocation with scratch kept inside the project.
set -euo pipefail
SC_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SC_ENGINE=${UE_ENGINE_ROOT:-"$HOME/UnrealEngine/5.9"}
SC_TARGET=${1:-SCViewerEditor}
shift "$(( $# > 0 ? 1 : 0 ))"
mkdir -p "$SC_REPO/ue/SCViewer/Saved/BuildConfig" "$SC_REPO/ue/SCViewer/Saved/Spikes"
export XDG_CONFIG_HOME="$SC_REPO/ue/SCViewer/Saved/BuildConfig"
exec "$SC_ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" "$SC_TARGET" Linux Development \
    -Project="$SC_REPO/ue/SCViewer/SCViewer.uproject" -WaitMutex -NoHotReload \
    -NoUBA -NoUBALocal -UBARootDir="$SC_REPO/ue/SCViewer/Saved/UBA" \
    -Log="$SC_REPO/ue/SCViewer/Saved/Spikes/build-$SC_TARGET.log" "$@"
