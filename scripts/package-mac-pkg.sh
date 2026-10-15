#!/bin/bash
# Builds the KERF installer for macOS (a .pkg: double-click, Continue, Install).
#
#   bash scripts/package-mac-pkg.sh <Release artefacts folder> <version> "<label>" <output folder>
#   e.g. bash scripts/package-mac-pkg.sh build-mac/Kerf_artefacts/Release 0.9.1 "0.9.1 beta" dist
#
# Installs for every user of the Mac:
#   KERF.vst3       -> /Library/Audio/Plug-Ins/VST3         (Ableton, FL Studio, Reaper, Bitwig ...)
#   KERF.component  -> /Library/Audio/Plug-Ins/Components   (Logic, GarageBand)
#   KERF.app        -> /Applications                        (runs without a DAW)
# The three are choices in the installer ("Customize"); all are on by default.
#
# With MACOS_INSTALLER_IDENTITY set ("Developer ID Installer: ..."), the .pkg is signed.
set -euo pipefail

artefacts="$1"
version="$2"
label="$3"
out="$4"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$out" "$work/pkgs" "$work/resources"

component () # <id> <bundle> <install location>
{
    local id="$1" bundle="$2" location="$3"
    local root="$work/root-$id"
    mkdir -p "$root"
    ditto "$artefacts/$bundle" "$root/$(basename "$bundle")"
    # Never "relocate" the bundle to a copy found elsewhere on the Mac: always install here.
    pkgbuild --analyze --root "$root" "$work/$id.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$work/$id.plist"
    pkgbuild --root "$root" --component-plist "$work/$id.plist" \
             --identifier "com.kerfinstruments.kerf.$id" --version "$version" \
             --install-location "$location" "$work/pkgs/kerf-$id.pkg"
}

component vst3 VST3/KERF.vst3 /Library/Audio/Plug-Ins/VST3
component au AU/KERF.component /Library/Audio/Plug-Ins/Components
component app Standalone/KERF.app /Applications

cat > "$work/resources/welcome.txt" <<EOF
KERF $label

A sample chopper, granular instrument and effects box.

This installs KERF for every user of this Mac:
  - VST3 plugin (Ableton Live, FL Studio, Reaper, Bitwig, Studio One ...)
  - Audio Unit (Logic Pro, GarageBand)
  - the standalone app (runs without a DAW)

Afterwards, restart your DAW (Logic finds new plugins on its next start).
EOF

cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>KERF $label</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <os-version min="11.0"/>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="vst3" title="VST3 plugin" description="For Ableton Live, FL Studio, Reaper, Bitwig, Studio One ...">
        <pkg-ref id="com.kerfinstruments.kerf.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit" description="For Logic Pro and GarageBand.">
        <pkg-ref id="com.kerfinstruments.kerf.au"/>
    </choice>
    <choice id="app" title="Standalone app" description="KERF without a DAW.">
        <pkg-ref id="com.kerfinstruments.kerf.app"/>
    </choice>
    <pkg-ref id="com.kerfinstruments.kerf.vst3" version="$version">kerf-vst3.pkg</pkg-ref>
    <pkg-ref id="com.kerfinstruments.kerf.au" version="$version">kerf-au.pkg</pkg-ref>
    <pkg-ref id="com.kerfinstruments.kerf.app" version="$version">kerf-app.pkg</pkg-ref>
</installer-gui-script>
EOF

pkg="$out/KERF-$version-macOS-Installer.pkg"
sign=()
if [ -n "${MACOS_INSTALLER_IDENTITY:-}" ]; then
    sign=(--sign "$MACOS_INSTALLER_IDENTITY")
fi
productbuild --distribution "$work/distribution.xml" --package-path "$work/pkgs" \
             --resources "$work/resources" "${sign[@]+"${sign[@]}"}" "$pkg"
echo "Built $pkg"
