#!/bin/bash
# Build pinned gvproxy with Brinewell's outbound TCP, UDP and DNS policy.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=gvproxy
OUTDIR="$OUT/$NAME"
NETWORK="$ROOT/runtime/network"
VERSION="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["gvproxy"]["version"])' "$ROOT/deps.lock.json")"
URL="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["gvproxy"]["url"])' "$ROOT/deps.lock.json")"
SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["gvproxy"]["sha256"])' "$ROOT/deps.lock.json")"
require_cmd go patch
SOURCE_DIGEST="$(find "$NETWORK" -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | awk '{print $1}')"
INPUT="$(input_digest "$NAME" version "$VERSION" sha "$SHA" network "$SOURCE_DIGEST" recipe "$(sha256_file "$0")" go "$(go version)")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/gvproxy"; then exit 0; fi

ARCHIVE="$CACHE/gvisor-tap-vsock-$VERSION.tar.gz"
fetch_verified "$URL" "$ARCHIVE" "$SHA"
SOURCE="$SRC/gvisor-tap-vsock-$VERSION"
rm -rf "$SOURCE"
mkdir -p "$SOURCE" "$OUTDIR"
tar -xzf "$ARCHIVE" --strip-components=1 -C "$SOURCE"
patch -d "$SOURCE" -p1 < "$NETWORK/gvproxy.patch"
cp -R "$NETWORK" "$SOURCE/brinewellnetwork"
cd "$SOURCE"
export GOCACHE="$BUILD_ROOT/go-cache"
export GOMODCACHE="$BUILD_ROOT/go-mod"
go mod edit -require=brinewell/network@v0.0.0 -replace=brinewell/network=./brinewellnetwork
go build -mod=mod -trimpath -ldflags "-s -w -X github.com/containers/gvisor-tap-vsock/pkg/types.gitVersion=$VERSION-brinewell" \
    -o "$OUTDIR/gvproxy.new" ./cmd/gvproxy
codesign --force --sign - "$OUTDIR/gvproxy.new"
mv -f "$OUTDIR/gvproxy.new" "$OUTDIR/gvproxy"
version="$("$OUTDIR/gvproxy" --version)"
[[ "$version" == *"$VERSION"* ]] || die "unexpected gvproxy version: $version"
stamp_write "$NAME" "$INPUT" "$OUTDIR/gvproxy"
