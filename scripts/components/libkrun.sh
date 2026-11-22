#!/bin/bash
# Build libkrun without the init blob; Brinewell supplies its own kernel/initramfs.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=libkrun
SRC="$ROOT/../libkrun"
OUTDIR="$OUT/$NAME"
[[ -f "$OUT/virglrenderer/libvirglrenderer.1.dylib" ]] || die "virglrenderer must be built first"
ensure_rust
INPUT="$(input_digest "$NAME" source "$(git_state_digest "$SRC")" rust "$(rustc --version)" features gpu,blk,net,input,snd virgl "$(sha256_file "$OUT/virglrenderer/libvirglrenderer.1.dylib")")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/libkrun.1.dylib"; then exit 0; fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
export CARGO_TARGET_DIR="$OUTDIR/target"
export PKG_CONFIG_PATH="$OUT/virglrenderer/prefix/lib/pkgconfig:$OUT/libepoxy/prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export RUSTFLAGS="-L native=$OUT/virglrenderer -L native=$OUT/libepoxy"
export LIBCLANG_PATH="${LIBCLANG_PATH:-/Library/Developer/CommandLineTools/usr/lib}"
cd "$SRC"
cargo build --release --locked --no-default-features \
    --features gpu,blk,net,input,snd -p libkrun
cp "$CARGO_TARGET_DIR/release/libkrun.dylib" "$OUTDIR/libkrun.1.dylib"
install_name "$OUTDIR/libkrun.1.dylib" libkrun.1.dylib
install_name_tool -add_rpath @loader_path "$OUTDIR/libkrun.1.dylib"
sign "$OUTDIR/libkrun.1.dylib"
ln -sf ../virglrenderer/libvirglrenderer.1.dylib "$OUTDIR/libvirglrenderer.1.dylib"
ln -sf ../libepoxy/libepoxy.0.dylib "$OUTDIR/libepoxy.0.dylib"
stamp_write "$NAME" "$INPUT" "$OUTDIR/libkrun.1.dylib"
