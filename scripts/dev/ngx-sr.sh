#!/bin/bash
# NGX SuperSampling fixture driver: runs the dx12-lab/ngx-sr-v1 harness
# inside the running guest against the effective Neptune payload.
#
#   scripts/dev/ngx-sr.sh --fixture --ring single
#   scripts/dev/ngx-sr.sh --fixture --ring multi   (NPT_PERF=multi_ring)
#   scripts/dev/ngx-sr.sh --loader-check         (alias: --deployment-check)
#   scripts/dev/ngx-sr.sh --wrapper-check
#   scripts/dev/ngx-sr.sh --help
#
# --fixture syncs the effective payload's nvngx.dll/nvapi64.dll and the
# freshly cross-compiled fixture exe into the private guest directory
# ~/dx12-lab/ngx-sr-v1, prepares its private prefix (nptunix.dll), sets
# BRINEWELL_NGX_SR_ACTIVE=1 + BRINEWELL_NEPTUNE_BACKEND=d3dmetal and runs
# run-vm.sh over SSH.  --ring multi adds NPT_PERF=multi_ring and the
# fixture's --threads split-recording mode.
#
# --loader-check is a LOADER FIXTURE, not a product deployment proof: it
# hand-places the two payload DLLs into the private prefix's system32 and
# launches the fixture through Proton/Wine without an app-local copy
# (positive: the loaded module path must be that system32 and each hash
# must equal the effective payload — a mismatch fails nonzero; negative:
# with no NGX DLL reachable the fixture must fail exactly at
# "nvngx.dll load/resolve", any other failure is not accepted).
#
# --wrapper-check exercises the REAL brinewell-graphics-wrap injection
# code in the guest (imported as a module; a full bwrap/pressure-vessel
# boundary cannot be launched without Steam): it runs _injection for an
# opt-in and an opt-out launch against a scratch compatdata, then runs the
# fixture with WINEPREFIX pointed at the wrapper-managed prefix (positive:
# module path inside that prefix's system32, hashes equal the payload;
# negative: after the opt-out cleanup the expected load failure only).
# It requires the guest backend mode file to read "neptune" and the
# installed Proton to offer the x86-64 layout.
source "$(dirname "$0")/lib.sh"

FIXTURE_HOST_DIR="$WORKSPACE/dx12-lab/ngx-sr-v1"
FIXTURE_GUEST_DIR='~/dx12-lab/ngx-sr-v1'
EVIDENCE_DIR="$FIXTURE_HOST_DIR/evidence"

usage() {
    sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

MODE=""
RING="single"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fixture) MODE=fixture ;;
        --loader-check) MODE=loader ;;
        --deployment-check)
            say "--deployment-check is now --loader-check (loader fixture)"
            MODE=loader ;;
        --wrapper-check) MODE=wrapper ;;
        --ring)
            shift
            [[ "${1:-}" == single || "${1:-}" == multi ]] ||
                fail "--ring needs single|multi"
            RING="$1" ;;
        --help|-h) usage 0 ;;
        *) fail "unknown argument: $1 (try --help)" ;;
    esac
    shift
done
[[ -n "$MODE" ]] || usage 1

guest_up || fail "guest SSH is not reachable; start the VM first"

# Push the built exe + scripts and the effective payload's NGX DLLs into
# the private guest harness directory, and prepare its private prefix.
sync_fixture() {
    [[ -f "$FIXTURE_HOST_DIR/out/ngx-sr-fixture.exe" ]] ||
        fail "fixture exe missing; run $FIXTURE_HOST_DIR/build.sh first"
    guest "mkdir -p $FIXTURE_GUEST_DIR/out"
    # scp uses -P for the port; reuse the SSH options with the flag swapped.
    local SCP_OPTS=("${SSH_OPTS[@]}")
    for i in "${!SCP_OPTS[@]}"; do
        [[ "${SCP_OPTS[$i]}" == -p ]] && SCP_OPTS[$i]=-P
    done
    scp "${SCP_OPTS[@]}" -q \
        "$FIXTURE_HOST_DIR/out/ngx-sr-fixture.exe" \
        "steamos@127.0.0.1:dx12-lab/ngx-sr-v1/out/ngx-sr-fixture.exe"
    scp "${SCP_OPTS[@]}" -q \
        "$FIXTURE_HOST_DIR/run-vm.sh" "$FIXTURE_HOST_DIR/check.py" \
        "steamos@127.0.0.1:dx12-lab/ngx-sr-v1/"
    guest bash -s <<'SH'
set -euo pipefail
d=~/dx12-lab/ngx-sr-v1
mkdir -p "$d/out" "$d/pfx/drive_c/windows/system32"
chmod +x "$d/run-vm.sh"
dev="$HOME/.local/share/brinewell/neptune-dev"
packaged=/usr/lib/brinewell/neptune
payload=$packaged
[[ -f "$dev/payload.json" ]] && payload=$dev
win="$payload/x86_64-windows"
# All payload DLLs go next to the fixture exe so the run needs no manual
# copies: the exe directory is always on Wine's module search path.
for name in nvngx nvapi64 d3d11 dxgi d3d12; do
    [[ -f "$win/$name.dll" ]] || { echo "payload $payload lacks $name.dll" >&2; exit 1; }
    cp -f "$win/$name.dll" "$d/out/$name.dll"
done
cp -f "$win/nptunix.dll" "$d/pfx/drive_c/windows/system32/nptunix.dll"
# Drop stale copies at the harness root: WINEDLLPATH still lists it and an
# outdated DLL there would shadow the fresh payload.
rm -f "$d"/{nvngx,nvapi64,d3d11,dxgi,d3d12}.dll
echo "fixture synced from $payload"
sha256sum "$d"/out/nvngx.dll "$d"/out/nvapi64.dll
SH
}

run_fixture() { # EXTRA_ENV...
    local extra="$*"
    mkdir -p "$EVIDENCE_DIR/last"
    local rc=0
    guest "cd $FIXTURE_GUEST_DIR && \
        BRINEWELL_NGX_SR_ACTIVE=1 BRINEWELL_NEPTUNE_BACKEND=d3dmetal \
        $extra timeout 300 ./run-vm.sh $ARGS" \
        >"$EVIDENCE_DIR/last/fixture-$RING.out" \
        2>"$EVIDENCE_DIR/last/fixture-$RING.err" || rc=$?
    echo "$rc" >"$EVIDENCE_DIR/last/fixture-$RING.rc"
    return "$rc"
}

case "$MODE" in
fixture)
    ARGS=""
    ENV_EXTRA=""
    if [[ "$RING" == multi ]]; then
        ARGS="--threads"
        ENV_EXTRA="NPT_PERF=multi_ring"
    fi
    step "build fixture exe" "$FIXTURE_HOST_DIR/build.sh"
    step "sync fixture + payload NGX DLLs" sync_fixture
    step "ngx-sr fixture (ring=$RING)" run_fixture "$ENV_EXTRA" ||
        say "fixture exited nonzero; see $EVIDENCE_DIR/last/fixture-$RING.err"
    python3 "$FIXTURE_HOST_DIR/check.py" \
        "$EVIDENCE_DIR/last/fixture-$RING.out" || {
            say "guest stderr tail:"; tail -20 "$EVIDENCE_DIR/last/fixture-$RING.err" >&2 || true
            exit 1; }
    ;;

loader)
    step "build fixture exe" "$FIXTURE_HOST_DIR/build.sh"
    step "sync fixture + payload NGX DLLs" sync_fixture
    say "positive: system32 NGX DLLs + ACTIVE env, no app-local DLL"
    rc_pos=0
    guest "cd $FIXTURE_GUEST_DIR && \
        cp -f out/nvngx.dll out/nvapi64.dll pfx/drive_c/windows/system32/ && \
        rm -f out/nvngx.dll out/nvapi64.dll nvngx.dll nvapi64.dll; \
        WINEDLLOVERRIDES='d3d11,dxgi,d3d12=n;nptunix=b;nvngx,nvapi64=n' \
        BRINEWELL_NGX_SR_ACTIVE=1 BRINEWELL_NEPTUNE_BACKEND=d3dmetal \
        timeout 300 ./run-vm.sh" \
        >"$EVIDENCE_DIR/last/deploy-pos.out" \
        2>"$EVIDENCE_DIR/last/deploy-pos.err" || rc_pos=$?
    path_pos=$(grep -o 'loaded module: .*' \
        "$EVIDENCE_DIR/last/deploy-pos.out" | head -1 || true)
    say "positive loaded path: ${path_pos:-<none>} (rc=$rc_pos)"
    if (( rc_pos != 0 )); then
        fail "positive loader run failed (rc=$rc_pos); see $EVIDENCE_DIR/last/deploy-pos.err"
    fi
    if [[ "$path_pos" != *system32* ]]; then
        fail "positive run did not load nvngx.dll from the prefix system32: ${path_pos:-<none>}"
    fi
    # Each placed DLL's hash must equal the effective payload's.
    hash_sys32=$(guest "cd $FIXTURE_GUEST_DIR && \
        sha256sum pfx/drive_c/windows/system32/nvngx.dll \
                  pfx/drive_c/windows/system32/nvapi64.dll | cut -d' ' -f1") ||
        fail "cannot hash the prefix system32 NGX DLLs"
    hash_payload=$(guest "dev=\$HOME/.local/share/brinewell/neptune-dev; \
        p=/usr/lib/brinewell/neptune; [[ -f \$dev/payload.json ]] && p=\$dev; \
        sha256sum \$p/x86_64-windows/nvngx.dll \
                  \$p/x86_64-windows/nvapi64.dll | cut -d' ' -f1") ||
        fail "cannot hash the effective payload NGX DLLs"
    say "system32 hashes: $(echo "$hash_sys32" | tr '\n' ' ')"
    say "payload   hashes: $(echo "$hash_payload" | tr '\n' ' ')"
    if [[ "$hash_sys32" != "$hash_payload" ]]; then
        fail "loaded NGX DLLs do not match the effective payload"
    fi

    say "negative: no NGX DLL anywhere reachable"
    rc_neg=0
    guest "cd $FIXTURE_GUEST_DIR && \
        rm -f pfx/drive_c/windows/system32/nvngx.dll \
              pfx/drive_c/windows/system32/nvapi64.dll \
              out/nvngx.dll out/nvapi64.dll nvngx.dll nvapi64.dll && \
        WINEDLLOVERRIDES='d3d11,dxgi,d3d12=n;nptunix=b' \
        timeout 300 ./run-vm.sh" \
        >"$EVIDENCE_DIR/last/deploy-neg.out" \
        2>"$EVIDENCE_DIR/last/deploy-neg.err" || rc_neg=$?
    path_neg=$(grep -o 'loaded module: .*' \
        "$EVIDENCE_DIR/last/deploy-neg.out" | head -1 || true)
    fail_neg=$(grep 'NGXSR: FAIL' \
        "$EVIDENCE_DIR/last/deploy-neg.out" | head -1 || true)
    say "negative result: ${fail_neg:-<none>} (rc=$rc_neg)"
    # restore app-local DLLs for later fixture runs
    guest "cd $FIXTURE_GUEST_DIR && dev=\$HOME/.local/share/brinewell/neptune-dev; \
        p=/usr/lib/brinewell/neptune; [[ -f \$dev/payload.json ]] && p=\$dev; \
        cp -f \$p/x86_64-windows/nvngx.dll \$p/x86_64-windows/nvapi64.dll out/" \
        2>/dev/null || true
    if (( rc_neg == 0 )); then
        fail "negative run unexpectedly passed"
    fi
    if [[ -n "$path_neg" ]]; then
        fail "negative run still loaded an NGX module: $path_neg"
    fi
    if [[ "$fail_neg" != *"ngx.resolve()"*load/resolve* ]]; then
        fail "negative run failed for an unexpected reason (rc=$rc_neg): ${fail_neg:-<no FAIL line>}; see $EVIDENCE_DIR/last/deploy-neg.err"
    fi
    say "loader check passed: positive loaded $path_pos"
    ;;

wrapper)
    step "build fixture exe" "$FIXTURE_HOST_DIR/build.sh"
    step "sync fixture + payload NGX DLLs" sync_fixture
    # Phase 1: real wrapper _injection installs into a scratch compatdata.
    step "wrapper-check: real _injection (opt-in)" guest bash -s <<'SH' || fail "wrapper opt-in injection failed; see output above"
set -euo pipefail
d=~/dx12-lab/ngx-sr-v1
compat="$d/wrap-compat"
rm -rf "$compat"
mkdir -p "$compat"
[[ "$(cat /run/brinewell/graphics-backend 2>/dev/null || true)" == neptune ]] || {
    echo "guest backend is not 'neptune'; NGX SR cannot activate" >&2
    exit 2
}
python3 - "$compat" <<'PY'
import importlib.machinery, importlib.util, os, sys
compat = sys.argv[1]
loader = importlib.machinery.SourceFileLoader(
    "graphics_wrap", "/usr/lib/brinewell/brinewell-graphics-wrap")
spec = importlib.util.spec_from_loader(loader.name, loader)
wrap = importlib.util.module_from_spec(spec)
loader.exec_module(wrap)
proton = os.path.expanduser(
    "~/.local/share/Steam/steamapps/common/Proton 11.0")
env = {"STEAM_COMPAT_TOOL_PATHS": proton,
       "STEAM_COMPAT_APP_ID": "1",
       "STEAM_COMPAT_DATA_PATH": compat,
       "BRINEWELL_NGX_SR": "1"}
cmd = ["FEXCompatTool", proton + "/proton",
       "waitforexitandrun", "/game/x.exe"]
inject = wrap._injection(wrap._mode(), cmd, dict(env))
assert inject, "wrapper produced no injection (unsupported tool/arch?)"
i = inject.index("BRINEWELL_NGX_SR_ACTIVE")
assert inject[i - 1] == "--setenv" and inject[i + 1] == "1", inject
assert "nvngx=n" in inject[inject.index("WINEDLLOVERRIDES") + 1], inject
sys32 = os.path.join(compat, "pfx/drive_c/windows/system32")
for name in ("nvngx.dll", "nvapi64.dll"):
    assert os.path.isfile(os.path.join(sys32, name)), name
assert os.path.isfile(os.path.join(compat, ".brinewell-ngx-sr.json"))
print("WRAPCHECK: opt-in injection installed managed DLLs + marker")
PY
SH
    say "positive: fixture inside the wrapper-managed prefix"
    rc_pos=0
    guest "cd $FIXTURE_GUEST_DIR && \
        rm -f out/nvngx.dll out/nvapi64.dll nvngx.dll nvapi64.dll; \
        WINEPREFIX=\$PWD/wrap-compat/pfx \
        WINEDLLOVERRIDES='d3d11,dxgi,d3d12=n;nptunix=b;nvngx,nvapi64=n' \
        BRINEWELL_NGX_SR_ACTIVE=1 BRINEWELL_NEPTUNE_BACKEND=d3dmetal \
        timeout 300 ./run-vm.sh" \
        >"$EVIDENCE_DIR/last/wrap-pos.out" \
        2>"$EVIDENCE_DIR/last/wrap-pos.err" || rc_pos=$?
    path_pos=$(grep -o 'loaded module: .*' \
        "$EVIDENCE_DIR/last/wrap-pos.out" | head -1 || true)
    say "positive loaded path: ${path_pos:-<none>} (rc=$rc_pos)"
    if (( rc_pos != 0 )); then
        fail "positive wrapper run failed (rc=$rc_pos); see $EVIDENCE_DIR/last/wrap-pos.err"
    fi
    if ! printf '%s' "$path_pos" | grep -qi 'windows.system32.nvngx\.dll'; then
        fail "positive run did not load nvngx.dll from the prefix system32: ${path_pos:-<none>}"
    fi
    hash_sys32=$(guest "cd $FIXTURE_GUEST_DIR && \
        sha256sum wrap-compat/pfx/drive_c/windows/system32/nvngx.dll \
                  wrap-compat/pfx/drive_c/windows/system32/nvapi64.dll | cut -d' ' -f1") ||
        fail "cannot hash the wrapper-installed NGX DLLs"
    hash_payload=$(guest "dev=\$HOME/.local/share/brinewell/neptune-dev; \
        p=/usr/lib/brinewell/neptune; [[ -f \$dev/payload.json ]] && p=\$dev; \
        sha256sum \$p/x86_64-windows/nvngx.dll \
                  \$p/x86_64-windows/nvapi64.dll | cut -d' ' -f1") ||
        fail "cannot hash the effective payload NGX DLLs"
    if [[ "$hash_sys32" != "$hash_payload" ]]; then
        fail "wrapper-installed NGX DLLs do not match the effective payload"
    fi
    # Phase 2: opt-out injection cleans the managed files, then the
    # fixture must fail exactly at the expected load step.
    step "wrapper-check: real _injection (opt-out)" guest bash -s <<'SH' || fail "wrapper opt-out injection failed; see output above"
set -euo pipefail
d=~/dx12-lab/ngx-sr-v1
# Stop the leftover wineserver so the wrapper's live-process guard lets
# this opt-out launch clean the prefix.
proton="$HOME/.local/share/Steam/steamapps/common/Proton 11.0"
WINEPREFIX="$d/wrap-compat/pfx" "$proton/files/bin/wineserver" -kw \
    >/dev/null 2>&1 || true
python3 - "$d/wrap-compat" <<'PY'
import importlib.machinery, importlib.util, os, sys
compat = sys.argv[1]
loader = importlib.machinery.SourceFileLoader(
    "graphics_wrap", "/usr/lib/brinewell/brinewell-graphics-wrap")
spec = importlib.util.spec_from_loader(loader.name, loader)
wrap = importlib.util.module_from_spec(spec)
loader.exec_module(wrap)
proton = os.path.expanduser(
    "~/.local/share/Steam/steamapps/common/Proton 11.0")
env = {"STEAM_COMPAT_TOOL_PATHS": proton,
       "STEAM_COMPAT_APP_ID": "1",
       "STEAM_COMPAT_DATA_PATH": compat,
       "BRINEWELL_NGX_SR_ACTIVE": "1"}  # inherited value must not leak
cmd = ["FEXCompatTool", proton + "/proton",
       "waitforexitandrun", "/game/x.exe"]
inject = wrap._injection(wrap._mode(), cmd, dict(env))
assert inject, "wrapper produced no injection"
i = inject.index("--unsetenv")
assert inject[i + 1] == "BRINEWELL_NGX_SR_ACTIVE", inject
sys32 = os.path.join(compat, "pfx/drive_c/windows/system32")
for name in ("nvngx.dll", "nvapi64.dll"):
    assert not os.path.exists(os.path.join(sys32, name)), name
assert not os.path.exists(os.path.join(compat, ".brinewell-ngx-sr.json"))
print("WRAPCHECK: opt-out cleaned managed DLLs + marker")
PY
SH
    say "negative: fixture after wrapper cleanup"
    rc_neg=0
    guest "cd $FIXTURE_GUEST_DIR && \
        WINEPREFIX=\$PWD/wrap-compat/pfx \
        WINEDLLOVERRIDES='d3d11,dxgi,d3d12=n;nptunix=b' \
        timeout 300 ./run-vm.sh" \
        >"$EVIDENCE_DIR/last/wrap-neg.out" \
        2>"$EVIDENCE_DIR/last/wrap-neg.err" || rc_neg=$?
    path_neg=$(grep -o 'loaded module: .*' \
        "$EVIDENCE_DIR/last/wrap-neg.out" | head -1 || true)
    fail_neg=$(grep 'NGXSR: FAIL' \
        "$EVIDENCE_DIR/last/wrap-neg.out" | head -1 || true)
    say "negative result: ${fail_neg:-<none>} (rc=$rc_neg)"
    guest "cd $FIXTURE_GUEST_DIR && rm -rf wrap-compat && dev=\$HOME/.local/share/brinewell/neptune-dev; \
        p=/usr/lib/brinewell/neptune; [[ -f \$dev/payload.json ]] && p=\$dev; \
        cp -f \$p/x86_64-windows/nvngx.dll \$p/x86_64-windows/nvapi64.dll out/" \
        2>/dev/null || true
    if (( rc_neg == 0 )); then
        fail "negative run unexpectedly passed"
    fi
    if [[ -n "$path_neg" ]]; then
        fail "negative run still loaded an NGX module: $path_neg"
    fi
    if [[ "$fail_neg" != *"ngx.resolve()"*load/resolve* ]]; then
        fail "negative run failed for an unexpected reason (rc=$rc_neg): ${fail_neg:-<no FAIL line>}; see $EVIDENCE_DIR/last/wrap-neg.err"
    fi
    say "wrapper check passed: positive loaded $path_pos"
    ;;
esac
