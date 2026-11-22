#!/bin/bash
# Dedicated ARM64 FEX runtime and private networking tools for UU installation.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"
NAME=uu-runtime
OUTDIR="$OUT/$NAME"
SOURCE="$SRC/fex-uu"
DOCKERFILE="$ROOT/runtime/uu/Dockerfile"
read -r VERSION URL COMMIT < <(python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["uu_fex"]; print(d["version"], d["url"], d["commit"])' "$ROOT/deps.lock.json")
INPUT="$(input_digest "$NAME" commit "$COMMIT" recipe "$(sha256_file "$0")" container "$(sha256_file "$DOCKERFILE")")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/root.tar"; then exit 0; fi
require_builder
if [[ ! -d "$SOURCE/.git" ]]; then git clone --no-checkout "$URL" "$SOURCE"; fi
git -C "$SOURCE" cat-file -e "$COMMIT^{commit}" 2>/dev/null || git -C "$SOURCE" fetch origin "$COMMIT"
git -C "$SOURCE" checkout --detach "$COMMIT"
git -C "$SOURCE" submodule update --init --depth 1 \
    Source/Common/cpp-optparse External/drm-headers External/fmt External/xxhash \
    External/range-v3 External/unordered_dense External/rpmalloc External/jemalloc_glibc External/vixl
IMAGE="brinewell-uu-builder:$(sha256_file "$DOCKERFILE" | cut -c1-12)"
if ! builder image exists "$IMAGE"; then builder build -t "$IMAGE" -f "$DOCKERFILE" "$ROOT/runtime/uu"; fi
mkdir -p "$OUTDIR"
builder run --rm -v "$SOURCE:/src:ro" -v "$OUTDIR:/out" "$IMAGE" bash -euc '
    cmake -G Ninja -S /src -B /tmp/fex-build \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/lib/brinewell/uu-runtime \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_THUNKS=OFF \
        -DENABLE_LTO=OFF -DENABLE_GDB_SYMBOLS=OFF -DENABLE_OFFLINE_TELEMETRY=OFF \
        -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF
    cmake --build /tmp/fex-build --target FEX FEXServer -j6
    root=/out/root
    rm -rf "$root"
    install -d "$root/usr/lib/brinewell/uu-runtime/bin" "$root/usr/lib/brinewell/uu-runtime/lib"
    tools="$root/usr/lib/brinewell/uu-runtime"
    licenses="$root/usr/share/licenses/brinewell-uu-runtime"
    install -d "$licenses"
    install -m755 /tmp/fex-build/Bin/FEX /tmp/fex-build/Bin/FEXServer "$tools/bin/"
    for tool in curl ip nft xtables-nft-multi; do
        binary=$(command -v "$tool")
        install -m755 "$binary" "$tools/bin/$tool"
    done
    # Keep optional distribution libraries private; use the guests own glibc.
    for binary in "$tools/bin/"* /usr/lib/aarch64-linux-gnu/xtables/*.so; do
        ldd "$binary" | awk '\''$3 ~ /^\// {print $3}'\''
    done | sort -u | while read -r library; do
        case "$(basename "$library")" in libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*) continue;; esac
        cp -L "$library" "$tools/lib/"
        package=$(dpkg-query -S "$(readlink -f "$library")" 2>/dev/null | head -n1 | cut -d: -f1)
        if [[ -f "/usr/share/doc/$package/copyright" ]]; then
            cp "/usr/share/doc/$package/copyright" "$licenses/$package.txt"
        fi
    done
    for tool in iptables ip6tables iptables-save iptables-restore ip6tables-save ip6tables-restore; do
        ln -s xtables-nft-multi "$tools/bin/$tool"
    done
    tar -C /usr/lib/aarch64-linux-gnu/xtables -cf "$tools/xtables.tar" .
    install -m644 /etc/ssl/certs/ca-certificates.crt "$tools/ca-certificates.crt"
    cp /src/LICENSE "$root/usr/share/licenses/brinewell-uu-runtime/FEX.txt"
    for package in iptables iproute2 nftables curl; do
        cp "/usr/share/doc/$package/copyright" "$root/usr/share/licenses/brinewell-uu-runtime/$package.txt"
    done
    for module in fmt xxhash range-v3 unordered_dense rpmalloc jemalloc_glibc vixl cephes; do
        license=$(find "/src/External/$module" -maxdepth 1 -type f \( -iname "*licen[sc]e*" -o -iname "copying*" \) -print -quit)
        [[ -n "$license" && -s "$license" ]] || { echo "Missing license for $module" >&2; exit 1; }
        cp "$license" "$licenses/$module.txt"
    done
    cp /src/Source/Common/cpp-optparse/LICENSE "$licenses/cpp-optparse.txt"
    # These dependencies carry their notices in headers rather than LICENSE files.
    softfloat_header=/src/External/SoftFloat-3e/include/SoftFloat-3e/softfloat.h
    [[ -s "$softfloat_header" ]] || { echo "Missing SoftFloat license header" >&2; exit 1; }
    cp "$softfloat_header" "$licenses/softfloat.h"
    install -d "$licenses/drm-headers"
    for header in /src/External/drm-headers/include/fex-drm/*.h; do
        [[ -s "$header" ]] || { echo "Missing DRM license header: $header" >&2; exit 1; }
        cp "$header" "$licenses/drm-headers/"
    done
    dpkg-query -W > "$licenses/build-packages.txt"
    printf "%s\n" "FEX-2611.1" > "$tools/version"
    tar -C "$root" -cf /out/root.tar .
'
stamp_write "$NAME" "$INPUT" "$OUTDIR/root.tar"
