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
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" image "$BUILDER_IMAGE" config zink-virtio-aarch64-wayland-glvnd neptune off)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/root.tar"; then exit 0; fi

require_builder
BUILDER_IMAGE_TAG=brinewell-guest-builder
ensure_builder_image "$BUILDER_IMAGE_TAG" "$ROOT/scripts/containers/guest.Dockerfile" "$ROOT/scripts/containers"
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
# SteamOS owns the GLVND dispatch libraries. Replacing them with standalone
# Mesa while GLFW loads libGLX.so.0 creates two independent GL dispatch states.
for name in libGL.so.1 libEGL.so.1 libGLESv1_CM.so.1 libGLESv2.so.2; do
    [[ ! -e "$OUTDIR/root/usr/lib/$name" ]] || die "Mesa must not replace GLVND dispatcher: $name"
done
for name in libGLX_mesa.so.0 libEGL_mesa.so.0; do
    [[ -e "$OUTDIR/root/usr/lib/$name" ]] || die "missing Mesa GLVND vendor library: $name"
done
[[ -f "$OUTDIR/root/usr/share/glvnd/egl_vendor.d/50_mesa.json" ]] || die "missing Mesa EGL vendor manifest"
tar -C "$OUTDIR/root" -cf "$OUTDIR/root.tar" .
find "$OUTDIR/root/usr/lib" -maxdepth 1 -name 'libgallium*' -o -name 'zink_dri.so' -o -name 'libvulkan_virtio.so' | sort
stamp_write "$NAME" "$INPUT" "$OUTDIR/root.tar"
