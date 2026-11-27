/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared machinery of the guest-fabricated swapchains; see
 * npt_swapchain_common.h.
 */

#include "npt_swapchain_common.h"

#if defined(__WINE__)

#include "npt_api_trace.h"
#include "npt_device.h"
#include "npt_event.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_renderer_wine_common.h"
#include "npt_shared_texture.h"
#include "nptunix/npt_unixlib.h"

#include <stdlib.h>
#include <string.h>

#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"

static_assert(NPT_SC_PRESENT_IMAGES <= NPT_WSI_MAX_IMAGES,
              "present pool exceeds unixlib image table");

static const GUID *const npt_gsc_qi_chain[] = {
   &NPT_IID_IUnknown,
   &NPT_IID_IDXGIObject,
   &NPT_IID_IDXGIDeviceSubObject,
   &NPT_IID_IDXGISwapChain,
   &NPT_IID_IDXGISwapChain1,
   &NPT_IID_IDXGISwapChain2,
   &NPT_IID_IDXGISwapChain3,
   &NPT_IID_IDXGISwapChain4,
   NULL,
};

/* ------------------------------------------------------------------ */
/* held-wrapper helpers                                                */
/* ------------------------------------------------------------------ */

void
npt_gsc_release_held(struct npt_device *dev, void **slot, uint64_t *id)
{
   void *w = *slot;
   uint64_t host_id = *id;
   *slot = NULL;
   *id = 0;
   if (!w)
      return;
   /* The device-level force-destroy drain frees wrappers in arbitrary
    * order; a release on an already-freed wrapper is UAF. */
   if (!dev || !host_id ||
       npt_device_wrapper_cache_is_live(dev, host_id, w))
      npt_com_default_release(w);
}

/* Generic "QI a held wrapper for the caller's riid" helper: the held
 * ref stays ours; QI hands the caller its own. */
HRESULT
npt_gsc_qi_held(void *wrapper, const IID *riid, void **out)
{
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!wrapper)
      return NPT_E_FAIL;
   const struct npt_idxgiswapchain_client_vtbl *v =
      (const void *)((struct npt_com_base *)wrapper)->lpVtbl;
   return v->QueryInterface(wrapper, riid, out);
}

/* ------------------------------------------------------------------ */
/* IDLE_NOTIFY drain thread                                            */
/* ------------------------------------------------------------------ */

static DWORD WINAPI
npt_gsc_drain_thread(LPVOID arg)
{
   struct npt_guest_swapchain_common *c = arg;
   struct npt_renderer *renderer = c->com->base.device->renderer;
   while (!atomic_load(&c->drain_exit)) {
      uint32_t released[NPT_WSI_MAX_IMAGES] = { 0 };
      int rc = npt_renderer_wsi_drain(renderer, c->wsi, /*timeout_ms=*/100,
                                      released);
      if (rc < 0) {
         if (atomic_load(&c->drain_exit) ||
             atomic_load(&c->gen_state) != NPT_GSC_GEN_ACTIVE)
            break;
         int expected = NPT_GSC_GEN_ACTIVE;
         atomic_compare_exchange_strong(&c->gen_state, &expected,
                                        NPT_GSC_GEN_WINDOW_LOST);
         npt_renderer_wsi_cancel(renderer, c->wsi);
         if (c->image_release_evt)
            SetEvent(c->image_release_evt);
         mtx_lock(&c->wsi_mutex);
         cnd_broadcast(&c->wsi_cond);
         mtx_unlock(&c->wsi_mutex);
         break;
      }
      for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
         if (released[i]) {
            uint32_t submitted = NPT_GSC_SLOT_SUBMITTED;
            if (!atomic_compare_exchange_strong(&c->slot_state[i], &submitted,
                                                NPT_GSC_SLOT_FREE))
               continue;
            if (atomic_load(&c->gen_state) == NPT_GSC_GEN_ACTIVE)
               atomic_store(&c->occluded_streak, 0);
            if (c->image_release_evt)
               SetEvent(c->image_release_evt);
         }
      }
   }
   return 0;
}

static bool
npt_gsc_drain_start(struct npt_guest_swapchain_common *c)
{
   if (c->drain_started)
      return true;
   atomic_store(&c->drain_exit, 0);
   c->drain_thread = CreateThread(NULL, 0, npt_gsc_drain_thread, c, 0, NULL);
   if (!c->drain_thread) {
      npt_log("%s: drain CreateThread failed err=%lu", c->tag,
              GetLastError());
      return false;
   }
   c->drain_started = true;
   return true;
}


/* ------------------------------------------------------------------ */
/* WSI worker thread                                                   */
/* ------------------------------------------------------------------ */

/* Process-unique single-use event tokens.  Bit 63 keeps them disjoint
 * from real Wine HANDLE values in the host's token registry; the one
 * counter serves both swapchain types, so tokens stay unique when a
 * process holds both. */
static _Atomic uint64_t npt_gsc_next_token_low = 1;

uint64_t
npt_gsc_make_token(void)
{
   return npt_com_make_guest_id(NPT_GUEST_KIND_SWAPCHAIN,
                                atomic_fetch_add(&npt_gsc_next_token_low, 1));
}

/* Advance the frame-latency waitable object by one completed frame.
 * Called exactly once per Present (on the worker once the present
 * retires, or synchronously when the frame is dropped) so a waiting
 * app never deadlocks.  Saturates silently at the semaphore's max. */
void
npt_gsc_frame_latency_release(struct npt_guest_swapchain_common *c)
{
   if (c->frame_latency_sem)
      ReleaseSemaphore(c->frame_latency_sem, 1, NULL);
}

static DWORD WINAPI
npt_gsc_wsi_thread(LPVOID arg)
{
   struct npt_guest_swapchain_common *c = arg;
   struct npt_device *dev = c->com->base.device;
   struct npt_renderer *renderer = dev->renderer;
   for (;;) {
      struct npt_gsc_present_entry entry;
      mtx_lock(&c->wsi_mutex);
      while (c->wsi_head == c->wsi_tail &&
             !atomic_load(&c->wsi_thread_exit) &&
             atomic_load(&c->gen_state) == NPT_GSC_GEN_ACTIVE)
         cnd_wait(&c->wsi_cond, &c->wsi_mutex);
      if (atomic_load(&c->wsi_thread_exit) ||
          atomic_load(&c->gen_state) != NPT_GSC_GEN_ACTIVE) {
         /* Drop remaining entries without presenting. */
         while (c->wsi_head != c->wsi_tail) {
            entry = c->wsi_queue[c->wsi_head % NPT_SC_WSI_QUEUE_SIZE];
            c->wsi_head++;
            /* Token release can wait for transport capacity. Never hold
             * the queue lock across cleanup needed by stop_begin/push. */
            mtx_unlock(&c->wsi_mutex);
            if (entry.wait_fence_fd >= 0)
               npt_wine_unixlib_close(entry.wait_fence_fd);
            if (entry.token)
               npt_event_release_token(dev, entry.token);
            npt_gsc_frame_latency_release(c);
            mtx_lock(&c->wsi_mutex);
         }
         mtx_unlock(&c->wsi_mutex);
         break;
      }
      entry = c->wsi_queue[c->wsi_head % NPT_SC_WSI_QUEUE_SIZE];
      c->wsi_head++;
      mtx_unlock(&c->wsi_mutex);

      if (entry.wait_for_gpu) {
         ULONGLONG deadline = GetTickCount64() + 5000;
         HRESULT hr = S_FALSE;
         while (!atomic_load(&c->wsi_thread_exit) &&
                atomic_load(&c->gen_state) == NPT_GSC_GEN_ACTIVE) {
            hr = c->gpu_poll ? c->gpu_poll(c->gpu_poll_ctx, entry.image_index,
                                           entry.producer_fence_val) : NPT_E_FAIL;
            if (hr != S_FALSE)
               break;
            if (GetTickCount64() >= deadline) {
               hr = NPT_E_FAIL;
               break;
            }
            Sleep(1);
         }
         if (hr == S_OK)
            atomic_store(&c->slot_completed_val[entry.image_index],
                         entry.producer_fence_val);
         else {
            if (NPT_FAILED(hr))
               npt_gsc_fail_generation(c, hr);
            if (entry.wait_fence_fd >= 0)
               npt_wine_unixlib_close(entry.wait_fence_fd);
            if (entry.token)
               npt_event_release_token(dev, entry.token);
            npt_gsc_frame_latency_release(c);
            continue;
         }
      }
      if (atomic_load(&c->wsi_thread_exit) ||
          atomic_load(&c->gen_state) != NPT_GSC_GEN_ACTIVE) {
         if (entry.wait_fence_fd >= 0)
            npt_wine_unixlib_close(entry.wait_fence_fd);
         if (entry.token)
            npt_event_release_token(dev, entry.token);
         npt_gsc_frame_latency_release(c);
         continue;
      }
      atomic_store(&c->slot_state[entry.image_index], NPT_GSC_SLOT_SUBMITTED);

      /* Owned by entry now; the backend closes/consumes the fd. */
      int rc = npt_renderer_wsi_present(renderer, c->wsi, entry.image_index,
                                        entry.wait_fence_fd, entry.present_time_ns);
      if (entry.token)
         npt_event_release_token(dev, entry.token);
      npt_gsc_frame_latency_release(c);
      if (rc < 0) {
         if (rc == -2) {
            int expected = NPT_GSC_GEN_ACTIVE;
            atomic_compare_exchange_strong(&c->gen_state, &expected,
                                           NPT_GSC_GEN_WINDOW_LOST);
            npt_renderer_wsi_cancel(renderer, c->wsi);
            if (c->image_release_evt)
               SetEvent(c->image_release_evt);
         } else if (atomic_load(&c->gen_state) == NPT_GSC_GEN_ACTIVE) {
            npt_gsc_fail_generation(c, NPT_E_FAIL);
         }
      }
   }
   return 0;
}

static bool
npt_gsc_wsi_start(struct npt_guest_swapchain_common *c)
{
   if (c->wsi_thread_started)
      return true;
   atomic_store_explicit(&c->wsi_thread_exit, 0, memory_order_release);
   c->wsi_head = 0;
   c->wsi_tail = 0;
   c->wsi_thread = CreateThread(NULL, 0, npt_gsc_wsi_thread, c, 0, NULL);
   if (!c->wsi_thread) {
      npt_log("%s: WSI worker CreateThread failed err=%lu", c->tag,
              GetLastError());
      return false;
   }
   c->wsi_thread_started = true;
   return true;
}


bool
npt_gsc_wsi_push(struct npt_guest_swapchain_common *c, uint32_t image_index,
                 int wait_fence_fd, uint64_t token,
                 uint64_t producer_fence_val, bool wait_for_gpu)
{
   int latency_fd = wait_fence_fd >= 0
                       ? npt_wine_unixlib_dup(wait_fence_fd) : -1;
   if (latency_fd < 0)
      wait_for_gpu = true;
   mtx_lock(&c->wsi_mutex);
   bool full = (c->wsi_tail - c->wsi_head) >= NPT_SC_WSI_QUEUE_SIZE ||
               c->latency_count >= NPT_SC_PRESENT_LATENCY;
   if (full || atomic_load(&c->wsi_thread_exit) ||
       atomic_load(&c->gen_state) != NPT_GSC_GEN_ACTIVE) {
      mtx_unlock(&c->wsi_mutex);
      if (latency_fd >= 0)
         npt_wine_unixlib_close(latency_fd);
      if (wait_fence_fd >= 0)
         npt_wine_unixlib_close(wait_fence_fd);
      if (token)
         npt_event_release_token(c->com->base.device, token);
      atomic_store(&c->slot_fence_val[image_index], producer_fence_val);
      if (full)
         npt_gsc_fail_generation(c, NPT_E_FAIL);
      return false;
   }
   c->latency[c->latency_count++] = (struct npt_gsc_latency_entry) {
      .fd = latency_fd, .image = image_index, .value = producer_fence_val,
   };
   atomic_store(&c->slot_fence_val[image_index], producer_fence_val);
   atomic_store(&c->slot_state[image_index], NPT_GSC_SLOT_QUEUED);
   c->wsi_queue[c->wsi_tail % NPT_SC_WSI_QUEUE_SIZE] =
      (struct npt_gsc_present_entry){
         .image_index = image_index,
         .wait_fence_fd = wait_fence_fd,
         .token = token,
         .producer_fence_val = producer_fence_val,
         .present_time_ns = c->frame_stats_enabled ? npt_profile_now_ns() : 0,
         .wait_for_gpu = wait_for_gpu,
      };
   c->wsi_tail++;
   cnd_signal(&c->wsi_cond);
   mtx_unlock(&c->wsi_mutex);
   return true;
}

/* ------------------------------------------------------------------ */
/* latency ring                                                        */
/* ------------------------------------------------------------------ */

static bool
npt_gsc_latency_done(struct npt_guest_swapchain_common *c, DWORD timeout_ms)
{
   struct npt_gsc_latency_entry *e = &c->latency[0];
   if (e->fd < 0)
      return atomic_load(&c->slot_completed_val[e->image]) >= e->value;
   if (npt_unixlib_ensure_init() != 0)
      return false;
   struct npt_unix_wait_fd_params p = {
      .fd = e->fd, .timeout_ms = timeout_ms, .result = -1,
   };
   npt_wine_unix_call(npt_unix_wait_fd, &p);
   return p.result > 0;
}

static void
npt_gsc_latency_pop(struct npt_guest_swapchain_common *c)
{
   if (c->latency[0].fd >= 0)
      npt_wine_unixlib_close(c->latency[0].fd);
   for (uint32_t i = 1; i < c->latency_count; i++)
      c->latency[i - 1] = c->latency[i];
   c->latency[--c->latency_count].fd = -1;
}

void
npt_gsc_backpressure_wait(struct npt_guest_swapchain_common *c)
{
   UINT cap = c->max_frame_latency;
   if (cap > NPT_SC_PRESENT_LATENCY)
      cap = NPT_SC_PRESENT_LATENCY;
   while (c->latency_count >= cap &&
          atomic_load(&c->gen_state) == NPT_GSC_GEN_ACTIVE) {
      if (npt_gsc_latency_done(c, 1))
         npt_gsc_latency_pop(c);
      else if (c->latency[0].fd < 0)
         Sleep(1);
   }
}

static void
npt_gsc_backpressure_flush(struct npt_guest_swapchain_common *c)
{
   while (c->latency_count)
      npt_gsc_latency_pop(c);
}

/* ------------------------------------------------------------------ */
/* present pool + WSI                                                  */
/* ------------------------------------------------------------------ */

void
npt_gsc_release_pool(struct npt_guest_swapchain_common *c)
{
   struct npt_device *dev = c->com->base.device;
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      npt_gsc_release_held(dev, &c->present_tex[i], &c->present_tex_id[i]);
}

void
npt_gsc_wsi_stop_begin(struct npt_guest_swapchain_common *c, bool shutting_down)
{
   (void)shutting_down;
   int expected = NPT_GSC_GEN_ACTIVE;
   atomic_compare_exchange_strong(&c->gen_state, &expected,
                                  NPT_GSC_GEN_CANCELLED);
   atomic_store(&c->wsi_thread_exit, 1);
   atomic_store(&c->drain_exit, 1);
   if (c->wsi_initialized)
      npt_renderer_wsi_cancel(c->com->base.device->renderer, c->wsi);
   mtx_lock(&c->wsi_mutex);
   cnd_broadcast(&c->wsi_cond);
   mtx_unlock(&c->wsi_mutex);
   if (c->image_release_evt)
      SetEvent(c->image_release_evt);
}

HRESULT
npt_gsc_wsi_join(struct npt_guest_swapchain_common *c, bool do_not_wait)
{
   HANDLE *handles[] = { &c->wsi_thread, &c->drain_thread };
   bool *started[] = { &c->wsi_thread_started, &c->drain_started };
   for (unsigned i = 0; i < 2; i++) {
      if (!*started[i])
         continue;
      DWORD result = WaitForSingleObject(*handles[i], do_not_wait ? 0 : INFINITE);
      if (result == WAIT_TIMEOUT)
         return S_FALSE;
      if (result != WAIT_OBJECT_0)
         return NPT_E_FAIL;
      CloseHandle(*handles[i]);
      *handles[i] = NULL;
      *started[i] = false;
   }
   return S_OK;
}

void
npt_gsc_wsi_finish(struct npt_guest_swapchain_common *c)
{
   npt_gsc_backpressure_flush(c);
   if (c->wsi_initialized)
      npt_renderer_wsi_destroy(c->com->base.device->renderer, c->wsi);
   c->wsi = 0;
   c->wsi_initialized = false;
   c->wsi_hdr = false;
   c->frame_stats_enabled = false;
   c->wsi_failed = false;
   c->wsi_head = c->wsi_tail = 0;
   atomic_store(&c->occluded_streak, 0);
   atomic_store(&c->gen_error_hr, 0);
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      atomic_store(&c->slot_state[i], NPT_GSC_SLOT_FREE);
      atomic_store(&c->slot_fence_val[i], 0);
      atomic_store(&c->slot_completed_val[i], 0);
   }
   atomic_store(&c->gen_state, NPT_GSC_GEN_ACTIVE);
}

static DXGI_COLOR_SPACE_TYPE
npt_gsc_default_color_space(uint32_t format)
{
   return format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
          format == DXGI_FORMAT_R16G16B16A16_TYPELESS
             ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
             : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

DXGI_COLOR_SPACE_TYPE
npt_gsc_color_space_for_resize(uint32_t old_format, uint32_t new_format,
                               DXGI_COLOR_SPACE_TYPE current_color_space)
{
   bool fp16 = new_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
               new_format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
   bool rgb10 = new_format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                new_format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
   struct npt_gsc_format_plan plan;
   if (old_format == new_format &&
       npt_gsc_get_format_plan(new_format, current_color_space, &plan) &&
       (current_color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        (fp16 && current_color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) ||
        (rgb10 && current_color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)))
      return current_color_space;
   return npt_gsc_default_color_space(new_format);
}

bool
npt_gsc_get_format_plan(uint32_t format, DXGI_COLOR_SPACE_TYPE color_space,
                        struct npt_gsc_format_plan *out)
{
   struct npt_gsc_format_plan plan = { 0 };
   struct npt_unix_wsi_hdr_caps_args caps = {0};
   if (format == DXGI_FORMAT_R10G10B10A2_UNORM ||
       format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
       format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
       format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
      npt_wsi_query_hdr_caps(&caps);
   if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
       (!caps.hdr_supported || !(caps.format_flags & NPT_HDR_FMT_R10G10B10A2)))
      return false;
   switch (format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      plan.pool_format = DXGI_FORMAT_B8G8R8A8_UNORM;
      break;
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      plan.pool_format = DXGI_FORMAT_B8G8R8X8_UNORM;
      break;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      plan.pool_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      break;
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      plan.pool_format = format;
      break;
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      if (caps.hdr_supported && (caps.format_flags & NPT_HDR_FMT_R10G10B10A2) &&
           color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
         plan.pool_format = DXGI_FORMAT_R10G10B10A2_UNORM;
         break;
      }
      plan = (struct npt_gsc_format_plan) {
         DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
         DXGI_FORMAT_B8G8R8A8_UNORM, true
      };
      break;
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      if (caps.hdr_supported && (caps.format_flags & NPT_HDR_FMT_R16G16B16A16F) &&
           color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
         plan.pool_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
         break;
      }
      if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
         plan.pool_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      else if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
         plan.pool_format = DXGI_FORMAT_B8G8R8A8_UNORM;
      else
         return false;
      plan.srv_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      plan.rtv_format = plan.pool_format;
      plan.needs_convert = true;
      break;
   default:
      return false;
   }
   *out = plan;
   return true;
}

bool
npt_gsc_wsi_backend_available(struct npt_guest_swapchain_common *c)
{
   struct npt_renderer *renderer = c->com->base.device->renderer;
   if (!renderer->ops.wsi_init || !renderer->ops.create_host_blob) {
      npt_log("%s: transport has no WSI backend; presents will be dropped",
              c->tag);
      c->wsi_failed = true;
      return false;
   }
   return true;
}

/* Bind the exportable pool and start the worker threads. */
bool
npt_gsc_wsi_bringup(struct npt_guest_swapchain_common *c, uint32_t fmt)
{
   struct npt_renderer *renderer = c->com->base.device->renderer;

   int fds[NPT_SC_PRESENT_IMAGES];
   uint32_t strides[NPT_SC_PRESENT_IMAGES];
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      fds[i] = -1;

   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      struct npt_shared_texture_desc exp;
      memset(&exp, 0, sizeof(exp));
      if (!npt_shared_texture_export_blob(c->present_tex[i], &exp))
         goto fail;
      if (exp.plane_count != 1 || exp.modifier != 0 /* LINEAR */)
         npt_log("%s: unexpected export (planes=%u modifier=0x%llx)",
                 c->tag, exp.plane_count, (unsigned long long)exp.modifier);
      strides[i] = (uint32_t)exp.planes[0].pitch;

      /* Page-align defensively; the blob create is page-granular. */
      const uint64_t size = (exp.allocation_size + 4095u) & ~4095ull;
      fds[i] = npt_renderer_create_host_blob(renderer, exp.blob_id, size);
      if (fds[i] < 0) {
         npt_log("%s: blob claim failed for image %u", c->tag, i);
         goto fail;
      }
   }

   if (!npt_renderer_wsi_init(renderer, NPT_SC_PRESENT_IMAGES, fds, strides,
                              npt_shared_texture_virgl_format(fmt),
                              c->desc.Width, c->desc.Height,
                              (uint64_t)(uintptr_t)c->hwnd, &c->wsi,
                              &c->frame_stats_enabled,
                              (fmt == DXGI_FORMAT_R10G10B10A2_UNORM ||
                               fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
                                 ? c->color_space : 0,
                              (fmt == DXGI_FORMAT_R10G10B10A2_UNORM ||
                               fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
                                 ? &c->hdr_meta : NULL)) {
      npt_log("%s: WSI init failed (hwnd=%p %ux%u)", c->tag,
              (void *)c->hwnd, c->desc.Width, c->desc.Height);
      goto fail;
   }

   /* The unixlib dup'd the fds; ours are done. */
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      npt_wine_unixlib_close(fds[i]);
      fds[i] = -1;
   }

   c->wsi_initialized = true;
   c->wsi_hdr = fmt == DXGI_FORMAT_R10G10B10A2_UNORM ||
                fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
   if (!npt_gsc_drain_start(c) || !npt_gsc_wsi_start(c)) {
      npt_log("%s: present worker startup failed", c->tag);
      npt_gsc_fail_generation(c, NPT_E_FAIL);
      npt_gsc_wsi_stop_begin(c, false);
      c->wsi_failed = true;
      return false;
   }
   return true;

fail:
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      if (fds[i] >= 0)
         npt_wine_unixlib_close(fds[i]);
   }
   npt_gsc_release_pool(c);
   c->wsi_failed = true;
   return false;
}

HRESULT
npt_gsc_present_test(struct npt_guest_swapchain_common *c)
{
   int state = atomic_load(&c->gen_state);
   if (state == NPT_GSC_GEN_FAILED)
      return (HRESULT)atomic_load(&c->gen_error_hr);
   if (c->wsi_failed || state != NPT_GSC_GEN_ACTIVE)
      return NPT_DXGI_STATUS_OCCLUDED;
   if (atomic_load(&c->occluded_streak) >= NPT_SC_OCCLUDED_STREAK) {
      for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
         if (atomic_load(&c->slot_state[i]) == NPT_GSC_SLOT_FREE) {
            atomic_store(&c->occluded_streak, 0);
            return NPT_S_OK;
         }
      }
      return NPT_DXGI_STATUS_OCCLUDED;
   }
   return NPT_S_OK;
}

void
npt_gsc_fail_generation(struct npt_guest_swapchain_common *c, HRESULT hr)
{
   HRESULT err = NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   int expected = 0;
   atomic_compare_exchange_strong(&c->gen_error_hr, &expected, (int)err);
   atomic_store(&c->gen_state, NPT_GSC_GEN_FAILED);
   if (c->wsi_initialized && c->com && c->com->base.device &&
       c->com->base.device->renderer)
      npt_renderer_wsi_cancel(c->com->base.device->renderer, c->wsi);
   mtx_lock(&c->wsi_mutex);
   cnd_broadcast(&c->wsi_cond);
   mtx_unlock(&c->wsi_mutex);
   if (c->image_release_evt)
      SetEvent(c->image_release_evt);
}

HRESULT
npt_gsc_reserve_slot(struct npt_guest_swapchain_common *c, uint32_t image,
                     bool do_not_wait)
{
   ULONGLONG deadline = GetTickCount64() +
                        (do_not_wait ? 0 : NPT_SC_REUSE_WAIT_MS);
   for (;;) {
      int state = atomic_load(&c->gen_state);
      if (state == NPT_GSC_GEN_FAILED)
         return (HRESULT)atomic_load(&c->gen_error_hr);
      if (state != NPT_GSC_GEN_ACTIVE)
         return NPT_DXGI_STATUS_OCCLUDED;

      bool free = atomic_load(&c->slot_state[image]) == NPT_GSC_SLOT_FREE;
      bool gpu_pending = false;
      if (free) {
         uint64_t fence = atomic_load(&c->slot_fence_val[image]);
         HRESULT done = fence ? (c->gpu_poll
            ? c->gpu_poll(c->gpu_poll_ctx, image, fence) : NPT_E_FAIL) : S_OK;
         if (NPT_FAILED(done)) {
            npt_gsc_fail_generation(c, done);
            return done;
         }
         if (done == S_OK) {
            atomic_store(&c->occluded_streak, 0);
            atomic_store(&c->slot_state[image], NPT_GSC_SLOT_RESERVED);
            return NPT_S_OK;
         }
         gpu_pending = true;
      }
      ULONGLONG now = GetTickCount64();
      if (now >= deadline)
         break;
      DWORD wait_ms = (DWORD)(deadline - now);
      if (gpu_pending)
         Sleep(1);
      else if (c->image_release_evt)
         WaitForSingleObject(c->image_release_evt, wait_ms);
      else
         Sleep(wait_ms < 5 ? wait_ms : 5);
   }
   if (do_not_wait)
      return NPT_DXGI_ERROR_WAS_STILL_DRAWING;
   if (atomic_load(&c->occluded_streak) < NPT_SC_OCCLUDED_STREAK)
      atomic_fetch_add(&c->occluded_streak, 1);
   return NPT_DXGI_STATUS_OCCLUDED;
}

HRESULT
npt_gsc_present_validate(const struct npt_guest_swapchain_common *c,
                         UINT SyncInterval, UINT Flags)
{
   if (SyncInterval > 4)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if ((Flags & NPT_DXGI_PRESENT_ALLOW_TEARING) &&
       (SyncInterval ||
        !(c->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ||
        c->fullscreen))
      return NPT_DXGI_ERROR_INVALID_CALL;
   return NPT_S_OK;
}

bool
npt_gsc_present_would_block(struct npt_guest_swapchain_common *c,
                            uint32_t image)
{
   UINT cap = c->max_frame_latency;
   if (cap > NPT_SC_PRESENT_LATENCY)
      cap = NPT_SC_PRESENT_LATENCY;
   while (c->latency_count && npt_gsc_latency_done(c, 0))
      npt_gsc_latency_pop(c);
   return atomic_load(&c->slot_state[image]) != NPT_GSC_SLOT_FREE ||
          c->latency_count >= cap;
}

void
npt_gsc_query_window_size(struct npt_guest_swapchain_common *c,
                          UINT *width, UINT *height)
{
   RECT rect;
   if (c->hwnd && GetClientRect(c->hwnd, &rect)) {
      *width = (UINT)(rect.right - rect.left);
      *height = (UINT)(rect.bottom - rect.top);
   }
   if (!*width)
      *width = 1;
   if (!*height)
      *height = 1;
}

/* Resize a windowed swap chain's client area to w x h. */
void
npt_gsc_window_resize(HWND hwnd, uint32_t w, uint32_t h)
{
   RECT newRect = { 0, 0, (LONG)w, (LONG)h };
   RECT oldRect = { 0, 0, 0, 0 };
   GetWindowRect(hwnd, &oldRect);
   AdjustWindowRectEx(&newRect, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                      GetWindowLongW(hwnd, GWL_EXSTYLE));
   SetRect(&newRect, 0, 0, newRect.right - newRect.left,
           newRect.bottom - newRect.top);
   OffsetRect(&newRect, oldRect.left, oldRect.top);
   MoveWindow(hwnd, newRect.left, newRect.top,
              newRect.right - newRect.left, newRect.bottom - newRect.top,
              TRUE);
}

/* ------------------------------------------------------------------ */
/* outputs                                                             */
/* ------------------------------------------------------------------ */

void
npt_gsc_assign_output(struct npt_guest_swapchain_common *c,
                      IDXGIOutput **slot, IDXGIOutput *new_val)
{
   IDXGIOutput *old;
   mtx_lock(&c->output_lock);
   old = *slot;
   if (new_val)
      npt_gsc_output_vtbl(new_val)->AddRef(new_val);
   *slot = new_val;
   mtx_unlock(&c->output_lock);
   if (old)
      npt_gsc_output_vtbl(old)->Release(old);
}

IDXGIOutput *
npt_gsc_load_output_addref(struct npt_guest_swapchain_common *c,
                           IDXGIOutput **slot)
{
   IDXGIOutput *v;
   mtx_lock(&c->output_lock);
   v = *slot;
   if (v)
      npt_gsc_output_vtbl(v)->AddRef(v);
   mtx_unlock(&c->output_lock);
   return v;
}

void
npt_gsc_release_outputs(struct npt_guest_swapchain_common *c)
{
   if (c->fullscreen_target)
      npt_gsc_output_vtbl(c->fullscreen_target)->Release(c->fullscreen_target);
   if (c->containing_output)
      npt_gsc_output_vtbl(c->containing_output)->Release(c->containing_output);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void
npt_gsc_init(struct npt_guest_swapchain_common *c, struct npt_com_base *com,
             const DXGI_SWAP_CHAIN_DESC1 *desc1,
             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
             HWND hwnd, const char *tag)
{
   c->com = com;
   c->tag = tag;
   npt_private_data_init(&c->private_data);
   mtx_init(&c->present_mutex, mtx_plain);
   mtx_init(&c->output_lock, mtx_plain);
   mtx_init(&c->wsi_mutex, mtx_plain);
   cnd_init(&c->wsi_cond);
   atomic_init(&c->gen_state, NPT_GSC_GEN_ACTIVE);
   atomic_init(&c->gen_error_hr, 0);
   atomic_init(&c->occluded_streak, 0);
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      atomic_init(&c->slot_state[i], NPT_GSC_SLOT_FREE);
      atomic_init(&c->slot_fence_val[i], 0);
      atomic_init(&c->slot_completed_val[i], 0);
   }
   /* Auto-reset: an unconsumed signal latches, so a release that races
    * the reuse gate's inflight check is not lost. */
   c->image_release_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
   for (uint32_t i = 0; i < NPT_SC_PRESENT_LATENCY; i++)
      c->latency[i].fd = -1;

   c->desc = *desc1;
   c->color_space = npt_gsc_default_color_space(c->desc.Format);
   c->color_space_dirty = false;
   if (fs_desc) {
      c->fs_desc = *fs_desc;
   } else {
      memset(&c->fs_desc, 0, sizeof(c->fs_desc));
      c->fs_desc.Windowed = 1;
   }
   c->hwnd = hwnd;
   /* Born windowed; a fullscreen creation desc is realized by the
    * factory's post-creation npt_swapchain_apply_initial_fullscreen ->
    * SetFullscreenState. */
   c->fullscreen = 0;
   /* Waitable swapchains default to 1 frame of latency (DXGI); others
    * to 3.  The waitable object is a semaphore released once per
    * frame. */
   const bool waitable =
      (c->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
   c->max_frame_latency = waitable ? 1 : 3;
   if (waitable) {
      c->frame_latency_sem = CreateSemaphoreW(
         NULL, (LONG)c->max_frame_latency,
         (LONG)NPT_SC_MAX_SWAP_CHAIN_BUFFERS, NULL);
      if (!c->frame_latency_sem)
         npt_log("%s: frame-latency semaphore create failed err=%lu; "
                 "waitable object unavailable", c->tag, GetLastError());
   }
   c->background.a = 1.0f;

   /* Normalize: DXGI fills zero dimensions from the window and
    * defaults the format. */
   if (!c->desc.Width || !c->desc.Height) {
      UINT w = c->desc.Width, h = c->desc.Height;
      npt_gsc_query_window_size(c, &w, &h);
      c->desc.Width = w;
      c->desc.Height = h;
   }
   if (!c->desc.Format)
      c->desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   if (!c->desc.SampleDesc.Count)
      c->desc.SampleDesc.Count = 1;
}

void
npt_gsc_fini(struct npt_guest_swapchain_common *c)
{
   cnd_destroy(&c->wsi_cond);
   mtx_destroy(&c->wsi_mutex);
   mtx_destroy(&c->present_mutex);
   mtx_destroy(&c->output_lock);
   npt_private_data_fini(&c->private_data);
   /* Workers joined by the WSI teardown, so these are unreferenced. */
   if (c->image_release_evt)
      CloseHandle(c->image_release_evt);
   if (c->frame_latency_sem)
      CloseHandle(c->frame_latency_sem);
}

/* ------------------------------------------------------------------ */
/* IUnknown / IDXGIObject                                              */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_QueryInterface(void *self, REFIID riid, void **ppvObject)
{
   NPT_API_TRACE("api=swapchain phase=enter self=%p iid_ptr=%p", self, (const void *)riid);
   if (self && ppvObject)
      npt_api_trace_guid("swapchain", "iid", riid);
   HRESULT hr = npt_com_default_query_interface_chain(
      self, riid, ppvObject, npt_gsc_qi_chain);
   hr = hr == NPT_S_OK ? hr : NPT_E_NOINTERFACE;
   NPT_API_TRACE("api=swapchain phase=leave hr=0x%08lx out=%p",
                 (unsigned long)(uint32_t)hr,
                 hr == NPT_S_OK && ppvObject ? *ppvObject : NULL);
   return hr;
}

ULONG NPT_STDMETHODCALLTYPE
npt_gsc_AddRef(void *self)
{
   return npt_com_default_addref(self);
}

ULONG NPT_STDMETHODCALLTYPE
npt_gsc_Release(void *self)
{
   return npt_com_default_release(self);
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateData(void *self, const GUID *Name, UINT DataSize,
                       const void *pData)
{
   return npt_private_data_set(&npt_gsc(self)->private_data, Name, DataSize,
                               pData);
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateDataInterface(void *self, const GUID *Name,
                                const IUnknown *pUnknown)
{
   return npt_private_data_set_interface(&npt_gsc(self)->private_data, Name,
                                         pUnknown);
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetPrivateData(void *self, const GUID *Name, UINT *pDataSize,
                       void *pData)
{
   return npt_private_data_get(&npt_gsc(self)->private_data, Name, pDataSize,
                               pData);
}

/* ------------------------------------------------------------------ */
/* desc / window                                                       */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc(void *self, DXGI_SWAP_CHAIN_DESC *pDesc)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   pDesc->BufferDesc.Width = c->desc.Width;
   pDesc->BufferDesc.Height = c->desc.Height;
   pDesc->BufferDesc.RefreshRate = c->fs_desc.RefreshRate;
   pDesc->BufferDesc.Format = c->desc.Format;
   pDesc->BufferDesc.ScanlineOrdering = c->fs_desc.ScanlineOrdering;
   pDesc->BufferDesc.Scaling = c->fs_desc.Scaling;
   pDesc->SampleDesc = c->desc.SampleDesc;
   pDesc->BufferUsage = c->desc.BufferUsage;
   pDesc->BufferCount = c->desc.BufferCount;
   pDesc->OutputWindow = c->hwnd;
   pDesc->Windowed = !c->fullscreen;
   pDesc->SwapEffect = c->desc.SwapEffect;
   pDesc->Flags = c->desc.Flags;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc1(void *self, DXGI_SWAP_CHAIN_DESC1 *pDesc)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   *pDesc = c->desc;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenDesc(void *self, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   *pDesc = c->fs_desc;
   pDesc->Windowed = !c->fullscreen;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetHwnd(void *self, HWND *pHwnd)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pHwnd)
      return NPT_E_POINTER;
   *pHwnd = c->hwnd;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetCoreWindow(void *self, const IID *refiid, void **ppUnk)
{
   (void)self; (void)refiid;
   if (ppUnk)
      *ppUnk = NULL;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenState(void *self, BOOL *pFullscreen,
                           IDXGIOutput **ppTarget)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (pFullscreen)
      *pFullscreen = c->fullscreen;
   if (ppTarget)
      *ppTarget = c->fullscreen
                     ? npt_gsc_load_output_addref(c, &c->fullscreen_target)
                     : NULL;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRestrictToOutput(void *self, IDXGIOutput **ppRestrictToOutput)
{
   (void)self;
   if (!ppRestrictToOutput)
      return NPT_E_POINTER;
   *ppRestrictToOutput = NULL;
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* stats / misc state                                                  */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameStatistics(void *self, DXGI_FRAME_STATISTICS *pStats)
{
   (void)self;
   if (!pStats)
      return NPT_E_POINTER;
   memset(pStats, 0, sizeof(*pStats));
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetLastPresentCount(void *self, UINT *pLastPresentCount)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pLastPresentCount)
      return NPT_E_POINTER;
   *pLastPresentCount = c->present_count;
   return NPT_S_OK;
}

BOOL NPT_STDMETHODCALLTYPE
npt_gsc_IsTemporaryMonoSupported(void *self)
{
   (void)self;
   return 0;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetBackgroundColor(void *self, const DXGI_RGBA *pColor)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pColor)
      return NPT_E_POINTER;
   c->background = *pColor;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetBackgroundColor(void *self, DXGI_RGBA *pColor)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pColor)
      return NPT_E_POINTER;
   *pColor = c->background;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetRotation(void *self, DXGI_MODE_ROTATION Rotation)
{
   (void)self;
   return Rotation == DXGI_MODE_ROTATION_IDENTITY
             ? NPT_S_OK : NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRotation(void *self, DXGI_MODE_ROTATION *pRotation)
{
   (void)self;
   if (!pRotation)
      return NPT_E_POINTER;
   *pRotation = DXGI_MODE_ROTATION_IDENTITY;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetSourceSize(void *self, UINT Width, UINT Height)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!Width || !Height ||
       Width > c->desc.Width || Height > c->desc.Height)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (Width != c->desc.Width || Height != c->desc.Height)
      npt_log("%s: SetSourceSize %ux%u ignored (full %ux%u)", c->tag,
              Width, Height, c->desc.Width, c->desc.Height);
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetSourceSize(void *self, UINT *pWidth, UINT *pHeight)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pWidth || !pHeight)
      return NPT_E_POINTER;
   *pWidth = c->desc.Width;
   *pHeight = c->desc.Height;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMaximumFrameLatency(void *self, UINT MaxLatency)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!(c->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ||
       !MaxLatency || MaxLatency > NPT_SC_MAX_SWAP_CHAIN_BUFFERS)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Waitable object: grow the semaphore by the delta and never shrink
    * it (matching DXGI -- shrinking could strand an app mid-wait). */
   if (c->frame_latency_sem && MaxLatency > c->max_frame_latency)
      ReleaseSemaphore(c->frame_latency_sem,
                       (LONG)(MaxLatency - c->max_frame_latency), NULL);
   c->max_frame_latency = MaxLatency;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMaximumFrameLatency(void *self, UINT *pMaxLatency)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pMaxLatency)
      return NPT_E_POINTER;
   *pMaxLatency = c->max_frame_latency;
   return NPT_S_OK;
}

HANDLE NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameLatencyWaitableObject(void *self)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!c->frame_latency_sem)
      return NULL;
   /* Hand the app its own handle (DXGI semantics); the swapchain keeps
    * the original and closes it at teardown. */
   HANDLE dup = NULL;
   DuplicateHandle(GetCurrentProcess(), c->frame_latency_sem,
                   GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
   return dup;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMatrixTransform(void *self, const DXGI_MATRIX_3X2_F *pMatrix)
{
   (void)self; (void)pMatrix;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMatrixTransform(void *self, DXGI_MATRIX_3X2_F *pMatrix)
{
   (void)self;
   if (!pMatrix)
      return NPT_E_POINTER;
   memset(pMatrix, 0, sizeof(*pMatrix));
   pMatrix->_11 = 1.0f;
   pMatrix->_22 = 1.0f;
   return NPT_S_OK;
}

static bool
npt_gsc_color_space_supported(const struct npt_guest_swapchain_common *c,
                              DXGI_COLOR_SPACE_TYPE color_space)
{
   struct npt_gsc_format_plan plan;
   if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
      return npt_gsc_get_format_plan(c->desc.Format, color_space, &plan);
   if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
       (c->desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        c->desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)) {
      struct npt_unix_wsi_hdr_caps_args caps;
      npt_wsi_query_hdr_caps(&caps);
      return caps.hdr_supported && (caps.format_flags & NPT_HDR_FMT_R10G10B10A2);
   }
   return color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 &&
          (c->desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           c->desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_CheckColorSpaceSupport(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace,
                               UINT *pColorSpaceSupport)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pColorSpaceSupport)
      return NPT_E_POINTER;
   mtx_lock(&c->present_mutex);
   *pColorSpaceSupport = npt_gsc_color_space_supported(c, ColorSpace)
      ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
   mtx_unlock(&c->present_mutex);
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetColorSpace1(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   mtx_lock(&c->present_mutex);
   if (!npt_gsc_color_space_supported(c, ColorSpace)) {
      mtx_unlock(&c->present_mutex);
      return NPT_E_INVALIDARG;
   }
   if (c->color_space != ColorSpace) {
      c->color_space = ColorSpace;
      c->color_space_dirty = true;
   }
   mtx_unlock(&c->present_mutex);
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetHDRMetaData(void *self, DXGI_HDR_METADATA_TYPE Type, UINT Size,
                       void *pMetaData)
{
   static_assert(sizeof(DXGI_HDR_METADATA_HDR10) == 28, "HDR10 metadata size");
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   struct npt_unix_hdr10_metadata meta = {0};
   if (Type == DXGI_HDR_METADATA_TYPE_HDR10 && pMetaData &&
       Size == sizeof(DXGI_HDR_METADATA_HDR10)) {
      const DXGI_HDR_METADATA_HDR10 *src = pMetaData;
      meta.valid = 1;
      memcpy(meta.red_primary, src->RedPrimary, sizeof(meta.red_primary));
      memcpy(meta.green_primary, src->GreenPrimary, sizeof(meta.green_primary));
      memcpy(meta.blue_primary, src->BluePrimary, sizeof(meta.blue_primary));
      memcpy(meta.white_point, src->WhitePoint, sizeof(meta.white_point));
      meta.max_mastering_luminance = src->MaxMasteringLuminance;
      meta.min_mastering_luminance = src->MinMasteringLuminance;
      meta.max_cll = src->MaxContentLightLevel;
      meta.max_fall = src->MaxFrameAverageLightLevel;
   } else if (Type != DXGI_HDR_METADATA_TYPE_NONE || Size != 0) {
      return NPT_DXGI_ERROR_INVALID_CALL;
   }
   mtx_lock(&c->present_mutex);
   struct npt_unix_hdr10_metadata next_meta = meta;
   HRESULT hr = NPT_S_OK;
   if (c->wsi_initialized && c->wsi_hdr && !c->color_space_dirty) {
      if (!meta.valid && c->color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
         meta = (struct npt_unix_hdr10_metadata) {
            1, {35400,14600}, {8500,39850}, {6550,2300}, {15635,16450},
            1000, 50, 1000, 400
         };
      }
      if (npt_wsi_set_hdr_metadata(c->wsi, &meta) != 0)
         hr = NPT_E_FAIL;
   }
   if (hr == NPT_S_OK)
      c->hdr_meta = next_meta;
   mtx_unlock(&c->present_mutex);
   return hr;
}

#endif /* __WINE__ */
