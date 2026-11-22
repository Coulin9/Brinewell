#!/bin/bash
# Build the Brinewell arm64 Linux kernel from upstream source + project patches.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=kernel
OUTDIR="$OUT/$NAME"
PATCHES="$ROOT/runtime/kernel/patches"
CONFIG="$ROOT/runtime/kernel/config"
TARBALL="$CACHE/linux-7.2.9.tar.xz"
SOURCE_SHA="b4c5dfbe51a364a6c7f03869200f88c8e1f77403539005f14b7fc6bc91b8d8ba"
SOURCE_URL="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.9.tar.xz"
PATCH_DIGEST="$(find "$PATCHES" -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | awk '{print $1}')"
INPUT="$(input_digest "$NAME" \
    source "$SOURCE_SHA" \
    patches "$PATCH_DIGEST" \
    config "$(sha256_file "$CONFIG")" \
    image "$BUILDER_IMAGE" \
    kbuild_user brinewell kbuild_host brinewell)"

if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/Image" "$OUTDIR/kernel.config" "$OUTDIR/kernel.release"; then
    exit 0
fi

mkdir -p "$OUTDIR" "$CACHE"
if [[ ! -f "$TARBALL" ]]; then
    local_fallback="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["kernel"].get("local_fallback",""))' "$ROOT/deps.lock.json")"
    if [[ -n "$local_fallback" && -f "$local_fallback" ]]; then
        log "seeding kernel source cache from $local_fallback"
        cp "$local_fallback" "$TARBALL"
    fi
fi
if [[ ! -f "$TARBALL" ]]; then
    fetch_verified "$SOURCE_URL" "$TARBALL" "$SOURCE_SHA"
else
    [[ "$(sha256_file "$TARBALL")" == "$SOURCE_SHA" ]] || die "kernel tarball checksum mismatch"
fi

require_builder
BUILDER_IMAGE_TAG=brinewell-kernel-builder
ensure_builder_image "$BUILDER_IMAGE_TAG" "$ROOT/scripts/containers/kernel.Dockerfile" "$ROOT/scripts/containers"

log "kernel build (linux 7.2.9 + $(find "$PATCHES" -type f | wc -l | tr -d ' ') patches)"
builder run --rm \
    -v "$CACHE:/cache:ro" \
    -v "$PATCHES:/patches:ro" \
    -v "$CONFIG:/config:ro" \
    -v "$OUTDIR:/out" \
    "$BUILDER_IMAGE_TAG" \
    bash -lc '
set -euo pipefail
rm -rf /build && mkdir -p /build/obj
cd /build
tar -xf /cache/linux-7.2.9.tar.xz
cd linux-7.2.9
for p in /patches/*.patch; do
    echo "applying $(basename "$p")"
    patch -p1 -F0 --no-backup-if-mismatch < "$p"
done
cp /config /build/obj/.config
make O=/build/obj ARCH=arm64 olddefconfig
make O=/build/obj ARCH=arm64 KBUILD_BUILD_USER=brinewell KBUILD_BUILD_HOST=brinewell -j6 Image
cp /build/obj/arch/arm64/boot/Image /out/Image
cp /build/obj/.config /out/kernel.config
cat /build/obj/include/config/kernel.release > /out/kernel.release
'

[[ "$(cat "$OUTDIR/kernel.release")" == "7.2.9-brinewell" ]] || die "unexpected kernel release: $(cat "$OUTDIR/kernel.release")"
if strings "$OUTDIR/Image" | grep -qi 'steamac'; then
    die "kernel Image still contains a steamac string"
fi
for marker in blob_alignment ACTLR_EL1 mem_model; do
    grep -aq "$marker" "$OUTDIR/Image" || die "kernel Image missing patch marker: $marker"
done
for option in CONFIG_DRM_VIRTIO_GPU=y CONFIG_EROFS_FS=y CONFIG_OVERLAY_FS=y \
              CONFIG_VIRTIO_BLK=y CONFIG_VIRTIO_CONSOLE=y CONFIG_VIRTIO_INPUT=y \
              CONFIG_VIRTIO_NET=y CONFIG_SND_VIRTIO=y CONFIG_ARM64_4K_PAGES=y; do
    grep -qx "$option" "$OUTDIR/kernel.config" || die "kernel config missing $option"
done
stamp_write "$NAME" "$INPUT" "$OUTDIR/Image" "$OUTDIR/kernel.config" "$OUTDIR/kernel.release"
