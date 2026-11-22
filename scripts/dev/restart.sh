#!/bin/bash
# Rebuild what changed, repackage the app and restart the VM headlessly.
# Use for host-side or guest-layer changes (virglrenderer, d3dmetal, libkrun,
# guest integration, ...); Neptune-only changes can use push-neptune.sh.
#
#   scripts/dev/restart.sh               incremental build + package + restart
#   scripts/dev/restart.sh --no-build    restart only
source "$(dirname "$0")/lib.sh"

BUILD=1
case "${1:-}" in
    "") ;;
    --no-build) BUILD=0 ;;
    *) fail "usage: $0 [--no-build]" ;;
esac

build_all() {
    local c list
    list="$("$DEV_ROOT/build.sh" --list-components)" && [[ -n "$list" ]] || return 1
    for c in $list; do
        "$DEV_ROOT/build.sh" --component "$c" || return 1
    done
}
vm_app_exited() { # RUN_DIR — no VM app process and no gvproxy for RUN_DIR left.
    [[ -z "$(app_pids)" ]] && ! pgrep -f -- "-pid-file $1/" >/dev/null
}
shutdown_vm() {
    local run; run="$(live_run_dir)"
    if [[ -z "$run" ]]; then
        [[ -z "$(app_pids)" ]] || fail "a VM app is running but no live run dir was found"
        say "VM not running"
        return
    fi
    fifo_send "$run" shutdown || fail "could not write $run/control.fifo"
    # The live VM may still be a pre-rename AppleDeck build: wait for whichever app owns it.
    wait_for 120 vm_app_exited "$run" || fail "the VM app did not exit within 120s"
}
start_vm() {
    open -a "$DEV_APP" --args --start-vm || return 1
    wait_for 240 guest_up || fail "guest SSH not ready within 240s"
}

(( BUILD == 0 )) || step "build" build_all
step "shutdown VM" shutdown_vm
(( BUILD == 0 )) || step "package" "$DEV_ROOT/scripts/package.sh"
step "start VM" start_vm
say "backend: $(guest cat /run/brinewell/graphics-backend 2>/dev/null || echo unknown)"
