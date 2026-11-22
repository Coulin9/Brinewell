#!/bin/bash
# Push the current Neptune build to the running guest as the dev payload.
# Takes effect on the next game launch; no layer rebuild or VM restart.
#
#   scripts/dev/push-neptune.sh          incremental build + push + harness sync
#   scripts/dev/push-neptune.sh --off    remove the dev payload (packaged again)
source "$(dirname "$0")/lib.sh"

guest_up || fail "guest SSH is not reachable; start the VM first"

if [[ "${1:-}" == --off ]]; then
    guest "rm -rf ~/$GUEST_DEV_PAYLOAD ~/$GUEST_DEV_PAYLOAD.new"
    say "dev payload removed"
    sync_harnesses
    exit 0
fi
[[ $# -eq 0 ]] || fail "usage: $0 [--off]"

step "build neptune" "$DEV_ROOT/build.sh" --component neptune
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
tar -C "$stage" -xf "$DEV_OUT/neptune/root.tar"
payload="$stage/usr/lib/brinewell/neptune"
[[ -f "$payload/payload.json" ]] || fail "payload.json missing from neptune root.tar"
upload() {
    COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -C "$payload" -cf - . | guest "set -e; d=~/$GUEST_DEV_PAYLOAD
        rm -rf \$d.new; mkdir -p \$d.new; tar -xf - -C \$d.new
        rm -rf \$d; mv \$d.new \$d"
}
step "upload payload" upload
step "sync harnesses" sync_harnesses
say "dev payload active: $(python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));print(d.get("build_input","?")[:12])' "$payload/payload.json") (relaunch the game to pick it up)"
