# Neptune Wine guest on ARM64 SteamOS

This directory contains the Neptune guest interoperability code. The early
validation below used an ARM64 Steamac
VM on Apple Silicon. The tested Windows application path was x86-64 Proton 11
under FEX, with x86-64 Neptune PE DLLs and an x86-64 `nptunix.so`. This does not
establish support for loading these artifacts into ARM64EC Proton or native
Wine Wayland. The WSI implementation uses the Wine X11 window and Xwayland
DRI3/Present.

The host must separately provide the Neptune virtio-gpu capset, Direct3D 12
support and shared-resource imports. These guest changes alone do not add
Neptune support to an unmodified VMM.

## Guest changes

- The Wine virtgpu backend queries `VIRTGPU_PARAM_BLOB_ALIGNMENT` (9) and uses
  the returned power-of-two alignment for both new shared allocations and host
  blob imports. Unsupported queries fall back to 4096. The tested Steamac
  kernel already reports 16384; the final Steamac run used its original kernel.
  Separate experimental kernels were used only during QEMU investigation.
- Swapchains support BGRA8, BGRX8 and RGBA8 UNORM, SRGB and TYPELESS
  formats, as well as R10G10B10A2 UNORM/TYPELESS with SDR quantization
  and R16G16B16A16_FLOAT in G10 or G22 SDR color spaces. Explicit views
  and conversion preserve the requested backbuffer format while producing
  a depth-24-compatible presentation pool. HDR10 PQ/BT.2020 is not
  advertised on depth-24 X11.
- When Xwayland/Gamescope and `virtgpu_drm` advertise the required
  capabilities, DRI3 1.4 and Present 1.4 use `PresentPixmapSynced` with
  persistent per-image acquire and release syncobj timelines. Otherwise,
  WSI waits on the GPU completion `sync_file` on the CPU before legacy
  Present. Failed waits reject presentation rather than exposing unfinished
  GPU writes.
- An inert exported `nptunix_anchor` gives LLVM lld an export directory for
  the helper DLL. The existing post-link step can then rewrite its embedded
  module name and apply the Wine builtin marker.

## WSI controls

Per-frame Present requests are sent unchecked; asynchronous X errors are
harvested on drain regardless of diagnostics. `NPT_WSI_DIAGNOSTICS=1` enables
sampled Present, fence poll, timeline-release, Idle and Complete telemetry for
the first eight and every 120th event; failed fence polls are also logged.
Diagnostics default to off. `NPT_WSI_DIAGNOSTIC_DEFAULT=1` may also be supplied
at compile time for a diagnostic build; a runtime value of `0` disables it.

`NPT_WSI_EXPLICIT_SYNC=0` forces the legacy CPU `sync_file` wait path for
A/B comparison. Explicit timeline synchronization is enabled automatically
when the required server and driver capabilities are available.

## D3D12 API tracing

Set `NPT_DEBUG=api_trace` in the guest process environment to enable
`NPT_API_TRACE` diagnostics. Combine it with other `NPT_DEBUG` tokens using
commas. The trace covers D3D12 entry points, COM QueryInterface results,
feature support requests and heap creation paths. It logs interface IDs,
feature structures and heap descriptions, but not shader contents.

Tracing is disabled by default and can produce substantial logging and
performance overhead; enable it only for diagnosis. Logged guest return values
are not necessarily host HRESULTs: in particular, the default asynchronous
heap creation path returns before the host result is known.

## Building the Brinewell guest payload

From the Brinewell checkout, run:

```sh
./build.sh --component neptune
```

The recipe builds x86-64 and ARM64EC Wine PE drivers and matching
x86-64 and ARM64 Linux unixlibs. Meson consumes the unmodified Mesa
release from `.build/out/kosmicrisp/src` as its read-only `mesa`
subproject. The Neptune sources, kernel ioctl declarations and generated
protocol client live in this directory. The resulting `payload.json`
records the upstream Mesa commit and a digest of the Neptune sources.

## Historical validation and remaining limits

The earlier experimental build passed FL12_0 device creation, a real DIRECT
queue CopyResource and fence wait, and a 64 KiB readback comparison. Nightreign
then displayed its title/menu with correct UNORM28 colors: a captured BGRA
pool frame and the X window matched all 1,764,000 RGB pixels. An existing save
loaded into Roundtable Hold, where movement and jumping were observed.

The earlier P1.2 package was built as x86-64 PE DLLs and an x86-64 ELF
Unixlib. Its WSI T1–T8 checks passed, as did the native format tests: 7/7
in each of four configurations. The user also confirmed startup and gameplay
with Nightreign on DX12 and AI Limit on DX11 using that installed package.
These are historical results and were not rerun for the current repairs. The
installed app remains that previous user-tested package; the current
post-review source and PE changes have not been repackaged or installed.

The post-review repairs implement WSI cancellation as a wakeup and retirement
request, not an image-release acknowledgement: submitted slots remain
unavailable until genuinely released or safely retired, including on terminal
failure. The first error is latched. If a GPU-completion FD cannot be obtained,
the WSI worker establishes completion before submitting without an FD rather
than waiting on the Present caller. Color-space reconfiguration retires the
old generation resumably: nonblocking Present can return `WAS_STILL_DRAWING`
while retirement is pending and retry the same drain without losing the
requested configuration.

The requested production PE DLL targets rebuilt successfully (exit 0). The
Wine regression executable passed 13 cases across three groups: 10 common,
two D3D11 and one D3D12. It exercises the real common worker, slot, latency,
terminal and cancellation logic and actual owner poll/drain helpers with
substituted boundaries, not the full Present pipeline or a real cross-VM/GPU
runtime. Build and execution logs are
`steamac/work/neptune/present-state-evidence/build-run3.log` and
`steamac/work/neptune/present-state-evidence/results-run3.log`.
The native `present-copy`, `present-format-d3d11` and
`present-format-d3d12` tests also passed with `DMN_MTL4=1` and the existing
Frameworks path; the fixture had identical bytes and no sibling include.

The experiment enabled diagnostics; reproduce those logs with
`NPT_WSI_DIAGNOSTICS=1`. Performance A/B measurement, long-duration gameplay,
and resize/minimize stress remain deferred; no CPU or frame-time improvement
has been established. Combat, multiplayer, other swapchain formats and full
D3D12 conformance also remain unvalidated. DXR and several D3D12 interfaces,
including root-signature deserialization and GetInterface paths, still have
existing limitations. The legacy CPU fence fallback adds a wait that the
explicit timeline path avoids.
