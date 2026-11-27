#!/bin/bash
# Build Mesa's KosmicKrisp Vulkan driver for macOS.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=kosmicrisp
OUTDIR="$OUT/$NAME"
read -r TAG URL SHA < <(python3 - "$ROOT/deps.lock.json" <<'LOCK'
import json, sys
with open(sys.argv[1]) as f:
    release = json.load(f)["kosmickrisp"]
print(release["tag"], release["url"], release["sha256"])
LOCK
)
ensure_meson
ensure_brew ninja cmake pkg-config llvm libclc spirv-llvm-translator spirv-tools spirv-headers
LLVM_PREFIX="$(brew --prefix llvm)"
LIBCLC_PREFIX="$(brew --prefix libclc)"
export PKG_CONFIG_PATH="$LIBCLC_PREFIX/lib/pkgconfig:$LIBCLC_PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"
INPUT="$(input_digest "$NAME" tag "$TAG" source "$SHA" recipe "$(sha256_file "$0")" \
    meson "$(meson --version)" llvm "$("$LLVM_PREFIX/bin/llvm-config" --version)" target macos-kosmickrisp)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libvulkan_kosmickrisp.dylib" "$OUTDIR/libSPIRV-Tools.dylib"; then exit 0; fi

ARCHIVE="$CACHE/$TAG.tar.xz"
fetch_verified "$URL" "$ARCHIVE" "$SHA"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/src" "$OUTDIR/toolbin"
ln -sf "$LLVM_PREFIX/bin/llvm-config" "$OUTDIR/toolbin/llvm-config"
export PATH="$OUTDIR/toolbin:$PATH"
tar -xf "$ARCHIVE" --strip-components=1 -C "$OUTDIR/src"
export PYTHONDONTWRITEBYTECODE=1
export MACOSX_DEPLOYMENT_TARGET=26.0
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
meson setup "$OUTDIR/build" "$OUTDIR/src" \
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
