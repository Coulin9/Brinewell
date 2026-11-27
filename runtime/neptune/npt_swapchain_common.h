/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * State and machinery shared by the D3D11 and D3D12 guest-fabricated
 * swapchains: the exportable present pool, the WSI worker and X
 * IDLE_NOTIFY drain threads, the CPU-runahead latency ring, the pool
 * reuse gate, and every DXGI method that only touches this state.
 *
 * Each swapchain embeds struct npt_guest_swapchain_common as its first
 * member, so the aux pointer of either converts to the common state and
 * the shared vtbl methods serve both.
 */

#ifndef NPT_SWAPCHAIN_COMMON_H
#define NPT_SWAPCHAIN_COMMON_H

#include "npt_com.h"
#include "nptunix/npt_unixlib.h"

#include "neptune-protocol/npt_protocol_directx_types.h"

#if defined(__WINE__)

#include <stdatomic.h>
#include <windows.h>

#include "neptune-protocol/npt_protocol_client_idxgioutput.h"

/* DXGI constants the protocol types header doesn't carry. */
#define NPT_DXGI_PRESENT_TEST            0x00000001u
#define NPT_DXGI_PRESENT_DO_NOT_WAIT     0x00000008u
#define NPT_DXGI_PRESENT_ALLOW_TEARING   0x00000200u
#define NPT_DXGI_STATUS_OCCLUDED         ((HRESULT)0x087A0001)
#define NPT_DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000A)
#define NPT_DXGI_ERROR_DEVICE_REMOVED    ((HRESULT)0x887A0005)

/* Present-pool depth: enough for one on-screen + one queued + one
 * being written.  Must fit the unixlib's image table. */
#define NPT_SC_PRESENT_IMAGES 3u

/* CPU-runahead cap: ring of GPU-done fence fds or completion markers;
 * a Present that finds the ring full first waits on the oldest.
 * Mirrors DXGI's MaximumFrameLatency semantics (default 3). */
#define NPT_SC_PRESENT_LATENCY 3u

/* Async WSI worker queue.  poll(fence) + xcb_present run on a per-
 * swapchain worker so the app's Present thread never blocks on the
 * host GPU; the latency ring above is the pacing bound. */
#define NPT_SC_WSI_QUEUE_SIZE 8u

/* Reuse-gate budget: how long Present waits for the compositor to
 * release the next pool image. A timeout returns busy or occluded and
 * never grants ownership of the image. */
#define NPT_SC_REUSE_WAIT_MS 100u

/* DXGI's DXGI_MAX_SWAP_CHAIN_BUFFERS; also caps MaximumFrameLatency
 * and the frame-latency waitable semaphore. */
#define NPT_SC_MAX_SWAP_CHAIN_BUFFERS 16u

/* Consecutive never-idled presents before Present reports
 * DXGI_STATUS_OCCLUDED. */
#define NPT_SC_OCCLUDED_STREAK 4u

enum npt_gsc_slot_state {
   NPT_GSC_SLOT_FREE = 0,
   NPT_GSC_SLOT_RESERVED = 1,
   NPT_GSC_SLOT_QUEUED = 2,
   NPT_GSC_SLOT_SUBMITTED = 3,
};

enum npt_gsc_gen_state {
   NPT_GSC_GEN_ACTIVE = 0,
   NPT_GSC_GEN_CANCELLED = 1,
   NPT_GSC_GEN_WINDOW_LOST = 2,
   NPT_GSC_GEN_FAILED = 3,
};

/* S_OK: complete; S_FALSE: pending; failing HRESULT: terminal error. */
typedef HRESULT (*npt_gsc_gpu_poll_fn)(void *ctx, uint32_t image,
                                       uint64_t value);

struct npt_gsc_format_plan {
   uint32_t pool_format;
   uint32_t srv_format;
   uint32_t rtv_format;
   bool needs_convert;
};

struct npt_gsc_present_entry {
   uint32_t image_index;
   int      wait_fence_fd;   /* -1 requires worker-established producer completion; ownership -> worker */
   uint64_t token;           /* event token to release after present */
   uint64_t producer_fence_val;
   uint64_t present_time_ns;
   bool wait_for_gpu;
};

struct npt_gsc_latency_entry {
   int fd;
   uint32_t image;
   uint64_t value;
};

struct npt_guest_swapchain_common {
   struct npt_com_base *com;

   /* Log prefix naming the owning swapchain type. */
   const char *tag;

   /* Guest-authoritative description. */
   DXGI_SWAP_CHAIN_DESC1 desc;
   DXGI_COLOR_SPACE_TYPE color_space;
   struct npt_unix_hdr10_metadata hdr_meta;
   bool color_space_dirty;
   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc;
   HWND hwnd;
   BOOL fullscreen;
   UINT max_frame_latency;
   DXGI_RGBA background;
   UINT present_count;
   struct npt_private_data private_data;

   /* Frame-latency waitable object (non-NULL only when the app created
    * the swapchain with FRAME_LATENCY_WAITABLE_OBJECT).  Released once
    * per Present so the app's wait paces it to max_frame_latency. */
   HANDLE frame_latency_sem;

   /* Present pool: exportable host textures bound to guest blobs whose
    * dmabufs feed the X11 WSI.  wsi is this swapchain's renderer WSI
    * instance, valid while wsi_initialized. */
   bool wsi_initialized;
   bool wsi_hdr;
   bool frame_stats_enabled;
   bool wsi_failed;
   uint64_t wsi;
   void *present_tex[NPT_SC_PRESENT_IMAGES];
   uint64_t present_tex_id[NPT_SC_PRESENT_IMAGES];
   _Atomic int gen_state;
   _Atomic int gen_error_hr;
   _Atomic uint32_t slot_state[NPT_SC_PRESENT_IMAGES];
   _Atomic uint64_t slot_fence_val[NPT_SC_PRESENT_IMAGES];
   _Atomic uint64_t slot_completed_val[NPT_SC_PRESENT_IMAGES];
   npt_gsc_gpu_poll_fn gpu_poll;
   void *gpu_poll_ctx;
   /* Auto-reset event the drain thread signals on each release so the
    * Present reuse-gate blocks instead of polling. */
   HANDLE image_release_evt;
   /* Consecutive presents whose image never idled (hidden/minimized
    * window): drives the DXGI_STATUS_OCCLUDED return so the app
    * throttles instead of spinning. */
   _Atomic uint32_t occluded_streak;

   /* Single lock over Present/Resize/GetBuffer. */
   mtx_t present_mutex;

   /* Entries are either duplicated FDs or producer completion markers. */
   struct npt_gsc_latency_entry latency[NPT_SC_PRESENT_LATENCY];
   uint32_t latency_count;

   /* Async WSI worker. */
   mtx_t wsi_mutex;
   cnd_t wsi_cond;
   struct npt_gsc_present_entry wsi_queue[NPT_SC_WSI_QUEUE_SIZE];
   uint32_t wsi_head, wsi_tail;
   _Atomic int wsi_thread_exit;
   bool wsi_thread_started;
   HANDLE wsi_thread;

   /* X IDLE_NOTIFY release-count drain. */
   HANDLE drain_thread;
   _Atomic int drain_exit;
   bool drain_started;

   /* IDXGIOutput bookkeeping (guest-fab outputs never reach the wire). */
   mtx_t output_lock;
   IDXGIOutput *fullscreen_target;
   IDXGIOutput *containing_output;
};

static inline struct npt_guest_swapchain_common *
npt_gsc(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static inline const struct npt_idxgioutput_client_vtbl *
npt_gsc_output_vtbl(IDXGIOutput *o)
{
   return (const void *)((struct npt_com_base *)o)->lpVtbl;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void
npt_gsc_init(struct npt_guest_swapchain_common *c, struct npt_com_base *com,
             const DXGI_SWAP_CHAIN_DESC1 *desc1,
             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
             HWND hwnd, const char *tag);

void
npt_gsc_fini(struct npt_guest_swapchain_common *c);

/* ------------------------------------------------------------------ */
/* held-wrapper helpers                                                */
/* ------------------------------------------------------------------ */

void
npt_gsc_release_held(struct npt_device *dev, void **slot, uint64_t *id);

HRESULT
npt_gsc_qi_held(void *wrapper, const IID *riid, void **out);

/* ------------------------------------------------------------------ */
/* WSI lifecycle                                                       */
/* ------------------------------------------------------------------ */

bool
npt_gsc_wsi_backend_available(struct npt_guest_swapchain_common *c);

DXGI_COLOR_SPACE_TYPE
npt_gsc_color_space_for_resize(uint32_t old_format, uint32_t new_format,
                               DXGI_COLOR_SPACE_TYPE current_color_space);

/* Pool format for a backbuffer format.  The WSI shows the pool through
 * depth-24 X pixmaps, so a backbuffer whose bits are not 8-bit BGRA/RGBA
 * gets a BGRA8 pool and is converted into it by a draw: a copy cannot
 * change format. */
bool
npt_gsc_get_format_plan(uint32_t backbuffer_format,
                        DXGI_COLOR_SPACE_TYPE color_space,
                        struct npt_gsc_format_plan *out);

bool
npt_gsc_wsi_bringup(struct npt_guest_swapchain_common *c, uint32_t fmt);

void
npt_gsc_wsi_stop_begin(struct npt_guest_swapchain_common *c, bool shutting_down);

/* S_FALSE means a worker is still running; no resources are destroyed. */
HRESULT
npt_gsc_wsi_join(struct npt_guest_swapchain_common *c, bool do_not_wait);

/* Requires joined workers and owner-established GPU completion. */
void
npt_gsc_wsi_finish(struct npt_guest_swapchain_common *c);

void
npt_gsc_release_pool(struct npt_guest_swapchain_common *c);

/* Takes ownership of fd and token on every outcome.  The producer value
 * must be nonzero and monotonically increasing; wait_for_gpu requests
 * worker-side completion before submitting to the backend. */
bool
npt_gsc_wsi_push(struct npt_guest_swapchain_common *c, uint32_t image_index,
                 int wait_fence_fd, uint64_t token,
                 uint64_t producer_fence_val, bool wait_for_gpu);

/* ------------------------------------------------------------------ */
/* present helpers                                                     */
/* ------------------------------------------------------------------ */

HRESULT
npt_gsc_present_test(struct npt_guest_swapchain_common *c);

HRESULT
npt_gsc_reserve_slot(struct npt_guest_swapchain_common *c, uint32_t image,
                     bool do_not_wait);

void
npt_gsc_fail_generation(struct npt_guest_swapchain_common *c, HRESULT hr);

uint64_t
npt_gsc_make_token(void);

void
npt_gsc_frame_latency_release(struct npt_guest_swapchain_common *c);

void
npt_gsc_backpressure_wait(struct npt_guest_swapchain_common *c);

/* Present's argument rules: DXGI_ERROR_INVALID_CALL or S_OK. */
HRESULT
npt_gsc_present_validate(const struct npt_guest_swapchain_common *c,
                         UINT SyncInterval, UINT Flags);

/* True when presenting into `image` now would block on backpressure or
 * on the image's reuse gate: DXGI_PRESENT_DO_NOT_WAIT's
 * DXGI_ERROR_WAS_STILL_DRAWING case.  Caller holds present_mutex. */
bool
npt_gsc_present_would_block(struct npt_guest_swapchain_common *c,
                            uint32_t image);

void
npt_gsc_query_window_size(struct npt_guest_swapchain_common *c,
                          UINT *width, UINT *height);

void
npt_gsc_window_resize(HWND hwnd, uint32_t w, uint32_t h);

/* ------------------------------------------------------------------ */
/* outputs                                                             */
/* ------------------------------------------------------------------ */

void
npt_gsc_assign_output(struct npt_guest_swapchain_common *c,
                      IDXGIOutput **slot, IDXGIOutput *new_val);

IDXGIOutput *
npt_gsc_load_output_addref(struct npt_guest_swapchain_common *c,
                           IDXGIOutput **slot);

void
npt_gsc_release_outputs(struct npt_guest_swapchain_common *c);

/* ------------------------------------------------------------------ */
/* shared vtbl methods                                                 */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_QueryInterface(void *self, REFIID riid, void **ppvObject);
ULONG NPT_STDMETHODCALLTYPE
npt_gsc_AddRef(void *self);
ULONG NPT_STDMETHODCALLTYPE
npt_gsc_Release(void *self);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateData(void *self, const GUID *Name, UINT DataSize,
                       const void *pData);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateDataInterface(void *self, const GUID *Name,
                                const IUnknown *pUnknown);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetPrivateData(void *self, const GUID *Name, UINT *pDataSize,
                       void *pData);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc(void *self, DXGI_SWAP_CHAIN_DESC *pDesc);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc1(void *self, DXGI_SWAP_CHAIN_DESC1 *pDesc);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenDesc(void *self, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetHwnd(void *self, HWND *pHwnd);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetCoreWindow(void *self, const IID *refiid, void **ppUnk);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenState(void *self, BOOL *pFullscreen,
                           IDXGIOutput **ppTarget);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRestrictToOutput(void *self, IDXGIOutput **ppRestrictToOutput);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameStatistics(void *self, DXGI_FRAME_STATISTICS *pStats);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetLastPresentCount(void *self, UINT *pLastPresentCount);
BOOL NPT_STDMETHODCALLTYPE
npt_gsc_IsTemporaryMonoSupported(void *self);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetBackgroundColor(void *self, const DXGI_RGBA *pColor);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetBackgroundColor(void *self, DXGI_RGBA *pColor);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetRotation(void *self, DXGI_MODE_ROTATION Rotation);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRotation(void *self, DXGI_MODE_ROTATION *pRotation);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetSourceSize(void *self, UINT Width, UINT Height);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetSourceSize(void *self, UINT *pWidth, UINT *pHeight);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMaximumFrameLatency(void *self, UINT MaxLatency);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMaximumFrameLatency(void *self, UINT *pMaxLatency);
HANDLE NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameLatencyWaitableObject(void *self);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMatrixTransform(void *self, const DXGI_MATRIX_3X2_F *pMatrix);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMatrixTransform(void *self, DXGI_MATRIX_3X2_F *pMatrix);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_CheckColorSpaceSupport(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace,
                               UINT *pColorSpaceSupport);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetColorSpace1(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetHDRMetaData(void *self, DXGI_HDR_METADATA_TYPE Type, UINT Size,
                       void *pMetaData);

#endif /* __WINE__ */

#endif /* NPT_SWAPCHAIN_COMMON_H */
