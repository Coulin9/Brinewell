#!/bin/bash
# Uncapped D3D11 submission microbench in the running guest; prints the median
# e2e ns/call per scenario against the saved baseline. No automatic verdict.
#
#   scripts/dev/perf.sh [REPS]              compare with the baseline (default 3 reps)
#   scripts/dev/perf.sh --baseline [REPS]   run and save as the new baseline
source "$(dirname "$0")/lib.sh"

SAVE=0
[[ "${1:-}" != --baseline ]] || { SAVE=1; shift; }
REPS="${1:-3}"
[[ "$REPS" =~ ^[1-9][0-9]*$ ]] || fail "usage: $0 [--baseline] [REPS]"
DIR="$WORKSPACE/perf-lab/dev-perf"
RUN="$DIR/runs/$(date +%Y%m%d-%H%M%S).jsonl"
mkdir -p "$DIR/runs"

guest_up || fail "guest SSH is not reachable; start the VM first"
uncap() {
    local value try
    for try in 1 2 3 4 5; do
        value="$(guest 'DISPLAY=:0 gamescopectl debug_set_fps_limit 0 >/dev/null 2>&1; sleep 1; DISPLAY=:0 xprop -root GAMESCOPE_LIMITER_FEEDBACK' | awk '{print $NF}')"
        [[ "$value" != 0 ]] || return 0
    done
    fail "GAMESCOPE_LIMITER_FEEDBACK=$value (expected 0)"
}
step "sync harnesses" sync_harnesses
uncap
# The guest samples GAMESCOPE_LIMITER_FEEDBACK every second while the suite
# runs and prints the distinct values seen on stderr.
bench() {
    guest "REPS=$REPS bash -s" > "$RUN" 2> "$RUN.limiter" <<'SH'
cd ~/dx12-lab/dx11-submit-bench
samples="$(mktemp)"
while :; do
    DISPLAY=:0 xprop -root GAMESCOPE_LIMITER_FEEDBACK | awk '{print $NF}'
    sleep 1
done > "$samples" &
watcher=$!
./suite.sh
kill "$watcher"; wait "$watcher" 2>/dev/null || true
sort -u "$samples" | tr '\n' ' ' >&2
rm -f "$samples"
SH
}
step "bench ($REPS reps)" bench
seen="$(cat "$RUN.limiter")"
[[ "$seen" == "0 " ]] || { SAVE=0; say "WARNING: limiter feedback during the run: '${seen% }' (expected only 0); results may be capped"; }

python3 - "$RUN" "$DIR/baseline.jsonl" "$SAVE" "$REPS" <<'PY'
import json, shutil, statistics, sys
run, baseline, save, reps = sys.argv[1], sys.argv[2], sys.argv[3] == "1", int(sys.argv[4])
def medians(path):
    by = {}
    for line in open(path):
        r = json.loads(line)
        if r.get("ok") is True:
            by.setdefault(r["mode"], []).append(r["e2e_ns_per_call"])
        else:
            by.setdefault(r["mode"].split()[0], [])
    return {k: (statistics.median(v) if v else None, len(v)) for k, v in by.items()}
cur = medians(run)
try:
    base = medians(baseline)
except FileNotFoundError:
    base = {}
print(f"  {'mode':8} {'e2e ns/call':>12} {'baseline':>10} {'delta':>8}  ok-reps")
for mode, (value, n) in cur.items():
    b = base.get(mode, (None, 0))[0]
    delta = f"{(value - b) / b * 100:+.1f}%" if value and b else "-"
    shown = f"{value:.1f}" if value else "FAILED"
    print(f"  {mode:8} {shown:>12} {b if b else '-':>10} {delta:>8}  {n}")
if save and cur and all(n == reps for _, n in cur.values()):
    shutil.copyfile(run, baseline)
    print("baseline saved")
elif save:
    print("baseline NOT saved: some scenarios failed")
print(f"raw: {run}")
PY
