#!/bin/bash
# Easy Anti-Cheat-capable FEX (StonkGuy/FEX eac tag) staged for Steam's FEX-Emu compat tool.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"
NAME=fex-eac
OUTDIR="$OUT/$NAME"
SOURCE="$SRC/fex-eac"
DOCKERFILE="$ROOT/runtime/uu/Dockerfile"
read -r VERSION URL COMMIT < <(python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["fex_eac"]; print(d["version"], d["url"], d["commit"])' "$ROOT/deps.lock.json")
INPUT="$(input_digest "$NAME" commit "$COMMIT" recipe "$(sha256_file "$0")" container "$(sha256_file "$DOCKERFILE")")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/root.tar"; then exit 0; fi
require_builder
if [[ ! -d "$SOURCE/.git" ]]; then git clone --no-checkout "$URL" "$SOURCE"; fi
# Fetch the tag so it can be checked against the pinned commit (it is lightweight, so the
# build gets its describe string from OVERRIDE_VERSION instead).
if [[ "$(git -C "$SOURCE" rev-parse --verify --quiet "$VERSION^{commit}" || true)" != "$COMMIT" ]]; then
    git -C "$SOURCE" fetch origin "refs/tags/$VERSION:refs/tags/$VERSION"
fi
[[ "$(git -C "$SOURCE" rev-parse "$VERSION^{commit}")" == "$COMMIT" ]] \
    || die "$VERSION is $(git -C "$SOURCE" rev-parse "$VERSION^{commit}"), expected $COMMIT"
git -C "$SOURCE" checkout --detach "$COMMIT"
git -C "$SOURCE" submodule update --init --recursive --depth 1
IMAGE="brinewell-uu-builder:$(sha256_file "$DOCKERFILE" | cut -c1-12)"
if ! builder image exists "$IMAGE"; then builder build -t "$IMAGE" -f "$DOCKERFILE" "$ROOT/runtime/uu"; fi
mkdir -p "$OUTDIR"
builder run --rm -v "$SOURCE:/src:ro" -v "$OUTDIR:/out" -e "FEX_EAC_TAG=$VERSION" -e "FEX_EAC_SHA=$COMMIT" "$IMAGE" bash -euc '
    cmake -G Ninja -S /src -B /tmp/fex-build \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DBUILD_STEAM_SUPPORT=ON -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF \
        -DBUILD_FEXCONFIG=OFF -DENABLE_LTO=OFF -DENABLE_ASSERTIONS=OFF \
        -DTUNE_CPU=apple-m1 -DOVERRIDE_VERSION="$FEX_EAC_TAG"
    cmake --build /tmp/fex-build -j6
    rm -rf /tmp/stage
    DESTDIR=/tmp/stage cmake --install /tmp/fex-build --component Runtime
    root=/out/root
    rm -rf "$root"
    payload="$root/usr/lib/brinewell/fex-eac"
    install -d "$payload"
    cp -R /tmp/stage/. "$payload/"
    # Every file the guest installer swaps into the FEX-Emu compat tool.
    files="usr/bin/FEX usr/bin/FEXBash usr/bin/FEXGetConfig usr/bin/FEXOfflineCompiler usr/bin/FEXServer usr/bin/FEXServerManager usr/bin/FEXpidof FEXCompatTool VERSIONS.txt ConfigTemplate.json"
    for f in $files; do
        [[ -s "$payload/$f" ]] || { echo "payload missing or empty: $f" >&2; exit 1; }
    done
    describe=$(sed -n "s/^FEX describe\t//p" "$payload/VERSIONS.txt")
    [[ "$describe" == "$FEX_EAC_TAG" ]] || { echo "VERSIONS.txt describe is $describe, expected $FEX_EAC_TAG" >&2; exit 1; }
    bash_sha=$(sed -n "s/^FEX bash\t//p" "$payload/VERSIONS.txt")
    [[ "$bash_sha" == "$FEX_EAC_SHA" ]] || { echo "VERSIONS.txt bash is $bash_sha, expected $FEX_EAC_SHA" >&2; exit 1; }
    # SteamOS ships glibc 2.39: reject any binary needing a newer symbol version.
    newest=$(find "$payload" -type f -exec strings {} + | grep -o "GLIBC_2\.[0-9]*" | sort -Vu | tail -n1)
    echo "max glibc symbol version: ${newest:-none}"
    if [[ -n "$newest" && "$(printf "%s\nGLIBC_2.39\n" "$newest" | sort -V | tail -n1)" != "GLIBC_2.39" ]]; then
        echo "payload requires $newest, newer than GLIBC_2.39" >&2; exit 1
    fi
    licenses="$root/usr/share/licenses/brinewell-fex-eac"
    install -d "$licenses"
    cp /src/LICENSE "$licenses/FEX.txt"
    for module in fmt xxhash range-v3 unordered_dense rpmalloc jemalloc_glibc vixl cephes; do
        license=$(find "/src/External/$module" -maxdepth 1 -type f \( -iname "*licen[sc]e*" -o -iname "copying*" \) -print -quit)
        [[ -n "$license" && -s "$license" ]] || { echo "Missing license for $module" >&2; exit 1; }
        cp "$license" "$licenses/$module.txt"
    done
    cp /src/Source/Common/cpp-optparse/LICENSE "$licenses/cpp-optparse.txt"
    softfloat_header=/src/External/SoftFloat-3e/include/SoftFloat-3e/softfloat.h
    [[ -s "$softfloat_header" ]] || { echo "Missing SoftFloat license header" >&2; exit 1; }
    cp "$softfloat_header" "$licenses/softfloat.h"
    install -d "$licenses/drm-headers"
    for header in /src/External/drm-headers/include/fex-drm/*.h; do
        [[ -s "$header" ]] || { echo "Missing DRM license header: $header" >&2; exit 1; }
        cp "$header" "$licenses/drm-headers/"
    done
    tar -C "$root" -cf /out/root.tar .
'
stamp_write "$NAME" "$INPUT" "$OUTDIR/root.tar"
