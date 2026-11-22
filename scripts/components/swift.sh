#!/bin/bash
# Build the Swift products against the locally built libkrun.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=swift
OUTDIR="$OUT/$NAME"
INPUT="$(swift_input_digest)"

if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/Brinewell" "$OUTDIR/brinewell-vm" "$OUTDIR/brinewell-diagnostics"; then
    exit 0
fi

mkdir -p "$OUTDIR"
# SwiftPM build products record absolute paths (module cache, PCH); start over
# if this checkout was moved since the last build.
if [[ "$(cat "$BUILD_ROOT/swiftpm-root" 2>/dev/null)" != "$ROOT" ]]; then
    rm -rf "$BUILD_ROOT/$(uname -m)-apple-macosx"
    printf '%s\n' "$ROOT" > "$BUILD_ROOT/swiftpm-root"
fi
export BRINEWELL_LIBKRUN_DIR="$OUT/libkrun"
export BRINEWELL_LIBKRUN_INCLUDE="$ROOT/../libkrun/include"
swift build --disable-sandbox --package-path "$ROOT" -c release --product Brinewell
swift build --disable-sandbox --package-path "$ROOT" -c release --product brinewell-vm
swift build --disable-sandbox --package-path "$ROOT" -c release --product brinewell-diagnostics
BIN="$(swift build --disable-sandbox --package-path "$ROOT" -c release --show-bin-path)"
cp "$BIN/Brinewell" "$BIN/brinewell-vm" "$BIN/brinewell-diagnostics" "$OUTDIR/"
stamp_write "$NAME" "$INPUT" "$OUTDIR/Brinewell" "$OUTDIR/brinewell-vm" "$OUTDIR/brinewell-diagnostics"
