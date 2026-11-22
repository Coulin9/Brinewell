#!/bin/bash
# Build the standalone performance overlay for the SteamOS guest.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=mangoapp
OUTDIR="$OUT/$NAME"
PATCHES="$ROOT/runtime/patches/mangohud"
DOCKERFILE="$ROOT/scripts/containers/mangoapp.Dockerfile"
read -r VERSION URL SHA < <(python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["mangohud"]; print(d["version"],d["url"],d["sha256"])' "$ROOT/deps.lock.json")
PATCH_SHA="$(shasum -a 256 "$PATCHES"/*.patch | shasum -a 256 | awk '{print $1}')"
DOCKER_SHA="$(sha256_file "$DOCKERFILE")"
# The MangoApp builder is FROM the guest builder, so both Dockerfiles key it.
BASE_SHA="$(sha256_file "$ROOT/scripts/containers/guest.Dockerfile")"
INPUT="$(input_digest "$NAME" source "$SHA" patches "$PATCH_SHA" dockerfile "$DOCKER_SHA" base_dockerfile "$BASE_SHA")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/mangoapp" "$OUTDIR/LICENSE"; then exit 0; fi

ARCHIVE="$CACHE/MangoHud-$VERSION-Source.tar.xz"
fetch_verified "$URL" "$ARCHIVE" "$SHA"
require_builder
ensure_builder_image brinewell-guest-builder "$ROOT/scripts/containers/guest.Dockerfile" "$ROOT/scripts/containers"
IMAGE="brinewell-mangoapp-builder:${DOCKER_SHA:0:12}-${BASE_SHA:0:12}"
if ! builder image exists "$IMAGE"; then
    builder build --tag "$IMAGE" -f "$DOCKERFILE" "$ROOT/scripts/containers"
fi
mkdir -p "$OUTDIR"
builder run --rm -v "$ARCHIVE:/mangohud.tar.xz:ro" -v "$PATCHES:/patches:ro" -v "$OUTDIR:/out" \
    "$IMAGE" bash -euc '
        mkdir /src
        tar -xf /mangohud.tar.xz --strip-components=1 -C /src
        cd /src
        for p in /patches/*.patch; do patch -p1 -F0 < "$p"; done
        meson setup build --buildtype=release --wrap-mode=nodownload -Dmangoapp=true \
            -Dwith_nvml=disabled -Dwith_xnvctrl=disabled -Dtests=disabled -Dmangoplot=disabled
        ninja -C build -j6 src/mangoapp
        install -m 0755 build/src/mangoapp /out/mangoapp
        {
            cat LICENSE
            for license in subprojects/imgui-*/LICENSE.txt subprojects/implot-*/LICENSE subprojects/spdlog-*/LICENSE; do
                printf "\n%s\n\n" "$license"
                cat "$license"
            done
        } > /out/LICENSE
    '
[[ "$(file -b "$OUTDIR/mangoapp")" == *"ARM aarch64"* ]] || die "mangoapp is not aarch64"
stamp_write "$NAME" "$INPUT" "$OUTDIR/mangoapp" "$OUTDIR/LICENSE"
