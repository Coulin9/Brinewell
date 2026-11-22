#!/bin/bash
# Build the arm64 Mullion adapter for the Neptune render worker.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=mullion
MULLION="$ROOT/../Mullion"
OUTDIR="$OUT/$NAME"
DYLIB="$OUTDIR/libmullion-native.dylib"
LICENSES="$OUTDIR/licenses"
[[ -f "$MULLION/meson.build" ]] || die "missing Mullion checkout: ../Mullion"

read -r LLVM_VERSION LLVM_URL LLVM_SHA LICENSE_URL LICENSE_SHA < <(python3 - "$ROOT/deps.lock.json" <<'PY'
import json, sys
entry = json.load(open(sys.argv[1]))['mullion_llvm']
print(*(entry[key] for key in ['version', 'url', 'sha256', 'license_url', 'license_sha256']))
PY
)
read -r MINGW_URL MINGW_SHA < <(python3 - "$ROOT/deps.lock.json" <<'PY'
import json, sys
entry = json.load(open(sys.argv[1]))['neptune_toolchain']
print(entry['url'], entry['sha256'])
PY
)

export DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"
if [[ "$DEVELOPER_DIR" == /Library/Developer/CommandLineTools ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
ensure_meson
require_cmd ninja xcrun
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$MULLION")" \
    recipe "$(sha256_file "$0")" meson "$(meson --version)" llvm "$LLVM_SHA" llvm_license "$LICENSE_SHA" \
    directx_headers "$MINGW_SHA" \
    sdk "$(xcrun -sdk macosx --show-sdk-build-version)" target "$MACOSX_DEPLOYMENT_TARGET")"
outputs=("$DYLIB" "$LICENSES/Mullion.txt" "$LICENSES/dxmt.txt" "$LICENSES/dxmt-mit.txt" "$LICENSES/LLVM.txt")
if component_should_reuse "$NAME" "$INPUT" "${outputs[@]}"; then exit 0; fi

LLVM="$BUILD_ROOT/toolchain/mullion-llvm-$LLVM_VERSION"
if [[ ! -x "$LLVM/bin/llvm-config" || ! -f "$LLVM/lib/libLLVMCore.a" ]]; then
    ARCHIVE="$CACHE/${LLVM_URL##*/}"
    fetch_verified "$LLVM_URL" "$ARCHIVE" "$LLVM_SHA"
    archive_root="${ARCHIVE##*/}"
    archive_root="${archive_root%.tar.xz}"
    staging="$(mktemp -d "$BUILD_ROOT/toolchain/.mullion-llvm.XXXXXXXX")"
    trap 'rm -rf "$staging"' EXIT
    tar -xf "$ARCHIVE" -C "$staging" --strip-components 1 \
        "$archive_root/include" "$archive_root/lib" "$archive_root/bin/llvm-config"
    rm -rf "$LLVM"
    mv "$staging" "$LLVM"
fi
[[ "$("$LLVM/bin/llvm-config" --version)" == "$LLVM_VERSION" ]] || die "unexpected Mullion LLVM version at $LLVM"
fetch_verified "$LICENSE_URL" "$LLVM/LICENSE.TXT" "$LICENSE_SHA"

MINGW="$BUILD_ROOT/toolchain/llvm-mingw-$MINGW_SHA"
MINGW_HEADERS="$MINGW/generic-w64-mingw32/include"
if [[ ! -f "$MINGW_HEADERS/d3d12.h" ]]; then
    archive="$CACHE/${MINGW_URL##*/}"
    archive_root="${archive##*/}"
    archive_root="${archive_root%.tar.xz}"
    fetch_verified "$MINGW_URL" "$archive" "$MINGW_SHA"
    mkdir -p "$MINGW"
    tar -xf "$archive" -C "$MINGW" --strip-components 1 "$archive_root/generic-w64-mingw32/include"
fi

if ! xcrun --no-cache -sdk macosx metal -v > "$LOGS/mullion-metal-toolchain.log" 2>&1; then
    die "Xcode Metal Toolchain is unavailable; run xcodebuild -downloadComponent MetalToolchain with the selected Xcode"
fi
rm -rf "$OUTDIR"
mkdir -p "$LICENSES"
meson setup "$OUTDIR/build" "$MULLION" --buildtype=release \
    -Dnative_llvm_path="$LLVM" -Dnative_directx_headers="$MINGW_HEADERS" -Denable_tests=false
ninja -C "$OUTDIR/build" src/mullion-native/libmullion-native.dylib
cp "$OUTDIR/build/src/mullion-native/libmullion-native.dylib" "$DYLIB"
[[ "$(lipo -archs "$DYLIB")" == "arm64" ]] || die "Mullion adapter is not arm64"
install_name "$DYLIB" libmullion-native.dylib
sign "$DYLIB"

cp "$MULLION/LICENSE" "$LICENSES/Mullion.txt"
cp "$MULLION/docs/licenses/dxmt.txt" "$MULLION/docs/licenses/dxmt-mit.txt" "$LICENSES/"
cp "$LLVM/LICENSE.TXT" "$LICENSES/LLVM.txt"
stamp_write "$NAME" "$INPUT" "${outputs[@]}"
