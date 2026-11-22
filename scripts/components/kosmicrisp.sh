#!/bin/bash
# Build Mesa's KosmicKrisp Vulkan driver for macOS.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=kosmicrisp
SRC="$BUILD_ROOT/src/mesa-kosmickrisp"
UPSTREAM_URL="https://gitlab.freedesktop.org/mesa/mesa.git"
UPSTREAM_COMMIT="ce576c29ed90822ea15564e1e60cb48e455881bb"
ensure_git_commit "$UPSTREAM_URL" "$SRC" "$UPSTREAM_COMMIT"
OUTDIR="$OUT/$NAME"
ensure_meson
ensure_brew ninja cmake pkg-config llvm libclc spirv-llvm-translator spirv-tools spirv-headers
LLVM_PREFIX="$(brew --prefix llvm)"
LIBCLC_PREFIX="$(brew --prefix libclc)"
export PKG_CONFIG_PATH="$LIBCLC_PREFIX/lib/pkgconfig:$LIBCLC_PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"
PATCH="$ROOT/runtime/patches/mesa-llvm23.patch"
PATCHDIR="$ROOT/runtime/patches/kosmickrisp"
PATCHES_HASH="$(find "$PATCHDIR" -type f -name '*.patch' -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | awk '{print $1}')"
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" upstream "$UPSTREAM_COMMIT" patch "$(sha256_file "$PATCH")" patches "$PATCHES_HASH" meson "$(meson --version)" llvm "$("$LLVM_PREFIX/bin/llvm-config" --version)" target macos-kosmickrisp)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libvulkan_kosmickrisp.dylib" "$OUTDIR/libSPIRV-Tools.dylib"; then exit 0; fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/src" "$OUTDIR/toolbin"
ln -sf "$LLVM_PREFIX/bin/llvm-config" "$OUTDIR/toolbin/llvm-config"
export PATH="$OUTDIR/toolbin:$PATH"
rsync -a --exclude .git "$SRC/" "$OUTDIR/src/"
for _p in "$PATCHDIR"/*.patch; do
    echo "patching $(basename "$_p")"
    patch -d "$OUTDIR/src" -p1 -F0 < "$_p"
done
python3 - "$OUTDIR/src/src/kosmickrisp/bridge/mtl_device.m" <<'PY2'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
if "supportsPlacementSparse" in s and "brinewell_supports_placement_sparse" not in s:
    s = s.replace("#include <Metal/MTLDevice.h>", """#include <Metal/MTLDevice.h>
#import <objc/message.h>

static BOOL brinewell_supports_placement_sparse(id<MTLDevice> dev) {
   SEL sel = sel_registerName("supportsPlacementSparse");
   if (![dev respondsToSelector:sel]) return NO;
   BOOL (*fn)(id, SEL) = (BOOL (*)(id, SEL))objc_msgSend;
   return fn(dev, sel);
}""")
    s = s.replace("return dev.supportsPlacementSparse;", "return brinewell_supports_placement_sparse(dev);")
    p.write_text(s)
PY2
if patch -d "$OUTDIR/src" -p1 -F0 --dry-run < "$PATCH" >/dev/null 2>&1; then
    patch -d "$OUTDIR/src" -p1 -F0 < "$PATCH"
fi
export MACOSX_DEPLOYMENT_TARGET=26.0
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
meson setup "$OUTDIR/build" "$OUTDIR/src" \
    --prefix="$OUTDIR/prefix" --libdir=lib --buildtype=release -Db_ndebug=true \
    -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
    -Dgallium-drivers= -Dopengl=false -Dzstd=disabled
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
