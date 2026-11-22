#!/bin/bash
# Build the full Brinewell initramfs: static tools + busybox + init script.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=initramfs
OUTDIR="$OUT/$NAME"
INIT="$ROOT/runtime/initramfs/init"
INSTALL_IMAGE="$ROOT/runtime/initramfs/install-from-image"
MKCONF="$ROOT/runtime/initramfs/etc/mke2fs.conf"
BUSYBOX_DEB="$CACHE/busybox-static_1.37.0-6+b9_arm64.deb"
BUSYBOX_SHA="c833be48abfa16bc19c4966ec93e289ff1ce5d2f1476cad3a57bd105378cd15c"
BUSYBOX_URL="https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6+b9_arm64.deb"
DOS_URL="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initramfs"]["dosfstools_url"])' "$ROOT/deps.lock.json")"
E2_URL="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initramfs"]["e2fsprogs_url"])' "$ROOT/deps.lock.json")"
BTRFS_URL="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initramfs"]["btrfs_progs_url"])' "$ROOT/deps.lock.json")"
DOS_TAR="$CACHE/initramfs-src/dosfstools-4.2.tar.gz"
E2_TAR="$CACHE/initramfs-src/e2fsprogs-1.47.2.tar.xz"
BTRFS_TAR="$CACHE/initramfs-src/btrfs-progs-v6.14.tar.xz"
DOS_SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initramfs"]["dosfstools_sha256"])' "$ROOT/deps.lock.json")"
E2_SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initramfs"]["e2fsprogs_sha256"])' "$ROOT/deps.lock.json")"
BTRFS_SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initramfs"]["btrfs_progs_sha256"])' "$ROOT/deps.lock.json")"
INPUT="$(input_digest "$NAME" \
    script "$(sha256_file "${BASH_SOURCE[0]}")" \
    init "$(sha256_file "$INIT")" \
    install_image "$(sha256_file "$INSTALL_IMAGE")" \
    mke2fs_conf "$(sha256_file "$MKCONF")" \
    busybox "$BUSYBOX_SHA" dosfstools "$DOS_SHA" e2fsprogs "$E2_SHA" btrfs "$BTRFS_SHA" \
    image "$BUILDER_IMAGE")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/initramfs.cpio.gz"; then exit 0; fi

require_builder
fetch_verified "$BUSYBOX_URL" "$BUSYBOX_DEB" "$BUSYBOX_SHA"
fetch_verified "$DOS_URL" "$DOS_TAR" "$DOS_SHA"
fetch_verified "$E2_URL" "$E2_TAR" "$E2_SHA"
fetch_verified "$BTRFS_URL" "$BTRFS_TAR" "$BTRFS_SHA"
mkdir -p "$(dirname "$DOS_TAR")" "$OUTDIR"
[[ -f "$DOS_TAR" ]] || die "missing $DOS_TAR"
[[ -f "$E2_TAR" ]] || die "missing $E2_TAR"
[[ -f "$BTRFS_TAR" ]] || die "missing $BTRFS_TAR"
[[ "$(sha256_file "$DOS_TAR")" == "$DOS_SHA" ]] || die "dosfstools checksum mismatch"
[[ "$(sha256_file "$E2_TAR")" == "$E2_SHA" ]] || die "e2fsprogs checksum mismatch"
[[ "$(sha256_file "$BTRFS_TAR")" == "$BTRFS_SHA" ]] || die "btrfs-progs checksum mismatch"

BUILDER_IMAGE_TAG=brinewell-guest-builder
ensure_builder_image "$BUILDER_IMAGE_TAG" "$ROOT/scripts/containers/guest.Dockerfile" "$ROOT/scripts/containers"

builder run --rm -i \
    -v "$CACHE:/cache:ro" \
    -v "$INIT:/inputs/init:ro" \
    -v "$INSTALL_IMAGE:/inputs/install-from-image:ro" \
    -v "$MKCONF:/inputs/mke2fs.conf:ro" \
    -v "$OUTDIR:/out" \
    "$BUILDER_IMAGE_TAG" \
    bash -s <<'INNER'
set -euo pipefail
rm -rf /build /tools /rootfs
mkdir -p /build /tools
cd /build
tar -xzf /cache/initramfs-src/dosfstools-4.2.tar.gz
cd dosfstools-4.2
./configure --disable-dependency-tracking CFLAGS="-O2 -static" LDFLAGS="-static"
make -j6
cp src/mkfs.fat src/fsck.fat /tools/
cd /build
tar -xf /cache/initramfs-src/e2fsprogs-1.47.2.tar.xz
cd e2fsprogs-1.47.2
./configure --disable-nls --disable-elf-shlibs --disable-libuuid \
    --disable-libblkid --disable-uuidd --disable-fsck --disable-e2initrd-helper
make -j6 LDFLAGS="-static"
cp misc/mke2fs /tools/ 2>/dev/null || cp mke2fs /tools/mke2fs
cp e2fsck/e2fsck resize/resize2fs misc/dumpe2fs /tools/
make install >/dev/null
ldconfig 2>/dev/null || true
cd /build
tar -xf /cache/initramfs-src/btrfs-progs-v6.14.tar.xz
cd btrfs-progs-v6.14
./configure --disable-documentation --disable-python --disable-libudev \
    --disable-zstd --disable-lzo \
    EXT2FS_CFLAGS="-I/build/e2fsprogs-1.47.2/lib" \
    EXT2FS_LIBS="-L/build/e2fsprogs-1.47.2/lib/ext2fs -lext2fs"
make -j6 btrfstune LDFLAGS="-static"
cp btrfstune /tools/
for tool in mkfs.fat fsck.fat mke2fs e2fsck resize2fs dumpe2fs btrfstune; do
    file "/tools/$tool" | grep -q "statically linked" || {
        echo "initramfs tool is not static: $tool" >&2
        file "/tools/$tool" >&2
        exit 1
    }
done

mkdir -p /rootfs/bin /rootfs/etc /rootfs/dev /rootfs/proc /rootfs/sys \
         /rootfs/run /rootfs/tmp /rootfs/esp /rootfs/efi /rootfs/newroot
dpkg-deb -x /cache/busybox-static_1.37.0-6+b9_arm64.deb /rootfs
cp /rootfs/usr/bin/busybox /rootfs/bin/busybox
cp /tools/* /rootfs/bin/
cp /inputs/init /rootfs/init
cp /inputs/install-from-image /rootfs/install-from-image
cp /inputs/mke2fs.conf /rootfs/etc/mke2fs.conf
chmod +x /rootfs/init /rootfs/bin/*
ln -sf bin /rootfs/sbin
/rootfs/bin/busybox --list | while IFS= read -r applet; do
    [ -n "$applet" ] || continue
    [ -e "/rootfs/bin/$applet" ] && continue
    ln -sf busybox "/rootfs/bin/$applet"
done
cd /rootfs
find . -print0 | cpio --null -o -H newc --owner 0:0 | gzip -9 > /out/initramfs.cpio.gz
INNER

gzip -t "$OUTDIR/initramfs.cpio.gz"
cpio -it < "$OUTDIR/initramfs.cpio.gz" >/dev/null
stamp_write "$NAME" "$INPUT" "$OUTDIR/initramfs.cpio.gz"
