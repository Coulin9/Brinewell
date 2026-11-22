#!/bin/bash
# Build virglrenderer (Venus + Neptune) and a universal render server.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=virglrenderer
SRC="$ROOT/../virglrenderer"
OUTDIR="$OUT/$NAME"
CROSS="$ROOT/../d3dmetal-native/build-macos-x86_64.txt"
[[ -f "$OUT/libepoxy/libepoxy.0.dylib" ]] || die "libepoxy must be built first"
ensure_meson
ensure_brew ninja pkg-config
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" meson "$(meson --version)" epoxy "$(sha256_file "$OUT/libepoxy/libepoxy.0.dylib")" neptune true universal true cross "$(sha256_file "$CROSS")")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libvirglrenderer.1.dylib" "$OUTDIR/virgl_render_server"; then exit 0; fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
export PATH="$BUILD_ROOT/toolchain/venv/bin:$PATH"
export PKG_CONFIG_PATH="$OUT/libepoxy/prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
for arch in arm64 x86_64; do
    prefix="$OUTDIR/prefix"
    [[ $arch == x86_64 ]] && prefix="$OUTDIR/prefix-x86_64"
    setup=(setup "$OUTDIR/build-$arch" "$SRC"
        --prefix="$prefix" --libdir=lib --buildtype=release
        -Dvenus=true -Dneptune=true -Dvulkan-dload=true -Dplatforms=[]
        -Drender-server-mode=process -Drender-server-worker=process
        -Dvtest=false -Dtests=false -Dvideo=false -Dcheck-gl-errors=false)
    if [[ $arch == x86_64 ]]; then
        setup+=(--cross-file "$CROSS")
    fi
    meson "${setup[@]}"
    ninja -C "$OUTDIR/build-$arch"
    if [[ $arch == arm64 ]]; then
        ninja -C "$OUTDIR/build-arm64" install
    fi
done

dylib="$OUTDIR/prefix/lib/libvirglrenderer.1.dylib"
[[ -f "$dylib" ]] || dylib="$(find "$OUTDIR/build-arm64" -name 'libvirglrenderer.1.dylib' -type f | head -1)"
arm_server="$OUTDIR/build-arm64/server/virgl_render_server"
intel_server="$OUTDIR/build-x86_64/server/virgl_render_server"
[[ -f "$dylib" && -f "$arm_server" && -f "$intel_server" ]] || die "virglrenderer outputs not found"

cp "$dylib" "$OUTDIR/libvirglrenderer.1.dylib"
ln -sf libvirglrenderer.1.dylib "$OUTDIR/libvirglrenderer.dylib"
install_name "$OUTDIR/libvirglrenderer.1.dylib" libvirglrenderer.1.dylib
epoxy_dep="$(otool -L "$OUTDIR/libvirglrenderer.1.dylib" | awk '/libepoxy/{print $1; exit}')"
[[ -z "$epoxy_dep" ]] || install_name_tool -change "$epoxy_dep" @rpath/libepoxy.0.dylib "$OUTDIR/libvirglrenderer.1.dylib"
install_name_tool -add_rpath @loader_path "$OUTDIR/libvirglrenderer.1.dylib" 2>/dev/null || true
sign "$OUTDIR/libvirglrenderer.1.dylib"

lipo -create "$arm_server" "$intel_server" -output "$OUTDIR/virgl_render_server"
install_name_tool -add_rpath @executable_path/../Frameworks "$OUTDIR/virgl_render_server" 2>/dev/null || true
sign "$OUTDIR/virgl_render_server"
lipo -info "$OUTDIR/virgl_render_server"
stamp_write "$NAME" "$INPUT" "$OUTDIR/libvirglrenderer.1.dylib" "$OUTDIR/virgl_render_server"
