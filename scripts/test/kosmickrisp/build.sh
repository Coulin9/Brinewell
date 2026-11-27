#!/bin/sh
set -eu

# Build a self-contained test executable for the current architecture.
# Run it as: output/vk-draw-check KosmicKrisp all (or Venus in the guest).
src=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: build.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
work=$(mktemp -d "${TMPDIR:-/tmp}/vk-draw-check.XXXXXX")
trap 'rm -rf "$work"' EXIT
for shader in triangle.vert color.frag faces.vert faces.frag duplicate.geom conditional.geom store.geom primitive-id.frag primitive-id.geom depth-slope.vert; do
    if command -v glslc >/dev/null; then
        glslc --target-env=vulkan1.1 -O -mfmt=c "$src/$shader" -o "$work/$shader.inc"
    else
        glslangValidator -V --target-env vulkan1.1 "$src/$shader" -o "$work/shader.spv"
        python3 - "$work/shader.spv" > "$work/$shader.inc" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    print("{" + ",".join(hex(word) for word, in struct.iter_unpack("<I", f.read())) + "}")
PY
    fi
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -O2 \
    -I"$work" $(pkg-config --cflags vulkan) "$src/vk-draw-check.cpp" \
    $(pkg-config --libs vulkan) -o "$out/vk-draw-check"
