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
    local deadline=$((SECONDS + 240)) ready_since= run=
    while ! guest_up; do
        (( SECONDS < deadline )) || fail "guest SSH not ready within 240s"
        run="$(live_run_dir)" || run=
        if [[ -n "$run" ]] && { grep -qF 'BRINEWELL state running' "$run/progress.log" 2>/dev/null ||
                                 grep -qF 'BRINEWELL state running' "$run/host.log" 2>/dev/null; }; then
            if nc -z 127.0.0.1 2222 >/dev/null 2>&1; then
                ready_since=
            elif [[ -z "$ready_since" ]]; then
                ready_since=$SECONDS
            elif (( SECONDS - ready_since >= 20 )); then
                fail "VM is running, but host port 2222 is not listening. Guest SSH may be disabled in settings or in the guest; enable SSH before running development scripts that require it. Logs: $run/progress.log, $run/host.log"
            fi
        else
            ready_since=
        fi
        sleep 2
    done
}

(( BUILD == 0 )) || step "build" build_all
step "shutdown VM" shutdown_vm
(( BUILD == 0 )) || step "package" "$DEV_ROOT/scripts/package.sh"
step "start VM" start_vm
say "backend: $(guest cat /run/brinewell/graphics-backend 2>/dev/null || echo unknown)"
