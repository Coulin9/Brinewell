#!/bin/bash
# Build the static aarch64 guest PE classifier used by strict routing.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=neptune-probe
SRC="$ROOT/runtime/guest/tools/brinewell-api-probe.c"
GPU_SRC="$ROOT/runtime/guest/tools/brinewell-gpu-probe.c"
OUTDIR="$OUT/$NAME"
[[ -f "$SRC" && -f "$GPU_SRC" ]] || die "missing guest probe source"
INPUT="$(input_digest "$NAME" source "$(sha256_file "$SRC")" gpu "$(sha256_file "$GPU_SRC")" image "$BUILDER_IMAGE" config static-aarch64-linux)"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/brinewell-api-probe" "$OUTDIR/brinewell-gpu-probe"; then exit 0; fi

require_builder
BUILDER_IMAGE_TAG=brinewell-guest-builder
ensure_builder_image "$BUILDER_IMAGE_TAG" "$ROOT/scripts/containers/guest.Dockerfile" "$ROOT/scripts/containers"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
builder run --rm \
    -v "$ROOT/runtime/guest/tools:/src:ro" \
    -v "$OUTDIR:/out:rw" \
    "$BUILDER_IMAGE_TAG" \
    bash -lc 'gcc -O2 -static -Wall -Wextra -Werror -o /out/brinewell-api-probe /src/brinewell-api-probe.c && gcc -O2 -static -Wall -Wextra -Werror -o /out/brinewell-gpu-probe /src/brinewell-gpu-probe.c'
file "$OUTDIR/brinewell-api-probe" "$OUTDIR/brinewell-gpu-probe"
[[ "$(file -b "$OUTDIR/brinewell-api-probe")" == *"ARM aarch64"* ]] || die "api probe is not aarch64"
[[ "$(file -b "$OUTDIR/brinewell-gpu-probe")" == *"ARM aarch64"* ]] || die "gpu probe is not aarch64"
stamp_write "$NAME" "$INPUT" "$OUTDIR/brinewell-api-probe" "$OUTDIR/brinewell-gpu-probe"
