#!/bin/bash
# Build the minimal aarch64 guest Mesa (zink + virtio Vulkan) for SteamOS.
#
# The source is the project's virtio-win-mesa checkout, which tracks upstream
# Mesa and carries the Neptune guest driver plus this product's backports.  The
# Neptune driver is built with -Dneptune=true for the Wine payload; the guest
# layer here runs the venus/zink stack, so it stays off.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=mesa
SRC="${BRINEWELL_MESA_SRC:-$ROOT/../virtio-win-mesa}"
OUTDIR="$OUT/$NAME"
git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 || die "missing Mesa checkout: ${SRC#"$ROOT/.."}"
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" image "$BUILDER_IMAGE" i386_dockerfile "$(sha256_file "$ROOT/scripts/containers/mesa-i386.Dockerfile")" config zink-virtio-aarch64-wayland-glvnd-i686 xattr_policy stripped-v1 neptune off)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/root.tar"; then exit 0; fi

require_builder
BUILDER_IMAGE_TAG=brinewell-guest-builder
ensure_builder_image "$BUILDER_IMAGE_TAG" "$ROOT/scripts/containers/guest.Dockerfile" "$ROOT/scripts/containers"
I386_BUILDER_IMAGE=brinewell-mesa-i386-builder
ensure_builder_image "$I386_BUILDER_IMAGE" "$ROOT/scripts/containers/mesa-i386.Dockerfile" "$ROOT/scripts/containers"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/root" "$OUTDIR/src"
rsync -a --exclude .git "$SRC/" "$OUTDIR/src/"
builder run --rm \
    -v "$OUTDIR/src:/src:rw" \
    -v "$OUTDIR/root:/out:rw" \
    "$BUILDER_IMAGE_TAG" \
    bash -lc '
set -euo pipefail
rm -rf /build && mkdir -p /build
meson setup /build /src \
    --prefix=/usr --libdir=lib --buildtype=release \
    -Dplatforms=x11,wayland -Dgallium-drivers=zink,softpipe \
    -Dvulkan-drivers=virtio -Dglx=dri -Degl=enabled -Dgbm=enabled -Dglvnd=enabled \
    -Dllvm=disabled -Dzstd=enabled -Dbuild-tests=false \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dvideo-codecs= -Dtools= -Dneptune=false
ninja -C /build -j6
DESTDIR=/out ninja -C /build install
'
if [[ ! -f "$OUTDIR/root/usr/share/vulkan/icd.d/virtio_icd.aarch64.json" ]]; then
    mkdir -p "$OUTDIR/root/usr/share/vulkan/icd.d"
    cat > "$OUTDIR/root/usr/share/vulkan/icd.d/virtio_icd.aarch64.json" <<'JSON'
{
    "ICD": {
        "api_version": "1.4.363",
        "library_arch": "64",
        "library_path": "/usr/lib/libvulkan_virtio.so"
    },
    "file_format_version": "1.0.1"
}
JSON
fi
cat > "$OUTDIR/i686-cross.txt" <<'CROSS'
[binaries]
c = 'i686-linux-gnu-gcc'
cpp = 'i686-linux-gnu-g++'
pkg-config = 'pkg-config'
wayland-scanner = '/usr/local/bin/wayland-scanner'

[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'

[properties]
pkg_config_libdir = ['/usr/lib/i386-linux-gnu/pkgconfig', '/usr/share/pkgconfig']

[built-in options]
c_args = ['-march=i686']
cpp_args = ['-march=i686']
CROSS
builder run --rm \
    -v "$OUTDIR/src:/src:rw" \
    -v "$OUTDIR/root:/out:rw" \
    -v "$OUTDIR/i686-cross.txt:/cross.txt:ro" \
    "$I386_BUILDER_IMAGE" bash -lc '
set -euo pipefail
meson setup /build-i686 /src --cross-file /cross.txt \
    --prefix=/usr --libdir=lib32 --buildtype=release \
    -Dvulkan-drivers=virtio -Dgallium-drivers= -Dglx=disabled -Degl=disabled \
    -Dgbm=disabled -Dllvm=disabled -Dplatforms=x11,wayland -Dxlib-lease=disabled \
    -Ddisplay-info=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Dzlib=enabled -Dzstd=enabled -Dexpat=enabled -Dbuild-tests=false \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dtools= \
    -Dvideo-codecs= -Dneptune=false -Dvulkan-manifest-per-architecture=true
ninja -C /build-i686 -j6 src/virtio/vulkan/libvulkan_virtio.so src/virtio/vulkan/virtio_icd.i686.json
lib=/build-i686/src/virtio/vulkan/libvulkan_virtio.so
manifest=/build-i686/src/virtio/vulkan/virtio_icd.i686.json
readelf -h "$lib" | grep -Eq "Class:[[:space:]]+ELF32"
readelf -h "$lib" | grep -Eq "Machine:[[:space:]]+Intel 80386"
if readelf -sW "$lib" | grep -E "UND[[:space:]]+wl_display_dispatch_queue_timeout(@|$)"; then
    echo "incompatible Wayland timeout symbol" >&2; exit 1
fi
python3 -c '\''import json,sys; d=json.load(open(sys.argv[1]))["ICD"]; assert d["library_arch"] == "32" and d["library_path"] == "/usr/lib32/libvulkan_virtio.so"'\'' "$manifest"
base=/out/usr/share/guestos/fex-mesa/usr
mkdir -p "$base/lib32" "$base/share/vulkan/icd.d"
cp --no-preserve=xattr,mode "$lib" "$base/lib32/libvulkan_virtio.so"
cp --no-preserve=xattr,mode "$manifest" "$base/share/vulkan/icd.d/virtio_icd.x86.json"
chmod 0755 "$base/lib32/libvulkan_virtio.so"
chmod 0644 "$base/share/vulkan/icd.d/virtio_icd.x86.json"
chmod 0755 /out/usr /out/usr/share /out/usr/share/guestos \
    /out/usr/share/guestos/fex-mesa "$base" "$base/lib32" \
    "$base/share" "$base/share/vulkan" "$base/share/vulkan/icd.d"
[[ $(stat -c %a "$base/lib32/libvulkan_virtio.so") == 755 ]]
[[ $(stat -c %a "$base/share/vulkan/icd.d/virtio_icd.x86.json") == 644 ]]
'
# SteamOS owns the GLVND dispatch libraries. Replacing them with standalone
# Mesa while GLFW loads libGLX.so.0 creates two independent GL dispatch states.
for name in libGL.so.1 libEGL.so.1 libGLESv1_CM.so.1 libGLESv2.so.2; do
    [[ ! -e "$OUTDIR/root/usr/lib/$name" ]] || die "Mesa must not replace GLVND dispatcher: $name"
done
for name in libGLX_mesa.so.0 libEGL_mesa.so.0; do
    [[ -e "$OUTDIR/root/usr/lib/$name" ]] || die "missing Mesa GLVND vendor library: $name"
done
[[ -f "$OUTDIR/root/usr/share/glvnd/egl_vendor.d/50_mesa.json" ]] || die "missing Mesa EGL vendor manifest"
# Podman volume exports can expose Linux ACLs as host xattrs even after chmod.
xattr -cr "$OUTDIR/root"
for f in usr/share/guestos/fex-mesa/usr/lib32/libvulkan_virtio.so \
         usr/share/guestos/fex-mesa/usr/share/vulkan/icd.d/virtio_icd.x86.json; do
    attrs=$(xattr -l "$OUTDIR/root/$f")
    [[ "$attrs" != *system.posix_acl_access* ]] || die "unexpected POSIX ACL: $f"
done
COPYFILE_DISABLE=1 tar --no-xattrs -C "$OUTDIR/root" -cf "$OUTDIR/root.tar" .
find "$OUTDIR/root/usr/lib" -maxdepth 1 -name 'libgallium*' -o -name 'zink_dri.so' -o -name 'libvulkan_virtio.so' | sort
stamp_write "$NAME" "$INPUT" "$OUTDIR/root.tar"
