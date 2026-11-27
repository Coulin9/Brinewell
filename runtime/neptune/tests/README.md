# Swapchain state regression harness

`npt-swapchain-state-test` is a self-contained Wine PE executable for guest
swapchain state and lifetime behavior. It includes production common, D3D11,
and D3D12 code in separate test translation units. The tests substitute
transport, GPU/COM, resource-export, and observation boundaries; they do not
link the complete Neptune driver or require a real D3D device or VM. Real
Win32 threads and synchronization primitives exercise worker and drain behavior.

The target is available in a Wine PE/GNU Meson configuration even with
`build-tests=false`, but is not built by default. With tests enabled it is also
registered in the `neptune` Meson suite. The fixture sources and production
changes must be complete and stable before building.

## Isolated build and execution

From the workspace root, with exclusive access to the configured PE build output
and a new test-owned Wine prefix:

```sh
set -o pipefail
EVIDENCE="$PWD/steamac/work/neptune/present-state-evidence"
mkdir -p "$EVIDENCE"

/opt/homebrew/bin/ninja \
  -C steamac/work/neptune/build-footprint-guest -j3 \
  npt-swapchain-state-test.exe \
  2>&1 | tee "$EVIDENCE/build.log"

WINE_BIN="/Applications/Wine Staging.app/Contents/Resources/wine/bin"
export WINEPREFIX="$EVIDENCE/wine-prefix"
export WINEARCH=win64
export WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml="

"$WINE_BIN/wineboot" -u 2>&1 | tee "$EVIDENCE/wineboot.log"

EXE="$PWD/steamac/work/neptune/build-footprint-guest/npt-swapchain-state-test.exe"
"$WINE_BIN/wine" "$EXE" --group all \
  2>&1 | tee "$EVIDENCE/results.log"
```

Use a newly provisioned prefix, not a user bottle. The executable also accepts
`--group common`, `d3d11`, and `d3d12` for targeted runs. It reports
case progress and assertions and exits nonzero on failure; retain the logs and
prefix as evidence. Stop on unexpected build, bootstrap, or runtime failures
rather than retrying with another environment. If runtime makes no progress for
60–90 seconds, preserve the current case, phase, and outstanding barrier/thread
ledger for investigation.

## Coverage boundaries

The fixture focuses on the common worker's terminal, release, fallback, and
latency behavior, and on the D3D11 and D3D12 owners' poll/drain helpers at
substituted boundaries. It does **not** exercise the full actual Present pipeline
or establish real GPU behavior, including:

- real X11/DRI3/DRM cancellation and release behavior;
- GPU conversion or displayed-color correctness;
- real cross-VM completion-FD creation/import or missing-feedback RPC latency;
- a hard real-time bound for every nonblocking Present.

Those integration behaviors require a separately configured Linux guest and
render server.

## Recorded results

The post-review Wine run passed 13 cases in three groups: 10 common, two
D3D11 and one D3D12. The common cases exercise the real worker, slot, latency,
terminal and cancellation logic; the owner cases exercise the actual poll and
drain helpers at substituted boundaries. The requested production PE DLL
targets also rebuilt successfully (exit 0). The build and run logs are
`steamac/work/neptune/present-state-evidence/build-run3.log` and
`steamac/work/neptune/present-state-evidence/results-run3.log`.

The native `present-copy`, `present-format-d3d11` and
`present-format-d3d12` tests passed with `DMN_MTL4=1` and the existing
Frameworks path. Their fixture had identical bytes and no sibling include.
Neither those tests nor the Wine harness establish full Present-pipeline or
real cross-VM/GPU runtime behavior.

## Descriptor copy regression harness

`npt-descriptor-copy-test` includes the production descriptor bookkeeping
implementation and substitutes the copy transport and ring ordering boundaries.
It exercises slot writer and reader state, publication ordering, range
boundaries, heap cache invalidation, and an event-ordered cross-thread handoff.
The staged multi-range implementation retains at most the first 16 destination
ranges through submission; larger calls still process every range, resolving
overflow destinations again after submission.

In the same Wine PE/GNU configuration, with exclusive access to the build
output and an isolated test-owned Wine prefix as above, build and run it with:

```sh
/opt/homebrew/bin/ninja -C steamac/work/neptune/build-footprint-guest -j3 \
  npt-descriptor-copy-test.exe
"$WINE_BIN/wine" \
  "$PWD/steamac/work/neptune/build-footprint-guest/npt-descriptor-copy-test.exe"
```

The target is not built by default; with tests enabled it is also registered as
`npt-descriptor-copy` in the `neptune` suite. Its substituted boundaries do
not validate real GPU, protocol, or cross-VM execution.
