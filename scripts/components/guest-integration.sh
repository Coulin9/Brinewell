#!/bin/bash
# Stage the Brinewell guest integration tree and verify it is brand-free.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/scripts/lib/common.sh"

NAME=guest-integration
SRC="$ROOT/runtime/guest/rootfs"
OUTDIR="$OUT/$NAME"
DIGEST="$(cd "$SRC" && python3 - <<'PY'
import hashlib
import os
import stat

paths = ['.']
for dirpath, dirnames, filenames in os.walk('.'):
    dirnames.sort()
    for name in sorted(dirnames + filenames):
        paths.append(os.path.join(dirpath, name))

h = hashlib.sha256()
for path in sorted(paths):
    st = os.lstat(path)
    if stat.S_ISDIR(st.st_mode):
        tag = 'dir'
    elif stat.S_ISLNK(st.st_mode):
        tag = 'link'
    else:
        tag = 'file'
    h.update(path.encode())
    h.update(b'\0')
    h.update(tag.encode())
    h.update(b'\0')
    h.update(oct(stat.S_IMODE(st.st_mode)).encode())
    h.update(b'\0')
    if tag == 'file':
        with open(path, 'rb') as f:
            h.update(f.read())
    elif tag == 'link':
        h.update(os.readlink(path).encode())
    h.update(b'\0')
print(h.hexdigest())
PY
)"
INPUT="$(input_digest "$NAME" tree "$DIGEST")"
if component_should_reuse "$NAME" "$INPUT" "$OUTDIR/rootfs.tar"; then exit 0; fi

if grep -RIl 'steamac\|Steamac\|STEAMAC' "$SRC" 2>/dev/null | head -1 | grep -q .; then
    grep -RIl 'steamac\|Steamac\|STEAMAC' "$SRC" >&2 || true
    die "guest integration still contains steamac branding"
fi
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
cp -R "$SRC" "$OUTDIR/rootfs"
tar -C "$OUTDIR/rootfs" -cf "$OUTDIR/rootfs.tar" .
stamp_write "$NAME" "$INPUT" "$OUTDIR/rootfs.tar"
