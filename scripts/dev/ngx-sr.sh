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
# ~/dx12-lab/ngx-sr-v1, prepares its private prefix (nptunix.dll plus
# the two NGX DLLs in its system32 — the payload's process-side check
# only accepts files visible there), passes the candidate env a real
# launch would issue (BRINEWELL_NGX_SR_CANDIDATE=1 + both SHA-256s,
# rebuilt from the staged files; BRINEWELL_NEPTUNE_BACKEND=d3dmetal) and
# runs run-vm.sh over SSH.  --ring multi adds NPT_PERF=multi_ring and
# the fixture's --threads split-recording mode.
#
# --loader-check is a LOADER FIXTURE, not a product deployment proof: it
# hand-places the two payload DLLs into the private prefix's system32 and
# launches the fixture through Proton/Wine without an app-local copy,
# under the same candidate env (positive: the loaded module path must be
# that system32 and each hash must equal the effective payload — a
# mismatch fails nonzero; negative: with no NGX DLL reachable the
# fixture must fail exactly at "nvngx.dll load/resolve", any other
# failure is not accepted).
#
# --wrapper-check replays the real launch order against the REAL
# brinewell-graphics-wrap code (imported as a module) and the real
# selected Proton, on a dedicated scratch compatdata:
#   1. wrapper _injection for a default-enabled launch: managed nvngx.dll +
#      marker land in the scratch prefix's system32, and the returned
#      bwrap args carry the nvapi64 ro-bind over the tool's copy source
#      plus BRINEWELL_NGX_SR_CANDIDATE=1 and the two expected SHA-256s
#      (BRINEWELL_NGX_SR_ACTIVE is always --unsetenv);
#   2. the nvapi64.dll step of Proton's setup_prefix is replayed:
#      unlink + copy into the prefix, where the copy source is the
#      ro-bind SOURCE from step 1 (what the tool path serves inside the
#      real container), exactly like try_copy's remove+copy;
#   3. the fixture runs with WINEPREFIX at the scratch prefix, only the
#      wrapper-issued env (candidate + digests + the merged
#      WINEDLLOVERRIDES) and no app-local NGX DLLs — ACTIVE must be
#      set by the payload's own process-side check, never by hand.
# This is replayed three times on the same prefix: unset, empty again
# (prefix reuse), then opt-out — where the next simulated setup restores
# the tool's stock nvapi64.dll (the ro-bind target path read without the
# bind), the wrapper removes its nvngx.dll/marker, and the fixture must
# fail exactly at "nvngx.dll load/resolve".
# Limits (cannot be done faithfully here): no real pressure-vessel/bwrap
# namespace is created, so the bind is simulated by copying its source;
# only the nvapi64.dll part of setup_prefix is replayed (other tracked
# files, registry and prefix-init steps are not); and no wineserver,
# Steam or pv-adverb session takes part.  It proves the deployment and
# the process-side digest gate, not SDK trust of the unsigned core.
# Requires the guest backend mode file to read "neptune" and the
# installed Proton to offer the x86-64 nvapi copy source.
source "$(dirname "$0")/lib.sh"

FIXTURE_HOST_DIR="$WORKSPACE/dx12-lab/ngx-sr-v1"
FIXTURE_GUEST_DIR='~/dx12-lab/ngx-sr-v1'
EVIDENCE_DIR="$FIXTURE_HOST_DIR/evidence"

usage() {
    sed -n '2,54p' "$0" | sed 's/^# \{0,1\}//'
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

# The candidate env a real enabled launch would carry, rebuilt from the
# two NGX DLLs currently staged in the private prefix's system32.  The
# process-side check hashes those same files, so digests computed here
# always match; ACTIVE itself is only ever set by the payload.
candidate_env() {
    guest "cd $FIXTURE_GUEST_DIR && \
        printf 'BRINEWELL_NGX_SR_CANDIDATE=1 BRINEWELL_NGX_CORE_SHA256=%s BRINEWELL_NGX_NVAPI_SHA256=%s' \
        \$(sha256sum pfx/drive_c/windows/system32/nvngx.dll | cut -d' ' -f1) \
        \$(sha256sum pfx/drive_c/windows/system32/nvapi64.dll | cut -d' ' -f1)"
}

run_fixture() { # EXTRA_ENV...
    local extra="$*" cand
    cand=$(candidate_env) || fail "cannot compute the candidate digests"
    mkdir -p "$EVIDENCE_DIR/last"
    local rc=0
    guest "cd $FIXTURE_GUEST_DIR && \
        $cand BRINEWELL_NEPTUNE_BACKEND=d3dmetal \
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
    step "stage NGX DLLs in the private prefix" \
        guest "cd $FIXTURE_GUEST_DIR && \
            cp -f out/nvngx.dll out/nvapi64.dll \
                  pfx/drive_c/windows/system32/ && \
            rm -f out/nvngx.dll out/nvapi64.dll nvngx.dll nvapi64.dll" ||
        fail "cannot stage NGX DLLs into the private prefix"
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
    say "positive: system32 NGX DLLs + candidate env, no app-local DLL"
    rc_pos=0
    guest "cd $FIXTURE_GUEST_DIR && \
        cp -f out/nvngx.dll out/nvapi64.dll pfx/drive_c/windows/system32/ && \
        rm -f out/nvngx.dll out/nvapi64.dll nvngx.dll nvapi64.dll; \
        WINEDLLOVERRIDES='d3d11,dxgi,d3d12=n;nptunix=b;nvngx,nvapi64=n' \
        $(candidate_env) BRINEWELL_NEPTUNE_BACKEND=d3dmetal \
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
    mkdir -p "$EVIDENCE_DIR/last"

    # Run the fixture in the scratch prefix.  "env" sources the
    # wrapper-issued candidate variables + merged WINEDLLOVERRIDES;
    # "noenv" gives it none of them.  NGX DLLs are never placed next to
    # the exe or at the harness root, so only the prefix system32 can
    # provide them.  BRINEWELL_NGX_SR_ACTIVE is never set by hand.
    wrap_run_fixture() { # TAG env|noenv
        local tag="$1" rc=0 envsrc=":"
        [[ "$2" == env ]] && envsrc="set -a; . ./wrap-env.sh; set +a"
        guest "cd $FIXTURE_GUEST_DIR && \
            rm -f out/nvngx.dll out/nvapi64.dll nvngx.dll nvapi64.dll; \
            $envsrc; \
            WINEPREFIX=\$PWD/wrap-compat/pfx BRINEWELL_NEPTUNE_BACKEND=d3dmetal \
            timeout 300 ./run-vm.sh" \
            >"$EVIDENCE_DIR/last/wrap-$tag.out" \
            2>"$EVIDENCE_DIR/last/wrap-$tag.err" || rc=$?
        echo "$rc" >"$EVIDENCE_DIR/last/wrap-$tag.rc"
        return "$rc"
    }

    # Default-enabled launch, replayed against the real wrapper: _injection, then
    # the nvapi64.dll unlink+copy Proton's setup_prefix performs, where
    # the copy source is the ro-bind's source (what the tool path serves
    # inside the container).  $1: fresh wipes the scratch compatdata,
    # reuse keeps it like a second launch of the same game.
    wrap_enabled() { # fresh|reuse
        guest bash -s -- "$1" <<'SH'
set -euo pipefail
d=~/dx12-lab/ngx-sr-v1
compat="$d/wrap-compat"
[[ "$(cat /run/brinewell/graphics-backend 2>/dev/null || true)" == neptune ]] || {
    echo "guest backend is not 'neptune'; NGX SR cannot activate" >&2
    exit 2
}
if [[ "$1" == fresh ]]; then
    rm -rf "$compat"
    mkdir -p "$compat"
fi
python3 - "$compat" "$d" "$1" <<'PY'
import importlib.machinery, importlib.util, json, os, sys
compat, d, run = sys.argv[1:]
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
       # Hostile leftovers: the wrapper must overwrite or clear these.
       "BRINEWELL_NGX_SR_ACTIVE": "1",
       "BRINEWELL_NGX_SR_CANDIDATE": "1",
       "BRINEWELL_NGX_CORE_SHA256": "0" * 64,
       "BRINEWELL_NGX_NVAPI_SHA256": "0" * 64}
if run == "reuse":
    env["BRINEWELL_NGX_SR"] = ""
cmd = ["FEXCompatTool", proton + "/proton",
       "waitforexitandrun", "/game/x.exe"]
inject = wrap._injection(wrap._mode(), cmd, dict(env))
assert inject, "wrapper produced no injection (unsupported tool/arch?)"
binds, envs = {}, {}
i = 0
while i < len(inject):
    if inject[i] == "--ro-bind" and i + 2 < len(inject):
        binds[inject[i + 2]] = inject[i + 1]
        i += 3
    elif inject[i] == "--setenv" and i + 2 < len(inject):
        envs[inject[i + 1]] = inject[i + 2]
        i += 3
    elif inject[i] == "--unsetenv" and i + 1 < len(inject):
        envs[inject[i + 1]] = None
        i += 2
    else:
        i += 1
nvapi_dst = proton + "/files/lib/wine/nvapi/x86_64-windows/nvapi64.dll"
assert nvapi_dst in binds, "no nvapi64 copy-source bind: %r" % inject
assert envs.get("BRINEWELL_NGX_SR_CANDIDATE") == "1", envs
assert "BRINEWELL_NGX_SR_ACTIVE" in envs and \
    envs["BRINEWELL_NGX_SR_ACTIVE"] is None, envs
core = envs.get("BRINEWELL_NGX_CORE_SHA256")
nvapi = envs.get("BRINEWELL_NGX_NVAPI_SHA256")
assert core and nvapi and len(core) == len(nvapi) == 64, envs
overrides = envs.get("WINEDLLOVERRIDES", "")
assert "nvngx=n" in overrides and "nvapi64=n" in overrides, overrides
sys32 = os.path.join(compat, "pfx/drive_c/windows/system32")
assert os.path.isfile(os.path.join(sys32, "nvngx.dll")), "managed nvngx"
assert os.path.isfile(os.path.join(sys32, "nptunix.dll")), "nptunix"
assert os.path.isfile(os.path.join(compat, ".brinewell-ngx-sr.json"))
rec = {"proton": proton, "nvapi_src": binds[nvapi_dst],
       "nvapi_dst": nvapi_dst, "overrides": overrides,
       "core_sha256": core, "nvapi_sha256": nvapi}
with open(os.path.join(d, "wrap-env.json"), "w") as fh:
    json.dump(rec, fh)
with open(os.path.join(d, "wrap-env.sh"), "w") as fh:
    fh.write("BRINEWELL_NGX_SR_CANDIDATE='1'\n")
    fh.write("BRINEWELL_NGX_CORE_SHA256='%s'\n" % core)
    fh.write("BRINEWELL_NGX_NVAPI_SHA256='%s'\n" % nvapi)
    fh.write("WINEDLLOVERRIDES='%s'\n" % overrides)
print("WRAPCHECK: default-enabled _injection ok; nvapi bind %s -> %s" %
      (binds[nvapi_dst], nvapi_dst))
print("WRAPCHECK: candidate digests core=%s nvapi=%s" % (core, nvapi))
PY
# Replay the setup_prefix nvapi copy: unlink + copy, sourced from what
# the tool path serves inside the container (the ro-bind source).
sys32="$compat/pfx/drive_c/windows/system32"
src=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["nvapi_src"])' \
    "$d/wrap-env.json")
rm -f "$sys32/nvapi64.dll"
cp -f "$src" "$sys32/nvapi64.dll"
echo "WRAPCHECK: setup replay copied nvapi64 from $src"
python3 - "$sys32" "$d/wrap-env.json" <<'PY'
import hashlib, json, os, sys
sys32, rec = sys.argv[1], json.load(open(sys.argv[2]))
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
got_core = sha(os.path.join(sys32, "nvngx.dll"))
got_nvapi = sha(os.path.join(sys32, "nvapi64.dll"))
assert got_core == rec["core_sha256"], (got_core, rec["core_sha256"])
assert got_nvapi == rec["nvapi_sha256"], (got_nvapi, rec["nvapi_sha256"])
print("WRAPCHECK: prefix system32 digests match the candidate digests")
PY
SH
    }

    # Opt-out launch: _injection with BRINEWELL_NGX_SR=0 cleans the managed
    # nvngx.dll + marker and strips the candidate env, then the next
    # replayed setup restores the tool's stock nvapi64.dll (the bind
    # target read without the bind, i.e. the real tool file).
    wrap_opt_out() {
        guest bash -s <<'SH'
set -euo pipefail
d=~/dx12-lab/ngx-sr-v1
compat="$d/wrap-compat"
proton="$HOME/.local/share/Steam/steamapps/common/Proton 11.0"
# Stop the leftover wineserver so the wrapper's live-process guard lets
# this opt-out launch clean the prefix.
WINEPREFIX="$compat/pfx" "$proton/files/bin/wineserver" -kw \
    >/dev/null 2>&1 || true
python3 - "$compat" "$d" <<'PY'
import importlib.machinery, importlib.util, json, os, sys
compat, d = sys.argv[1], sys.argv[2]
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
       "BRINEWELL_NGX_SR": "0",
       # Inherited candidate/ACTIVE values must not leak either.
       "BRINEWELL_NGX_SR_ACTIVE": "1",
       "BRINEWELL_NGX_SR_CANDIDATE": "1",
       "BRINEWELL_NGX_CORE_SHA256": "0" * 64,
       "BRINEWELL_NGX_NVAPI_SHA256": "0" * 64}
cmd = ["FEXCompatTool", proton + "/proton",
       "waitforexitandrun", "/game/x.exe"]
inject = wrap._injection(wrap._mode(), cmd, dict(env))
assert inject, "wrapper produced no injection"
targets, envs = set(), {}
i = 0
while i < len(inject):
    if inject[i] == "--ro-bind" and i + 2 < len(inject):
        targets.add(inject[i + 2])
        i += 3
    elif inject[i] == "--setenv" and i + 2 < len(inject):
        envs[inject[i + 1]] = inject[i + 2]
        i += 3
    elif inject[i] == "--unsetenv" and i + 1 < len(inject):
        envs[inject[i + 1]] = None
        i += 2
    else:
        i += 1
nvapi_dst = proton + "/files/lib/wine/nvapi/x86_64-windows/nvapi64.dll"
assert nvapi_dst not in targets, "opt-out still binds nvapi64: %r" % inject
for name in ("BRINEWELL_NGX_SR_CANDIDATE", "BRINEWELL_NGX_CORE_SHA256",
             "BRINEWELL_NGX_NVAPI_SHA256", "BRINEWELL_NGX_SR_ACTIVE"):
    assert envs.get(name) is None, (name, envs)
sys32 = os.path.join(compat, "pfx/drive_c/windows/system32")
assert not os.path.exists(os.path.join(sys32, "nvngx.dll")), "nvngx"
assert not os.path.exists(os.path.join(compat,
                                       ".brinewell-ngx-sr.json")), "marker"
with open(os.path.join(d, "wrap-env.json"), "w") as fh:
    json.dump({"nvapi_dst": nvapi_dst}, fh)
print("WRAPCHECK: opt-out _injection ok; candidate env stripped, "
      "managed nvngx + marker removed")
PY
# Next setup restores the tool's stock nvapi64.dll: with no ro-bind the
# tool path serves its own file again.
sys32="$compat/pfx/drive_c/windows/system32"
dst=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["nvapi_dst"])' \
    "$d/wrap-env.json")
rm -f "$sys32/nvapi64.dll"
cp -f "$dst" "$sys32/nvapi64.dll"
echo "WRAPCHECK: setup replay restored stock nvapi64 from $dst"
sha256sum "$sys32/nvapi64.dll"
SH
    }

    wrap_check_positive() { # TAG
        local tag="$1" out="$EVIDENCE_DIR/last/wrap-$1.out"
        local path npath active
        path=$(grep -o 'loaded module: .*' "$out" | head -1 || true)
        npath=$(grep -o 'module nvapi64.dll path=[^ ]*' "$out" | head -1 || true)
        active=$(grep -o 'BRINEWELL_NGX_SR_ACTIVE in process env: [01]' \
            "$out" | head -1 || true)
        say "$tag loaded path: ${path:-<none>}"
        say "$tag nvapi path:  ${npath:-<none>}"
        say "$tag ${active:-active-env line missing}"
        printf '%s' "$path" | grep -qi 'windows.system32.nvngx\.dll' ||
            fail "$tag did not load nvngx.dll from the prefix system32: ${path:-<none>}"
        printf '%s' "$npath" | grep -qi 'windows.system32.nvapi64\.dll' ||
            fail "$tag nvapi64.dll did not come from the prefix system32: ${npath:-<none>}"
        [[ "$active" == *": 1" ]] ||
            fail "$tag process-side verification did not set ACTIVE; see $out"
    }

    step "wrapper-check pass 1: default-enabled _injection + nvapi copy (fresh)" \
        wrap_enabled fresh || fail "default-enabled injection pass 1 failed; see output above"
    say "positive 1: fixture in the wrapper-managed prefix"
    wrap_run_fixture on1 env ||
        fail "positive run 1 failed; see $EVIDENCE_DIR/last/wrap-on1.err"
    wrap_check_positive on1

    step "wrapper-check pass 2: default-enabled _injection + nvapi copy (reuse)" \
        wrap_enabled reuse || fail "default-enabled injection pass 2 failed; see output above"
    say "positive 2: fixture again in the same prefix"
    wrap_run_fixture on2 env ||
        fail "positive run 2 failed; see $EVIDENCE_DIR/last/wrap-on2.err"
    wrap_check_positive on2

    step "wrapper-check pass 3: opt-out _injection + stock nvapi copy" \
        wrap_opt_out || fail "opt-out injection failed; see output above"
    say "negative: fixture after opt-out cleanup + stock nvapi64"
    rc_neg=0
    wrap_run_fixture off noenv || rc_neg=$?
    path_neg=$(grep -o 'loaded module: .*' \
        "$EVIDENCE_DIR/last/wrap-off.out" | head -1 || true)
    fail_neg=$(grep 'NGXSR: FAIL' \
        "$EVIDENCE_DIR/last/wrap-off.out" | head -1 || true)
    say "negative result: ${fail_neg:-<none>} (rc=$rc_neg)"
    guest "cd $FIXTURE_GUEST_DIR && rm -rf wrap-compat wrap-env.json wrap-env.sh && \
        dev=\$HOME/.local/share/brinewell/neptune-dev; \
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
        fail "negative run failed for an unexpected reason (rc=$rc_neg): ${fail_neg:-<no FAIL line>}; see $EVIDENCE_DIR/last/wrap-off.err"
    fi
    say "wrapper check passed: on->on->off sequence verified"
    ;;
esac
