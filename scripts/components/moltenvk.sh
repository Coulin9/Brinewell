#!/bin/bash
# Fetch the official KhronosGroup MoltenVK release dylib.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=moltenvk
OUTDIR="$OUT/$NAME"
VERSION="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["moltenvk"]["version"])' "$ROOT/deps.lock.json")"
URL="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["moltenvk"]["url"])' "$ROOT/deps.lock.json")"
SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["moltenvk"]["sha256"])' "$ROOT/deps.lock.json")"
TAR="$CACHE/MoltenVK-macos-v$VERSION.tar"
INPUT="$(input_digest "$NAME" version "$VERSION" sha "$SHA")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libMoltenVK.dylib"; then exit 0; fi

fetch_verified "$URL" "$TAR" "$SHA"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/extract"
tar -xf "$TAR" -C "$OUTDIR/extract"
dylib="$(find "$OUTDIR/extract" -name libMoltenVK.dylib -type f | head -1)"
[[ -n "$dylib" ]] || die "libMoltenVK.dylib not found in $TAR"
cp "$dylib" "$OUTDIR/libMoltenVK.dylib"
install_name "$OUTDIR/libMoltenVK.dylib" libMoltenVK.dylib
sign "$OUTDIR/libMoltenVK.dylib"
rm -rf "$OUTDIR/extract"
stamp_write "$NAME" "$INPUT" "$OUTDIR/libMoltenVK.dylib"
