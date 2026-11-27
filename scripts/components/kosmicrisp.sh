#!/bin/bash
# Build Mesa's KosmicKrisp Vulkan driver for macOS.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"
source "$ROOT/scripts/lib/mesa-source.sh"

NAME=kosmicrisp
OUTDIR="$OUT/$NAME"
ensure_mesa_source
ensure_meson
ensure_brew ninja cmake pkg-config llvm libclc spirv-llvm-translator spirv-tools spirv-headers
LLVM_PREFIX="$(brew --prefix llvm)"
LIBCLC_PREFIX="$(brew --prefix libclc)"
export PKG_CONFIG_PATH="$LIBCLC_PREFIX/lib/pkgconfig:$LIBCLC_PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"
PATCH_DIR="$ROOT/runtime/patches/kosmickrisp"
patches=("$PATCH_DIR"/*.patch)
patch_digest="$(cd "$PATCH_DIR" && shasum -a 256 ./*.patch | shasum -a 256 | awk '{print $1}')"
INPUT="$(input_digest "$NAME" tag "$MESA_TAG" source "$MESA_SHA256" recipe "$(sha256_file "$0")" \
    patches "$patch_digest" meson "$(meson --version)" llvm "$("$LLVM_PREFIX/bin/llvm-config" --version)" target macos-kosmickrisp)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libvulkan_kosmickrisp.dylib" "$OUTDIR/libSPIRV-Tools.dylib"; then exit 0; fi

# Patch a disposable APFS clone. Guest Mesa and Neptune consume the verified tree.
stage="$(mktemp -d "$OUTDIR/.host-source.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
cp -cR "$MESA_SRC" "$stage/src"
for patch_file in "${patches[@]}"; do
    GIT_CEILING_DIRECTORIES="$stage" git -C "$stage/src" apply --no-index "$patch_file"
done
rm -rf "$OUTDIR/host-src"
mv "$stage/src" "$OUTDIR/host-src"
rm -rf "$OUTDIR/build" "$OUTDIR/prefix" "$OUTDIR/toolbin"
rm -f "$OUTDIR/libvulkan_kosmickrisp.dylib" "$OUTDIR/libSPIRV-Tools.dylib"
mkdir -p "$OUTDIR/toolbin"
ln -sf "$LLVM_PREFIX/bin/llvm-config" "$OUTDIR/toolbin/llvm-config"
export PATH="$OUTDIR/toolbin:$PATH"
export PYTHONDONTWRITEBYTECODE=1
export MACOSX_DEPLOYMENT_TARGET=26.0
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
meson setup "$OUTDIR/build" "$OUTDIR/host-src" \
    --prefix="$OUTDIR/prefix" --libdir=lib --buildtype=release --wrap-mode=nodownload -Db_ndebug=true \
    -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
    -Dgallium-drivers= -Dopengl=false -Dzstd=disabled -Dxmlconfig=disabled
ninja -C "$OUTDIR/build"
ninja -C "$OUTDIR/build" install

dylib="$(find "$OUTDIR/prefix" -name 'libvulkan_kosmickrisp*.dylib' -type f | head -1)"
[[ -n "$dylib" ]] || dylib="$(find "$OUTDIR/build" -name 'libvulkan_kosmickrisp*.dylib' -type f | head -1)"
[[ -n "$dylib" ]] || die "KosmicKrisp dylib not found"
cp "$dylib" "$OUTDIR/libvulkan_kosmickrisp.dylib"
install_name "$OUTDIR/libvulkan_kosmickrisp.dylib" libvulkan_kosmickrisp.dylib
spv="$(otool -L "$OUTDIR/libvulkan_kosmickrisp.dylib" | awk '/libSPIRV-Tools/{print $1; exit}')"
if [[ -n "$spv" ]]; then
    cp "$spv" "$OUTDIR/libSPIRV-Tools.dylib"
    install_name "$OUTDIR/libSPIRV-Tools.dylib" libSPIRV-Tools.dylib
    install_name_tool -change "$spv" @rpath/libSPIRV-Tools.dylib "$OUTDIR/libvulkan_kosmickrisp.dylib"
    sign "$OUTDIR/libSPIRV-Tools.dylib"
fi
install_name_tool -add_rpath @loader_path "$OUTDIR/libvulkan_kosmickrisp.dylib" 2>/dev/null || true
sign "$OUTDIR/libvulkan_kosmickrisp.dylib"
stamp_write "$NAME" "$INPUT" "$OUTDIR/libvulkan_kosmickrisp.dylib" ${spv:+"$OUTDIR/libSPIRV-Tools.dylib"}
