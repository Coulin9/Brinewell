#!/bin/bash
# Stage the pinned Apple Silicon telemetry sampler and its license.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=macmon
OUTDIR="$OUT/$NAME"
read -r VERSION URL SHA < <(python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["macmon"]; print(d["version"],d["url"],d["sha256"])' "$ROOT/deps.lock.json")
INPUT="$(input_digest "$NAME" version "$VERSION" sha "$SHA")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/macmon" "$OUTDIR/LICENSE"; then exit 0; fi

ARCHIVE="$CACHE/macmon-$VERSION.tar.gz"
fetch_verified "$URL" "$ARCHIVE" "$SHA"
mkdir -p "$OUTDIR"
tar -xzf "$ARCHIVE" -C "$OUTDIR" macmon LICENSE
stamp_write "$NAME" "$INPUT" "$OUTDIR/macmon" "$OUTDIR/LICENSE"
