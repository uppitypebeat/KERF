#!/usr/bin/env bash
# Copies KERF into the user's plugin folders so Logic, Ableton, FL Studio (Mac), etc. find it.
# No administrator rights needed (~/Library is the user's own).
#
# Usage:
#   bash scripts/install-mac.sh                # from a build (build-mac/Kerf_artefacts/Release)
#   bash scripts/install-mac.sh ~/Downloads/KERF-0.7.0-macOS
#                                              # from an unpacked download (the folder with KERF.vst3)

set -euo pipefail
cd "$(dirname "$0")/.."

src="${1:-build-mac/Kerf_artefacts/Release}"
vst3=$(find "$src" -maxdepth 2 -name KERF.vst3 -type d | head -n 1)
au=$(find "$src" -maxdepth 2 -name KERF.component -type d | head -n 1)
app=$(find "$src" -maxdepth 2 -name KERF.app -type d | head -n 1)

if [[ -z "$vst3" && -z "$au" ]]; then
    echo "No KERF.vst3 or KERF.component found in $src" >&2
    echo "Pass the unpacked download folder (or build KERF first)." >&2
    exit 1
fi

install_bundle() # <bundle> <target folder>
{
    local name; name=$(basename "$1")
    mkdir -p "$2"
    rm -rf "$2/$name"
    ditto "$1" "$2/$name"
    # A download from the internet carries the quarantine flag. Unsigned (not notarised)
    # builds are then blocked by Gatekeeper; removing it is what "Open anyway" would do.
    xattr -dr com.apple.quarantine "$2/$name" 2>/dev/null || true
    echo "Installed: $2/$name"
}

[[ -n "$vst3" ]] && install_bundle "$vst3" "$HOME/Library/Audio/Plug-Ins/VST3"
[[ -n "$au"   ]] && install_bundle "$au"   "$HOME/Library/Audio/Plug-Ins/Components"
[[ -n "$app"  ]] && install_bundle "$app"  "$HOME/Applications"

# Make the system forget the cached AU list, so Logic / GarageBand rescan it.
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo
echo "Done. Restart your DAW (Logic: it checks the AU on the next start)."
echo "Check the AU yourself with:  auval -v aumu Krf1 Kerf"
