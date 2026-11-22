#!/bin/bash
# Wrap the existing app bundle in a drag-to-install disk image.
set -euo pipefail
source "$(cd "$(dirname "$0")/.." && pwd)/scripts/lib/common.sh"

[[ -d "$APP" ]] || die "missing build/Brinewell.app; run ./build.sh first"
codesign --verify --deep --strict "$APP"

DMG="$ROOT/build/Brinewell.dmg"
temp="$(mktemp -d "$ROOT/build/.dmg.XXXXXX")"
trap 'rm -rf "$temp"' EXIT

mkdir "$temp/volume"
ditto "$APP" "$temp/volume/Brinewell.app"
ln -s /Applications "$temp/volume/Applications"
hdiutil create -fs HFS+ -volname Brinewell -format UDZO \
    -srcfolder "$temp/volume" "$temp/Brinewell.dmg"

# Replace the previous image only after the new one has been created.
mv -f "$temp/Brinewell.dmg" "$DMG"
log "disk image: ${DMG#"$ROOT"/}"
