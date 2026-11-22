#!/bin/bash
# Build universal libepoxy (arm64 Venus + x86_64 Neptune render workers).
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=libepoxy
SRC="$ROOT/../libepoxy"
OUTDIR="$OUT/$NAME"
CROSS="$ROOT/../d3dmetal-native/build-macos-x86_64.txt"
ensure_git_clone "https://github.com/anholt/libepoxy.git" "$SRC" 1.5.10
ensure_meson
ensure_brew ninja pkg-config
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" meson "$(meson --version)" build release-universal cross "$(sha256_file "$CROSS")")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libepoxy.0.dylib"; then exit 0; fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
export PATH="$BUILD_ROOT/toolchain/venv/bin:$PATH"
for arch in arm64 x86_64; do
    prefix="$OUTDIR/prefix"
    [[ $arch == x86_64 ]] && prefix="$OUTDIR/prefix-x86_64"
    setup=(setup "$OUTDIR/build-$arch" "$SRC"
        --prefix="$prefix" --libdir=lib --buildtype=release
        -Dtests=false)
    if [[ $arch == x86_64 ]]; then
        setup+=(--cross-file "$CROSS")
    fi
    meson "${setup[@]}"
    ninja -C "$OUTDIR/build-$arch"
    if [[ $arch == arm64 ]]; then
        ninja -C "$OUTDIR/build-arm64" install
    fi
done

arm="$(find "$OUTDIR/build-arm64" -name 'libepoxy.0.dylib' -type f | head -1)"
intel="$(find "$OUTDIR/build-x86_64" -name 'libepoxy.0.dylib' -type f | head -1)"
[[ -f "$arm" && -f "$intel" ]] || die "libepoxy outputs not found"
lipo -create "$arm" "$intel" -output "$OUTDIR/libepoxy.0.dylib"
install_name "$OUTDIR/libepoxy.0.dylib" libepoxy.0.dylib
sign "$OUTDIR/libepoxy.0.dylib"
cp "$OUTDIR/libepoxy.0.dylib" "$OUTDIR/prefix/lib/libepoxy.0.dylib"
lipo -info "$OUTDIR/libepoxy.0.dylib"
stamp_write "$NAME" "$INPUT" "$OUTDIR/libepoxy.0.dylib"
