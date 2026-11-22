#!/bin/bash
# Build the guest agents in the Linux builder.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=agent
SRC="$ROOT/runtime/guest/agent"
OUTDIR="$OUT/$NAME"
SOURCE_DIGEST="$(cd "$SRC" && shasum -a 256 brinewell-agent.c brinewell-clipboard.cpp clipboard-port.h clipboard-x11.cpp | shasum -a 256 | awk '{print $1}')"
INPUT="$(input_digest "$NAME" source "$SOURCE_DIGEST" recipe "$(sha256_file "$0")" image "$BUILDER_IMAGE")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/brinewell-agent" "$OUTDIR/brinewell-clipboard"; then exit 0; fi

require_builder
BUILDER_IMAGE_TAG=brinewell-guest-builder
ensure_builder_image "$BUILDER_IMAGE_TAG" "$ROOT/scripts/containers/guest.Dockerfile" "$ROOT/scripts/containers"
mkdir -p "$OUTDIR"
builder run --rm -v "$SRC:/src:ro" \
    -v "$OUTDIR:/out" "$BUILDER_IMAGE_TAG" bash -lc '
    set -e
    gcc -O2 -static -Wall -Wextra -Werror -o /out/brinewell-agent /src/brinewell-agent.c
    cd /tmp
    protocol=/usr/share/wayland-protocols/staging/ext-data-control/ext-data-control-v1.xml
    wayland-scanner client-header "$protocol" ext-data-control-client.h
    wayland-scanner private-code "$protocol" ext-data-control.c
    gcc -O2 -c ext-data-control.c
    g++ -std=c++20 -O2 -Wall -Wextra -Werror -I. /src/brinewell-clipboard.cpp /src/clipboard-x11.cpp \
        ext-data-control.o -o /out/brinewell-clipboard $(pkg-config --cflags --libs wayland-client xcb xcb-xfixes)
'
file "$OUTDIR/brinewell-agent" "$OUTDIR/brinewell-clipboard"
stamp_write "$NAME" "$INPUT" "$OUTDIR/brinewell-agent" "$OUTDIR/brinewell-clipboard"
