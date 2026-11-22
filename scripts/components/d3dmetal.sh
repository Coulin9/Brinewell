#!/bin/bash
# Build the x86_64 D3DMetal backend adapter; D3DMetal.framework stays external.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=d3dmetal
SRC="$ROOT/../d3dmetal-native"
OUTDIR="$OUT/$NAME"
CROSS="$SRC/build-macos-x86_64.txt"
[[ -d "$SRC" ]] || die "missing d3dmetal-native checkout: ${SRC#"$ROOT/.."}"
ensure_meson
ensure_brew ninja pkg-config
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" meson "$(meson --version)" cross "$(sha256_file "$CROSS")" framework external build release per-app-metal-cache true)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libd3dmetal-native.dylib"; then exit 0; fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
export PATH="$BUILD_ROOT/toolchain/venv/bin:$PATH"
meson setup "$OUTDIR/build" "$SRC" \
    --cross-file "$CROSS" \
    --prefix="$OUTDIR/prefix" --libdir=lib --buildtype=release \
    -Dtests=disabled -Dper_app_metal_cache=true \
    -Ddev_framework_path=/__brinewell_external_d3dmetal_required__/D3DMetal
ninja -C "$OUTDIR/build"

dylib="$OUTDIR/build/libd3dmetal-native.dylib"
[[ -f "$dylib" ]] || dylib="$(find "$OUTDIR/build" -name 'libd3dmetal-native.dylib' -type f | head -1)"
[[ -f "$dylib" ]] || die "libd3dmetal-native.dylib not found"
cp "$dylib" "$OUTDIR/libd3dmetal-native.dylib"
install_name "$OUTDIR/libd3dmetal-native.dylib" libd3dmetal-native.dylib
sign "$OUTDIR/libd3dmetal-native.dylib"
[[ "$(lipo -archs "$OUTDIR/libd3dmetal-native.dylib")" == "x86_64" ]] || die "adapter is not x86_64"
stamp_write "$NAME" "$INPUT" "$OUTDIR/libd3dmetal-native.dylib"
