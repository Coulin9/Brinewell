#!/bin/bash
# Quick D3D11 + D3D12 correctness check in the running guest using the
# private ~/dx12-lab harnesses and the effective Neptune payload.
#
#   scripts/dev/smoke.sh             run, compare with the saved reference
#   scripts/dev/smoke.sh --update    run and save the results as the reference
source "$(dirname "$0")/lib.sh"

UPDATE=0
case "${1:-}" in
    "") ;;
    --update) UPDATE=1 ;;
    *) fail "usage: $0 [--update]" ;;
esac
REFERENCE="$WORKSPACE/dx12-lab/dev-smoke/reference.json"
LOGDIR="$WORKSPACE/dx12-lab/dev-smoke/last"
mkdir -p "$LOGDIR"

guest_up || fail "guest SSH is not reachable; start the VM first"
step "sync harnesses" sync_harnesses

run_case() { # NAME DIR ARGS...
    local name="$1" dir="$2"; shift 2
    local rc=0
    guest "cd ~/dx12-lab/$dir && timeout 180 ./run-vm.sh $*" \
        >"$LOGDIR/$name.out" 2>"$LOGDIR/$name.err" || rc=$?
    echo "$rc" >"$LOGDIR/$name.rc"
}
step "d3d11 submit" run_case d3d11 dx11-submit-bench imm 1 20000
step "d3d12 render" run_case d3d12 dx12-guard-20261120 \
    --shader-dir shaders --frames 120 --mode cached --iterations 1024 --grid 1

python3 - "$LOGDIR" "$REFERENCE" "$UPDATE" <<'PY'
import json, pathlib, sys
logdir, reference, update = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3] == "1"
def record(name):
    if (logdir / f"{name}.rc").read_text().strip() != "0":
        return None
    lines = [l for l in (logdir / f"{name}.out").read_text().splitlines() if l.startswith('{"mode"')]
    return json.loads(lines[-1]) if lines else None
results = {}
d3d11 = record("d3d11")
results["d3d11"] = d3d11 and d3d11.get("ok") is True and d3d11.get("pixel")
d3d12 = record("d3d12")
results["d3d12"] = d3d12 and d3d12.get("rgba_fnv1a64")
ref = json.loads(reference.read_text()) if reference.exists() else {}
failed = False
for name, value in results.items():
    if not value:
        status = "FAIL (no valid result, see %s/%s.{out,err})" % (logdir, name)
        failed = True
    elif update or name not in ref:
        status = f"PASS {value} (reference recorded)"
        ref[name] = value
    elif ref[name] == value:
        status = f"PASS {value}"
    else:
        status = f"FAIL {value} != reference {ref[name]}"
        failed = True
    print(f"  {name:6} {status}")
if not failed:
    reference.parent.mkdir(parents=True, exist_ok=True)
    reference.write_text(json.dumps(ref, indent=2, sort_keys=True) + "\n")
print("SMOKE " + ("FAIL" if failed else "PASS"))
sys.exit(1 if failed else 0)
PY
