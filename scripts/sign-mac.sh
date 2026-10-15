#!/usr/bin/env bash
# Signs KERF with an Apple "Developer ID Application" certificate, has Apple notarise it and
# staples the ticket, so it opens on any Mac without the "unidentified developer" warning.
# Needs a paid Apple Developer account. GitHub Actions runs this by itself
# once the secrets exist; on a Mac you can run it by hand after a Release build.
#
# Usage:
#   MACOS_SIGNING_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
#   APPLE_ID="you@example.com" APPLE_TEAM_ID="TEAMID" APPLE_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx" \
#   bash scripts/sign-mac.sh [folder with KERF.vst3, KERF.component, KERF.app]
#
# Without APPLE_ID etc. it only signs (no notarisation).

set -euo pipefail
cd "$(dirname "$0")/.."

dir="${1:-build-mac/Kerf_artefacts/Release}"
entitlements="build-mac/Kerf_artefacts/JuceLibraryCode"
: "${MACOS_SIGNING_IDENTITY:?Set MACOS_SIGNING_IDENTITY (e.g. Developer ID Application: Name (TEAMID))}"

bundles=()
for b in "$dir"/KERF.vst3 "$dir"/VST3/KERF.vst3 "$dir"/KERF.component "$dir"/AU/KERF.component \
         "$dir"/KERF.app "$dir"/Standalone/KERF.app; do
    [[ -d "$b" ]] && bundles+=("$b")
done
(( ${#bundles[@]} )) || { echo "No KERF bundles in $dir" >&2; exit 1; }

for b in "${bundles[@]}"; do
    case "$b" in
        *.vst3)      ent="$entitlements/Kerf_VST3.entitlements" ;;
        *.component) ent="$entitlements/Kerf_AU.entitlements" ;;
        *.app)       ent="$entitlements/Kerf_Standalone.entitlements" ;;
    esac
    args=(--force --timestamp --options runtime --sign "$MACOS_SIGNING_IDENTITY")
    [[ -f "$ent" ]] && args+=(--entitlements "$ent")
    codesign "${args[@]}" "$b"
    codesign --verify --strict --verbose=2 "$b"
    echo "Signed: $b"
done

if [[ -z "${APPLE_ID:-}" || -z "${APPLE_TEAM_ID:-}" || -z "${APPLE_APP_PASSWORD:-}" ]]; then
    echo "APPLE_ID / APPLE_TEAM_ID / APPLE_APP_PASSWORD not set: signed, but not notarised."
    exit 0
fi

# One upload for all bundles; Apple's ticket then covers each of them.
zip="$(mktemp -d)/KERF-notarise.zip"
staging="$(mktemp -d)"
for b in "${bundles[@]}"; do ditto "$b" "$staging/$(basename "$b")"; done
ditto -c -k "$staging" "$zip"

xcrun notarytool submit "$zip" \
    --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" \
    --wait --timeout 30m

for b in "${bundles[@]}"; do
    xcrun stapler staple "$b"
done
spctl --assess --type execute --verbose "$(printf '%s\n' "${bundles[@]}" | grep '\.app$' | head -n 1)" 2>&1 || true
echo "Notarised and stapled."
