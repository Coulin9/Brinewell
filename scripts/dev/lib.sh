#!/bin/bash
# Shared helpers for the scripts/dev fast-iteration commands.
set -euo pipefail

DEV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKSPACE="$(cd "$DEV_ROOT/.." && pwd)"
DEV_OUT="$DEV_ROOT/.build/out"
DEV_APP="$DEV_ROOT/build/Brinewell.app"
# Guest path (relative to the guest home) read by brinewell-graphics-wrap.
GUEST_DEV_PAYLOAD=.local/share/brinewell/neptune-dev
SSH_OPTS=(-F /dev/null -i "$HOME/.ssh/steamos_ed25519" -p 2222 -o BatchMode=yes
          -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5)

say()  { printf '\033[1;36m[dev]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[dev]\033[0m %s\n' "$*" >&2; exit 1; }

guest() { ssh "${SSH_OPTS[@]}" steamos@127.0.0.1 "$@"; }
guest_up() { guest true >/dev/null 2>&1; }

# step LABEL CMD... — run CMD and report its wall time.
# Note: CMD runs in an errexit-suspended context; multi-command functions
# passed here must return on failure explicitly.
step() {
    local label="$1"; shift
    local start=$SECONDS
    say "$label..."
    local rc=0
    "$@" || rc=$?
    if (( rc )); then
        say "$label FAILED (exit $rc) after $((SECONDS - start))s"
        return $rc
    fi
    say "$label done in $((SECONDS - start))s"
}

# Copy the effective Neptune payload (dev if present, else packaged) into the
# private ~/dx12-lab harnesses: same-named DLLs, prefix nptunix.dll and the
# payload path in their run-vm.sh WINEDLLPATH.
sync_harnesses() {
    guest bash -s <<'SH'
set -euo pipefail
dev="$HOME/.local/share/brinewell/neptune-dev"
packaged=/usr/lib/brinewell/neptune
payload=$packaged
[[ -f "$dev/payload.json" ]] && payload=$dev
win="$payload/x86_64-windows"
n=0
for d in "$HOME"/dx12-lab/*/; do
    [[ -d "$d" ]] || continue
    for name in d3d11 dxgi d3d12; do
        [[ -f "$d$name.dll" ]] && { cp -f "$win/$name.dll" "$d$name.dll"; n=$((n + 1)); }
    done
    # Only explicitly opted-in harnesses participate in the fourth DLL sync.
    if [[ -f "${d}.brinewell-sync-d3d10core" ]]; then
        core="${d}d3d10core.dll"
        stamp="${d}.brinewell-managed-d3d10core.sha256"
        if [[ -f "$win/d3d10core.dll" ]]; then
            rm -f "$stamp"
            cp -f "$win/d3d10core.dll" "$core"
            sha256sum "$core" | cut -d ' ' -f 1 > "$stamp"
            n=$((n + 1))
        else
            echo "warning: $payload lacks d3d10core.dll; checking managed harness copy in $d" >&2
            if [[ -f "$stamp" && ! -L "$stamp" && -f "$core" && ! -L "$core" ]] \
                    && recorded=$(<"$stamp") \
                    && actual=$(sha256sum "$core") \
                    && [[ "$recorded" == "${actual%% *}" ]]; then
                if rm -f "$core"; then
                    rm -f "$stamp" || echo "warning: cannot remove $stamp" >&2
                else
                    echo "warning: cannot remove managed $core" >&2
                fi
            fi
        fi
    fi
    sys32="${d}pfx/drive_c/windows/system32/nptunix.dll"
    [[ -f "$sys32" ]] && { cp -f "$win/nptunix.dll" "$sys32"; n=$((n + 1)); }
    if [[ -f "${d}run-vm.sh" ]]; then
        # Also rewrite legacy pre-rename payload paths (literal and expanded $HOME).
        sed -i -E "s#:($packaged|$dev|/usr/lib/appledeck/neptune|\$HOME/.local/share/appledeck/neptune-dev|$HOME/.local/share/appledeck/neptune-dev)\"#:$payload\"#" "${d}run-vm.sh"
    fi
done
echo "harnesses synced from $payload ($n files)"
SH
}

# The live run dir: the -pid-file directory of the Brinewell gvproxy that
# forwards guest SSH on 2222 (stale /tmp/brinewell-* dirs are ignored).
# During the rename transition a still-running older VM may use /tmp/appledeck-*.
live_run_dir() {
    pgrep -fl gvproxy 2>/dev/null | grep -e '/Brinewell.app/' -e '/AppleDeck.app/' | grep -e '-ssh-port 2222' \
        | sed -nE 's/.*-pid-file (\/tmp\/(brinewell|appledeck)-[^ ]+)\/gvproxy\.pid.*/\1/p' | sort -u | head -1
}

# PIDs of the VM app process: the current Brinewell.app executable or a
# still-running pre-rename AppleDeck.app one (either may own the live VM).
app_pids() { pgrep -f '/(Brinewell|AppleDeck)\.app/Contents/MacOS/' || true; }

# Write one command to control.fifo without blocking on a FIFO nobody reads.
fifo_send() { # RUN_DIR COMMAND
    python3 - "$1/control.fifo" "$2" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
os.write(fd, (sys.argv[2] + "\n").encode())
os.close(fd)
PY
}

wait_for() { # SECONDS CMD...
    local limit="$1"; shift
    local deadline=$((SECONDS + limit))
    until "$@"; do
        (( SECONDS < deadline )) || return 1
        sleep 2
    done
}
