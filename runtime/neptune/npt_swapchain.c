/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-fabricated IDXGISwapChain for the Wine path.  The host owns no
 * swapchains: presentable surfaces are exportable host textures bound to
 * virtio-gpu blob resources, presented through the X11 DRI3/Present unixlib
 * backend.
 *
 * The swapchain never touches the tier default vtbls: it swaps in its own
 * fully-populated vtbl at creation, so no wire method can ever see a
 * guest-fab object id (an unregistered id is a fatal host decode error).
 * Model:
 *
 *   backbuffer      one ordinary host texture (app's format, optimal
 *                   tiling); GetBuffer maps every index to it.
 *   present pool    NPT_SC_PRESENT_IMAGES host textures with
 *                   MISC_SHARED|LINEAR_EXPORT storage, exported once
 *                   via the SHARED transport, bound to guest blobs
 *                   whose dmabufs feed the X11 WSI.
 *   Present         CopyResource(pool[i], backbuffer) + Flush on the
 *                   DC/SC ring, then wsi_present(i).  A multisampled
 *                   backbuffer is resolved first and copied from its
 *                   resolve target; a format the pool cannot hold is
 *                   drawn into it by a pre-recorded command list.
 *                   Pool reuse is gated on X IDLE_NOTIFY release
 *                   counts drained by a per-swapchain thread.
 */

#include "npt_swapchain.h"

#include "npt_com.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_resource.h"
#include "npt_ring.h"
#include "npt_shared_texture.h"
#include "npt_swapchain_common.h"

#include <string.h>

#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"
#include "neptune-protocol/npt_protocol_guest_idxgiswapchain.h"

#if defined(__WINE__)

#include "npt_event.h"
#include "npt_present_convert_dxbc.h"
#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_client_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_client_id3d11fence.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture2d.h"
#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgidevice.h"
#include "neptune-protocol/npt_protocol_client_idxgifactory.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_guest_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_guest_id3d11fence.h"

/* Deferred context, VS, PS, backbuffer SRV, then an RTV and a command
 * list per pool image. */
#define NPT_SC_CONVERT_HELD (4 + 2 * NPT_SC_PRESENT_IMAGES)

/* DXGI usage bits the protocol types header doesn't carry. */
#define NPT_DXGI_USAGE_SHADER_INPUT      0x00000010u
#define NPT_DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020u
#define NPT_DXGI_USAGE_UNORDERED_ACCESS  0x00000040u

struct npt_guest_swapchain {
   struct npt_guest_swapchain_common base;
   struct npt_gsc_format_plan plan;

   /* Held wire wrappers (one public ref each) + their host ids for
    * liveness-guarded release at teardown (see npt_gsc_release_held). */
   void *factory;                /* creating IDXGIFactory*, may be NULL */
   uint64_t factory_id;
   void *device;                 /* ID3D11Device* */
   uint64_t device_id;
   void *dc;                     /* ID3D11DeviceContext* */
   uint64_t dc_id;
   void *backbuffer;             /* ID3D11Texture2D* */
   uint64_t backbuffer_id;
   /* Single-sample twin of a multisampled backbuffer: the pool is
    * single-sample, so presents resolve into it and copy from there. */
   void *resolved;               /* ID3D11Texture2D* */
   uint64_t resolved_id;

   /* Pool rotation cursor; the pool itself lives in the common state. */
   uint32_t next_image;

   /* Present conversion for backbuffer formats the pool cannot hold
    * (npt_gsc_get_format_plan): per pool image, a deferred-context command list
    * that draws the backbuffer into it.  The held objects live and die
    * with the pool; convert_list[] borrows from them. */
   void *convert_held[NPT_SC_CONVERT_HELD];
   uint64_t convert_held_id[NPT_SC_CONVERT_HELD];
   uint32_t convert_held_count;
   void *convert_list[NPT_SC_PRESENT_IMAGES];   /* ID3D11CommandList* */

   /* GPU-done fencing: an ID3D11Fence signaled on the DC after each
    * present copy; per-Present event tokens turn its completion into
    * a guest sync_file (see npt_event_arm_token_fd). */
   void *dc4;                    /* ID3D11DeviceContext4* */
   uint64_t dc4_id;
   void *fence;                  /* ID3D11Fence* */
   uint64_t fence_id;
   uint64_t fence_value;
   ID3D11Query *slot_query[NPT_SC_PRESENT_IMAGES];
   uint64_t slot_query_id[NPT_SC_PRESENT_IMAGES];
   ID3D11Query *drain_query;
   uint64_t drain_query_id;
   uint64_t drain_value;
   ULONGLONG drain_deadline;
   bool drain_pending;
   bool retiring;

   /* Fullscreen window management, done entirely guest-side.
    * The guest-fab swapchain owns a real HWND, so all styling/placement/
    * mode-switching is done here with plain Win32 calls; base.fullscreen
    * is the authoritative windowed/fullscreen state.
    * mode_change_in_progress rejects a transition that overlaps another
    * (DXGI_STATUS_MODE_CHANGE_IN_PROGRESS); it does not guard these fields
    * against concurrent readers. */
   _Atomic int mode_change_in_progress;
   HMONITOR    fs_monitor;          /* real Win32 monitor while fullscreen */
   LONG        fs_saved_style;
   LONG        fs_saved_exstyle;
   RECT        fs_saved_rect;
   bool        fs_did_modeset;      /* we changed the display mode */
};

static inline struct npt_guest_swapchain *
gsc(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

/* ------------------------------------------------------------------ */
/* present pool + WSI                                                  */
/* ------------------------------------------------------------------ */

/* What presents copy or convert from. */
static void *
gsc_present_source(struct npt_guest_swapchain *s)
{
   return s->resolved ? s->resolved : s->backbuffer;
}

static uint32_t
gsc_resolve_format(uint32_t backbuffer_fmt,
                   const struct npt_gsc_format_plan *plan)
{
   switch (backbuffer_fmt) {
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return plan->needs_convert ? plan->srv_format : plan->pool_format;
   default:
      return backbuffer_fmt;
   }
}

static void
gsc_convert_hold(struct npt_guest_swapchain *s, void *wrapper)
{
   const uint32_t n = s->convert_held_count++;
   s->convert_held[n] = wrapper;
   s->convert_held_id[n] = npt_com_self_id(wrapper);
}

static void
gsc_release_convert(struct npt_guest_swapchain *s, struct npt_device *dev)
{
   while (s->convert_held_count) {
      const uint32_t n = --s->convert_held_count;
      npt_gsc_release_held(dev, &s->convert_held[n], &s->convert_held_id[n]);
   }
   memset(s->convert_list, 0, sizeof(s->convert_list));
}

static HRESULT
gsc_gpu_poll(void *ctx, uint32_t image, uint64_t value)
{
   struct npt_guest_swapchain *s = ctx;
   if (s->fence) {
      const struct npt_id3d11fence_client_vtbl *v =
         (const void *)((struct npt_com_base *)s->fence)->lpVtbl;
      UINT64 done = v->GetCompletedValue(s->fence);
      if (done == UINT64_MAX)
         return NPT_DXGI_ERROR_DEVICE_REMOVED;
      return done >= value ? S_OK : S_FALSE;
   }
   ID3D11Query *query = image < NPT_SC_PRESENT_IMAGES
                           ? s->slot_query[image] : s->drain_query;
   if (!query || !s->dc)
      return NPT_E_FAIL;
   const struct npt_id3d11devicecontext_client_vtbl *v =
      (const void *)((struct npt_com_base *)s->dc)->lpVtbl;
   BOOL done = FALSE;
   HRESULT hr = v->GetData(s->dc, (ID3D11Asynchronous *)query,
                           &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
   if (NPT_FAILED(hr))
      return hr;
   return hr == S_OK && done ? S_OK : S_FALSE;
}

static HRESULT
gsc_create_query(struct npt_guest_swapchain *s, ID3D11Query **slot,
                 uint64_t *id)
{
   if (*slot)
      return S_OK;
   const struct npt_id3d11device_client_vtbl *dv =
      (const void *)((struct npt_com_base *)s->device)->lpVtbl;
   D3D11_QUERY_DESC desc = { D3D11_QUERY_EVENT, 0 };
   HRESULT hr = dv->CreateQuery(s->device, &desc, slot);
   if (NPT_FAILED(hr) || !*slot)
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   *id = npt_com_self_id(*slot);
   return S_OK;
}

static HRESULT
gsc_gpu_drain_context(struct npt_guest_swapchain *s, bool do_not_wait)
{
   struct npt_guest_swapchain_common *c = &s->base;
   if (!s->drain_pending && !s->backbuffer && !c->wsi_initialized)
      return S_OK;
   if (!s->dc)
      return NPT_E_FAIL;
   const struct npt_id3d11devicecontext_client_vtbl *v =
      (const void *)((struct npt_com_base *)s->dc)->lpVtbl;
   if (!s->drain_pending) {
      if (s->dc4 && s->fence) {
         const struct npt_id3d11devicecontext4_client_vtbl *v4 =
            (const void *)((struct npt_com_base *)s->dc4)->lpVtbl;
         s->drain_value = ++s->fence_value;
         HRESULT hr = v4->Signal(s->dc4, (ID3D11Fence *)s->fence,
                                 s->drain_value);
         if (NPT_FAILED(hr))
            return hr;
      } else {
         HRESULT hr = gsc_create_query(s, &s->drain_query, &s->drain_query_id);
         if (NPT_FAILED(hr))
            return hr;
         s->drain_value = ++s->fence_value;
         v->End(s->dc, (ID3D11Asynchronous *)s->drain_query);
      }
      v->Flush(s->dc);
      s->drain_pending = true;
      s->drain_deadline = GetTickCount64() + 5000;
   }
   for (;;) {
      HRESULT hr = gsc_gpu_poll(s, NPT_SC_PRESENT_IMAGES, s->drain_value);
      if (hr == S_OK) {
         s->drain_pending = false;
         return S_OK;
      }
      if (NPT_FAILED(hr))
         return hr;
      if (GetTickCount64() >= s->drain_deadline)
         return NPT_E_FAIL;
      if (do_not_wait)
         return NPT_DXGI_ERROR_WAS_STILL_DRAWING;
      Sleep(1);
   }
}

/* Record, per pool image, a fullscreen triangle loading the backbuffer
 * into that image.  Present executes it with RestoreContextState, which
 * leaves the app's immediate-context state as it was. */
static bool
gsc_record_convert(struct npt_guest_swapchain *s)
{
   struct npt_guest_swapchain_common *c = &s->base;
   const struct npt_id3d11device_client_vtbl *dv =
      (const void *)((struct npt_com_base *)s->device)->lpVtbl;

   ID3D11DeviceContext *ctx = NULL;
   ID3D11VertexShader *vs = NULL;
   ID3D11PixelShader *ps = NULL;
   ID3D11ShaderResourceView *srv = NULL;
   HRESULT hr = dv->CreateDeferredContext(s->device, 0, &ctx);
   if (NPT_FAILED(hr) || !ctx)
      goto fail;
   gsc_convert_hold(s, ctx);

   hr = dv->CreateVertexShader(s->device, npt_present_convert_vs,
                               sizeof(npt_present_convert_vs), NULL, &vs);
   if (NPT_FAILED(hr) || !vs)
      goto fail;
   gsc_convert_hold(s, vs);

   hr = dv->CreatePixelShader(s->device, npt_present_convert_ps,
                              sizeof(npt_present_convert_ps), NULL, &ps);
   if (NPT_FAILED(hr) || !ps)
      goto fail;
   gsc_convert_hold(s, ps);

   D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
   srv_desc.Format = (DXGI_FORMAT)s->plan.srv_format;
   srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
   srv_desc.Texture2D.MostDetailedMip = 0;
   srv_desc.Texture2D.MipLevels = 1;
   hr = dv->CreateShaderResourceView(s->device,
                                     (ID3D11Resource *)gsc_present_source(s),
                                     &srv_desc, &srv);
   if (NPT_FAILED(hr) || !srv)
      goto fail;
   gsc_convert_hold(s, srv);

   const struct npt_id3d11devicecontext_client_vtbl *cv =
      (const void *)((struct npt_com_base *)ctx)->lpVtbl;
   const D3D11_VIEWPORT vp = {
      0.0f, 0.0f, (float)c->desc.Width, (float)c->desc.Height, 0.0f, 1.0f,
   };
   D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {0};
   rtv_desc.Format = (DXGI_FORMAT)s->plan.rtv_format;
   rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
   rtv_desc.Texture2D.MipSlice = 0;
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      ID3D11RenderTargetView *rtv = NULL;
      hr = dv->CreateRenderTargetView(
         s->device, (ID3D11Resource *)c->present_tex[i], &rtv_desc, &rtv);
      if (NPT_FAILED(hr) || !rtv)
         goto fail;
      gsc_convert_hold(s, rtv);

      cv->IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      cv->VSSetShader(ctx, vs, NULL, 0);
      cv->PSSetShader(ctx, ps, NULL, 0);
      cv->PSSetShaderResources(ctx, 0, 1, &srv);
      cv->RSSetViewports(ctx, 1, &vp);
      cv->OMSetRenderTargets(ctx, 1, &rtv, NULL);
      cv->Draw(ctx, 3, 0);

      ID3D11CommandList *list = NULL;
      hr = cv->FinishCommandList(ctx, FALSE, &list);
      if (NPT_FAILED(hr) || !list)
         goto fail;
      gsc_convert_hold(s, list);
      s->convert_list[i] = list;
   }
   return true;

fail:
   npt_log("guest swapchain: present conversion fmt=%u setup failed hr=0x%x",
           c->desc.Format, hr);
   gsc_release_convert(s, c->com->base.device);
   return false;
}

static HRESULT
gsc_teardown_wsi(struct npt_guest_swapchain *s, bool shutting_down,
                 bool do_not_wait)
{
   struct npt_guest_swapchain_common *c = &s->base;
   s->retiring = true;
   npt_gsc_wsi_stop_begin(c, shutting_down);
   HRESULT hr = npt_gsc_wsi_join(c, do_not_wait);
   if (hr == S_FALSE)
      return NPT_DXGI_ERROR_WAS_STILL_DRAWING;
   if (NPT_FAILED(hr)) {
      npt_gsc_fail_generation(c, hr);
      return hr;
   }
   if (shutting_down)
      return NPT_S_OK;
   hr = gsc_gpu_drain_context(s, do_not_wait);
   if (hr == NPT_DXGI_ERROR_WAS_STILL_DRAWING)
      return hr;
   if (NPT_FAILED(hr)) {
      npt_gsc_fail_generation(c, hr);
      return hr;
   }
   npt_gsc_wsi_finish(c);
   gsc_release_convert(s, c->com->base.device);
   npt_gsc_release_pool(c);
   s->next_image = 0;
   s->retiring = false;
   return NPT_S_OK;
}

static HRESULT
gsc_recreate_resolved(struct npt_guest_swapchain *s, uint32_t format)
{
   struct npt_guest_swapchain_common *c = &s->base;
   D3D11_TEXTURE2D_DESC d = {0};
   d.Width = c->desc.Width;
   d.Height = c->desc.Height;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = (DXGI_FORMAT)format;
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   const struct npt_id3d11device_client_vtbl *v =
      (const void *)((struct npt_com_base *)s->device)->lpVtbl;
   ID3D11Texture2D *tex = NULL;
   HRESULT hr = v->CreateTexture2D(s->device, &d, NULL, &tex);
   if (NPT_FAILED(hr) || !tex)
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   npt_gsc_release_held(c->com->base.device, &s->resolved, &s->resolved_id);
   s->resolved = tex;
   s->resolved_id = npt_com_self_id(tex);
   return NPT_S_OK;
}

/* First Present: build the exportable pool and hand its dmabufs to
 * the WSI backend.  The window must exist by now (the app rendered a
 * frame), which is why this is not done at creation time. */
static void
gsc_ensure_wsi(struct npt_guest_swapchain *s)
{
   struct npt_guest_swapchain_common *c = &s->base;

   if (!npt_gsc_wsi_backend_available(c))
      return;

   const uint32_t fmt = s->plan.pool_format;
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      void *tex = npt_shared_texture_create_exportable(
         s->device, c->desc.Width, c->desc.Height, fmt);
      if (!tex) {
         npt_gsc_release_pool(c);
         c->wsi_failed = true;
         return;
      }
      c->present_tex[i] = tex;
      c->present_tex_id[i] = npt_com_self_id(tex);
   }

   if (s->plan.needs_convert && !gsc_record_convert(s)) {
      npt_gsc_release_pool(c);
      c->wsi_failed = true;
      return;
   }

   if (!npt_gsc_wsi_bringup(c, fmt) && !c->wsi_initialized)
      gsc_release_convert(s, c->com->base.device);
}

/* ------------------------------------------------------------------ */
/* IDXGIObject / IDXGIDeviceSubObject                                  */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetParent(void *self, const IID *riid, void **ppParent)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (s->factory)
      return npt_gsc_qi_held(s->factory, riid, ppParent);

   /* No creating factory recorded (D3D11CreateDeviceAndSwapChain):
    * walk device -> IDXGIDevice -> adapter -> factory. */
   if (!ppParent)
      return NPT_E_POINTER;
   *ppParent = NULL;

   IDXGIDevice *dxgi_dev = NULL;
   HRESULT hr = npt_gsc_qi_held(s->device, &NPT_IID_IDXGIDevice,
                                (void **)&dxgi_dev);
   if (NPT_FAILED(hr) || !dxgi_dev)
      return NPT_E_FAIL;
   const struct npt_idxgidevice_client_vtbl *dv =
      (const void *)((struct npt_com_base *)dxgi_dev)->lpVtbl;

   IDXGIAdapter *adp = NULL;
   hr = dv->GetAdapter(dxgi_dev, &adp);
   if (NPT_FAILED(hr) || !adp) {
      dv->Release(dxgi_dev);
      return NPT_E_FAIL;
   }
   const struct npt_idxgiadapter_client_vtbl *av =
      (const void *)((struct npt_com_base *)adp)->lpVtbl;
   hr = av->GetParent(adp, riid, ppParent);
   av->Release(adp);
   dv->Release(dxgi_dev);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetDevice(void *self, const IID *riid, void **ppDevice)
{
   return npt_gsc_qi_held(gsc(self)->device, riid, ppDevice);
}

/* ------------------------------------------------------------------ */
/* Present                                                             */
/* ------------------------------------------------------------------ */

static HRESULT
gsc_present_common(void *self, UINT SyncInterval, UINT Flags)
{
   struct npt_guest_swapchain *s = gsc(self);
   struct npt_guest_swapchain_common *c = &s->base;

   HRESULT hr = npt_gsc_present_validate(c, SyncInterval, Flags);
   if (NPT_FAILED(hr))
      return hr;
   if (Flags & NPT_DXGI_PRESENT_TEST)
      return npt_gsc_present_test(c);

   /* Frame-time instrumentation (NPT_DEBUG=present_timing).  t0=entry,
    * t1=after backpressure, t2=after the host present work is issued,
    * t3=after the WSI flip is queued. */
   const bool prof = NPT_DEBUG(PRESENT_TIMING);
   uint64_t t0 = prof ? npt_profile_now_ns() : 0, t1 = t0, t2 = t0, t3 = t0;

   const bool do_not_wait = (Flags & NPT_DXGI_PRESENT_DO_NOT_WAIT) != 0;
   if (do_not_wait) {
      if (mtx_trylock(&c->present_mutex) != thrd_success)
         return NPT_DXGI_ERROR_WAS_STILL_DRAWING;
   } else {
      mtx_lock(&c->present_mutex);
   }
   int state = atomic_load(&c->gen_state);
   if (state == NPT_GSC_GEN_FAILED) {
      hr = (HRESULT)atomic_load(&c->gen_error_hr);
      mtx_unlock(&c->present_mutex);
      return hr;
   }
   if (state == NPT_GSC_GEN_WINDOW_LOST ||
       (state == NPT_GSC_GEN_CANCELLED && !s->retiring)) {
      mtx_unlock(&c->present_mutex);
      return NPT_DXGI_STATUS_OCCLUDED;
   }
   if (c->color_space_dirty) {
      struct npt_gsc_format_plan plan;
      if (!npt_gsc_get_format_plan(c->desc.Format, c->color_space, &plan)) {
         mtx_unlock(&c->present_mutex);
         return NPT_E_INVALIDARG;
      }
      hr = gsc_teardown_wsi(s, false, do_not_wait);
      if (hr != NPT_S_OK) {
         mtx_unlock(&c->present_mutex);
         return hr;
      }
      uint32_t old_fmt = gsc_resolve_format(c->desc.Format, &s->plan);
      uint32_t new_fmt = gsc_resolve_format(c->desc.Format, &plan);
      if (s->resolved && old_fmt != new_fmt) {
         hr = gsc_recreate_resolved(s, new_fmt);
         if (NPT_FAILED(hr)) {
            c->color_space_dirty = true;
            mtx_unlock(&c->present_mutex);
            return hr;
         }
      }
      struct npt_gsc_format_plan old_plan = s->plan;
      s->plan = plan;
      gsc_ensure_wsi(s);
      if (c->wsi_failed) {
         s->plan = old_plan;
         hr = atomic_load(&c->gen_state) == NPT_GSC_GEN_FAILED
                 ? (HRESULT)atomic_load(&c->gen_error_hr) : NPT_E_FAIL;
         if (atomic_load(&c->gen_state) != NPT_GSC_GEN_FAILED)
            npt_gsc_fail_generation(c, hr);
         mtx_unlock(&c->present_mutex);
         return hr;
      }
      c->color_space_dirty = false;
   }

   if (!c->wsi_initialized && !c->wsi_failed)
      gsc_ensure_wsi(s);
   if (c->wsi_failed) {
      hr = atomic_load(&c->gen_state) == NPT_GSC_GEN_FAILED
              ? (HRESULT)atomic_load(&c->gen_error_hr) : NPT_E_FAIL;
      if (atomic_load(&c->gen_state) != NPT_GSC_GEN_FAILED)
         npt_gsc_fail_generation(c, hr);
      mtx_unlock(&c->present_mutex);
      return hr;
   }

   struct npt_device *dev = c->com->base.device;
   const uint32_t i = s->next_image;
   state = atomic_load(&c->gen_state);
   if (state != NPT_GSC_GEN_ACTIVE) {
      hr = state == NPT_GSC_GEN_FAILED
              ? (HRESULT)atomic_load(&c->gen_error_hr) : NPT_DXGI_STATUS_OCCLUDED;
      mtx_unlock(&c->present_mutex);
      return hr;
   }

   if (do_not_wait && npt_gsc_present_would_block(c, i)) {
      mtx_unlock(&c->present_mutex);
      return NPT_DXGI_ERROR_WAS_STILL_DRAWING;
   }

   if (!do_not_wait)
      npt_gsc_backpressure_wait(c);
   if (prof) t1 = npt_profile_now_ns();
   hr = npt_gsc_reserve_slot(c, i, do_not_wait);
   if (hr != NPT_S_OK) {
      if (hr == NPT_DXGI_STATUS_OCCLUDED)
         npt_gsc_frame_latency_release(c);
      mtx_unlock(&c->present_mutex);
      return hr;
   }
   if (!s->fence) {
      hr = gsc_create_query(s, &s->slot_query[i], &s->slot_query_id[i]);
      if (NPT_FAILED(hr)) {
         npt_gsc_fail_generation(c, hr);
         mtx_unlock(&c->present_mutex);
         return hr;
      }
   }


   struct npt_ring *ring = npt_com_self_ring(s->dc);
   if (s->resolved) {
      npt_async_ID3D11DeviceContext_ResolveSubresource(
         ring, s->dc_id, (ID3D11Resource *)s->resolved, 0,
         (ID3D11Resource *)s->backbuffer, 0,
         (DXGI_FORMAT)gsc_resolve_format(c->desc.Format, &s->plan));
   }
   if (s->convert_list[i]) {
      npt_async_ID3D11DeviceContext_ExecuteCommandList(
         ring, s->dc_id, (ID3D11CommandList *)s->convert_list[i], TRUE);
   } else {
      npt_async_ID3D11DeviceContext_CopyResource(
         ring, s->dc_id,
         (ID3D11Resource *)c->present_tex[i],
         (ID3D11Resource *)gsc_present_source(s));
   }

   int wait_fence_fd = -1;
   uint64_t token = 0;
   uint64_t value = 0;
   const struct npt_id3d11devicecontext_client_vtbl *cv =
      (const void *)((struct npt_com_base *)s->dc)->lpVtbl;
   if (s->dc4 && s->fence) {
      const struct npt_id3d11devicecontext4_client_vtbl *cv4 =
         (const void *)((struct npt_com_base *)s->dc4)->lpVtbl;
      value = ++s->fence_value;
      hr = cv4->Signal(s->dc4, (ID3D11Fence *)s->fence, value);
      if (NPT_FAILED(hr)) {
         npt_gsc_fail_generation(c, hr);
         mtx_unlock(&c->present_mutex);
         return hr;
      }
      cv->Flush(s->dc);
      token = npt_gsc_make_token();
      wait_fence_fd = npt_event_arm_token_fd(dev, token);
      if (wait_fence_fd >= 0) {
         npt_async_ID3D11Fence_SetEventOnCompletion(
            npt_device_method_ring(dev), s->fence_id, value,
            (HANDLE)(uintptr_t)token);
      } else {
         token = 0;
      }
   } else {
      value = ++s->fence_value;
      cv->End(s->dc, (ID3D11Asynchronous *)s->slot_query[i]);
      cv->Flush(s->dc);
   }
   if (prof) t2 = npt_profile_now_ns();
   bool pushed = npt_gsc_wsi_push(c, i, wait_fence_fd, token, value,
                                  wait_fence_fd < 0);
   if (!pushed) {
      npt_gsc_frame_latency_release(c);
      hr = atomic_load(&c->gen_state) == NPT_GSC_GEN_FAILED ?
           (HRESULT)atomic_load(&c->gen_error_hr) : NPT_DXGI_STATUS_OCCLUDED;
      mtx_unlock(&c->present_mutex);
      return hr;
   }

   s->next_image = (i + 1) % NPT_SC_PRESENT_IMAGES;
   c->present_count++;

   npt_profile_present_marker();
   if (prof) {
      t3 = npt_profile_now_ns();
      npt_profile_log_present_timing((unsigned)SyncInterval, t0, t1, t2, t3);
   }
   const bool occluded = c->occluded_streak >= NPT_SC_OCCLUDED_STREAK;
   mtx_unlock(&c->present_mutex);
   return occluded ? NPT_DXGI_STATUS_OCCLUDED : NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_Present(void *self, UINT SyncInterval, UINT Flags)
{
   return gsc_present_common(self, SyncInterval, Flags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_Present1(void *self, UINT SyncInterval, UINT PresentFlags,
             const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
   if (!pPresentParameters)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Dirty/scroll rects are an optimization hint; full-frame present
    * is always correct. */
   return gsc_present_common(self, SyncInterval, PresentFlags);
}

/* ------------------------------------------------------------------ */
/* buffers / desc                                                      */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetBuffer(void *self, UINT Buffer, const IID *riid, void **ppSurface)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!ppSurface)
      return NPT_E_POINTER;
   *ppSurface = NULL;
   if (Buffer >= s->base.desc.BufferCount)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Single-writable-buffer illusion: every index maps to the one
    * backbuffer.  Exact for DISCARD/FLIP_DISCARD (only index 0 is
    * legal); FLIP_SEQUENTIAL readback of previous frames is not
    * supported yet. */
   return npt_gsc_qi_held(s->backbuffer, riid, ppSurface);
}

/* TYPELESS family for a single-sampled chain format; identity for a
 * format without one.  The host back buffer is created typeless so a
 * view typed differently from the chain format -- DXGI permits them
 * (e.g. an sRGB RTV on a UNORM flip chain) -- resolves on the host
 * instead of failing E_INVALIDARG. */
static DXGI_FORMAT
gsc_backbuffer_wire_format(DXGI_FORMAT format)
{
   switch (format) {
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_TYPELESS;
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_TYPELESS;
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8X8_TYPELESS;
   case DXGI_FORMAT_R10G10B10A2_UNORM:
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
   default:
      return format;
   }
}

static HRESULT
gsc_create_backbuffer(struct npt_guest_swapchain *s)
{
   struct npt_guest_swapchain_common *c = &s->base;
   if (!npt_gsc_get_format_plan(c->desc.Format, c->color_space, &s->plan))
      return NPT_E_INVALIDARG;
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = c->desc.Width;
   d.Height = c->desc.Height;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = (DXGI_FORMAT)c->desc.Format;
   d.SampleDesc = c->desc.SampleDesc;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_RENDER_TARGET;
   const bool msaa = d.SampleDesc.Count > 1;
   if ((c->desc.BufferUsage & NPT_DXGI_USAGE_SHADER_INPUT) ||
       s->plan.needs_convert ||
       (!msaa && (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                  d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
                  d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                  d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)))
      d.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
   if (c->desc.BufferUsage & NPT_DXGI_USAGE_UNORDERED_ACCESS)
      d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

   const struct npt_id3d11device_client_vtbl *v =
      (const void *)((struct npt_com_base *)s->device)->lpVtbl;
   ID3D11Texture2D *tex = NULL;
   HRESULT hr;
   const bool flip = c->desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                     c->desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
   if (msaa || !flip) {
      /* Multisampled or BitBlt-model (DISCARD / SEQUENTIAL) chain:
       * previous typed create. */
      hr = v->CreateTexture2D(s->device, &d, NULL, &tex);
   } else {
      /* Single-sampled flip chain: host twin typeless (see above),
       * wrapper keeps the app's typed desc and the public format goes
       * into the device's back-buffer table. */
      hr = npt_d3d11_device_create_backbuffer(
         s->device, &d, gsc_backbuffer_wire_format(d.Format), &tex);
   }
   if (NPT_FAILED(hr) || !tex) {
      npt_log("guest swapchain: backbuffer %ux%u fmt=%u failed hr=0x%x",
              d.Width, d.Height, d.Format, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }
   s->backbuffer = tex;
   s->backbuffer_id = npt_com_self_id(tex);

   if (msaa) {
      d.SampleDesc.Count = 1;
      d.SampleDesc.Quality = 0;
      d.Format = (DXGI_FORMAT)gsc_resolve_format(c->desc.Format, &s->plan);
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      tex = NULL;
      hr = v->CreateTexture2D(s->device, &d, NULL, &tex);
      if (NPT_FAILED(hr) || !tex) {
         npt_log("guest swapchain: resolve target %ux%u fmt=%u failed "
                 "hr=0x%x", d.Width, d.Height, d.Format, hr);
         npt_gsc_release_held(c->com->base.device, &s->backbuffer,
                              &s->backbuffer_id);
         return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
      }
      s->resolved = tex;
      s->resolved_id = npt_com_self_id(tex);
   }
   return NPT_S_OK;
}

static HRESULT
gsc_resize_common(void *self, UINT BufferCount, UINT Width, UINT Height,
                  DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
   struct npt_guest_swapchain *s = gsc(self);
   struct npt_guest_swapchain_common *c = &s->base;
   struct npt_device *dev = c->com->base.device;
   (void)SwapChainFlags;

   mtx_lock(&c->present_mutex);

   struct npt_gsc_format_plan plan;
   uint32_t format = NewFormat == DXGI_FORMAT_UNKNOWN ? c->desc.Format : NewFormat;
   DXGI_COLOR_SPACE_TYPE next_cs =
      npt_gsc_color_space_for_resize(c->desc.Format, format, c->color_space);
   if (!npt_gsc_get_format_plan(format, next_cs, &plan)) {
      mtx_unlock(&c->present_mutex);
      return NPT_E_INVALIDARG;
   }
   DXGI_SWAP_CHAIN_DESC1 old_desc = c->desc;
   DXGI_COLOR_SPACE_TYPE old_cs = c->color_space;
   struct npt_gsc_format_plan old_plan = s->plan;
   void *old_backbuffer = s->backbuffer, *old_resolved = s->resolved;
   uint64_t old_backbuffer_id = s->backbuffer_id, old_resolved_id = s->resolved_id;
   s->backbuffer = s->resolved = NULL;
   s->backbuffer_id = s->resolved_id = 0;
   if (BufferCount)
      c->desc.BufferCount = BufferCount;
   c->desc.Format = format;
   c->color_space = next_cs;
   if (Width && Height) {
      c->desc.Width = Width;
      c->desc.Height = Height;
   } else {
      UINT w = Width, h = Height;
      npt_gsc_query_window_size(c, &w, &h);
      c->desc.Width = w;
      c->desc.Height = h;
   }
   HRESULT hr = gsc_create_backbuffer(s);
   if (!NPT_FAILED(hr))
      hr = gsc_teardown_wsi(s, false, false);
   if (NPT_FAILED(hr)) {
      npt_gsc_release_held(dev, &s->resolved, &s->resolved_id);
      npt_gsc_release_held(dev, &s->backbuffer, &s->backbuffer_id);
      s->backbuffer = old_backbuffer;
      s->backbuffer_id = old_backbuffer_id;
      s->resolved = old_resolved;
      s->resolved_id = old_resolved_id;
      c->desc = old_desc;
      c->color_space = old_cs;
      s->plan = old_plan;
   } else {
      npt_gsc_release_held(dev, &old_resolved, &old_resolved_id);
      npt_gsc_release_held(dev, &old_backbuffer, &old_backbuffer_id);
      c->color_space_dirty = false;
      if (next_cs != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
         c->hdr_meta.valid = 0;
   }
   mtx_unlock(&c->present_mutex);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_ResizeBuffers(void *self, UINT BufferCount, UINT Width, UINT Height,
                  DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
   return gsc_resize_common(self, BufferCount, Width, Height, NewFormat,
                            SwapChainFlags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_ResizeBuffers1(void *self, UINT BufferCount, UINT Width, UINT Height,
                   DXGI_FORMAT Format, UINT SwapChainFlags,
                   const UINT *pCreationNodeMask, IUnknown **ppPresentQueue)
{
   (void)pCreationNodeMask; (void)ppPresentQueue;
   return gsc_resize_common(self, BufferCount, Width, Height, Format,
                            SwapChainFlags);
}

/* -------------------------------------------------------------------------
 * Guest-side fullscreen window management.  The guest-fab swapchain owns the
 * app's OutputWindow, so the window is driven here with plain Win32 calls.
 * Real mode switches are gated on DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
 * without it we do borderless-fullscreen at the current desktop mode (the
 * common path, and the only realizable one in the composited model).
 * ------------------------------------------------------------------------- */
static bool
gsc_desktop_rect(HMONITOR mon, RECT *out)
{
   MONITORINFOEXW mi;
   memset(&mi, 0, sizeof(mi));
   mi.cbSize = sizeof(mi);
   if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
      return false;
   *out = mi.rcMonitor;
   return true;
}

/* Best-effort ChangeDisplaySettings.
 * Returns true if the mode is now (or already was) w x h. */
static bool
gsc_set_display_mode(HMONITOR mon, uint32_t w, uint32_t h, uint32_t refresh)
{
   MONITORINFOEXW mi;
   memset(&mi, 0, sizeof(mi));
   mi.cbSize = sizeof(mi);
   if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
      return false;

   DEVMODEW cur;
   memset(&cur, 0, sizeof(cur));
   cur.dmSize = sizeof(cur);
   if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur) &&
       cur.dmPelsWidth == w && cur.dmPelsHeight == h &&
       (!refresh || cur.dmDisplayFrequency == refresh))
      return true;

   DEVMODEW dm;
   memset(&dm, 0, sizeof(dm));
   dm.dmSize        = sizeof(dm);
   dm.dmFields      = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
   dm.dmPelsWidth   = w;
   dm.dmPelsHeight  = h;
   dm.dmBitsPerPel  = 32;
   if (refresh) {
      dm.dmFields |= DM_DISPLAYFREQUENCY;
      dm.dmDisplayFrequency = refresh;
   }
   LONG st = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
   if (st != DISP_CHANGE_SUCCESSFUL) {
      dm.dmFields &= ~DM_DISPLAYFREQUENCY;
      st = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
   }
   return st == DISP_CHANGE_SUCCESSFUL;
}

static void
gsc_restore_display_mode(HMONITOR mon)
{
   MONITORINFOEXW mi;
   memset(&mi, 0, sizeof(mi));
   mi.cbSize = sizeof(mi);
   if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
      return;
   DEVMODEW dm;
   memset(&dm, 0, sizeof(dm));
   dm.dmSize = sizeof(dm);
   if (EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
      ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
   else
      ChangeDisplaySettingsExW(mi.szDevice, NULL, NULL, 0, NULL);
}

/* Strip decorations and stretch the window over the whole output. */
static bool
gsc_window_enter_fullscreen(struct npt_guest_swapchain *s, HWND hwnd,
                            HMONITOR mon)
{
   GetWindowRect(hwnd, &s->fs_saved_rect);
   LONG style   = GetWindowLongW(hwnd, GWL_STYLE);
   LONG exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
   s->fs_saved_style   = style;
   s->fs_saved_exstyle = exstyle;

   style   &= ~WS_OVERLAPPEDWINDOW;
   exstyle &= ~WS_EX_OVERLAPPEDWINDOW;
   SetWindowLongW(hwnd, GWL_STYLE, style);
   SetWindowLongW(hwnd, GWL_EXSTYLE, exstyle);

   RECT r;
   if (!gsc_desktop_rect(mon, &r))
      return false;
   SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top,
                r.right - r.left, r.bottom - r.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
   return true;
}

/* Restore the pre-fullscreen style/rect.  Only touch the styles if the app
 * hasn't changed them itself, matching native DXGI. */
static void
gsc_window_leave_fullscreen(struct npt_guest_swapchain *s, HWND hwnd)
{
   LONG curStyle = GetWindowLongW(hwnd, GWL_STYLE)   & ~WS_VISIBLE;
   LONG curEx    = GetWindowLongW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST;
   if (curStyle == (s->fs_saved_style   & ~(WS_VISIBLE    | WS_OVERLAPPEDWINDOW)) &&
       curEx    == (s->fs_saved_exstyle & ~(WS_EX_TOPMOST | WS_EX_OVERLAPPEDWINDOW))) {
      SetWindowLongW(hwnd, GWL_STYLE,   s->fs_saved_style);
      SetWindowLongW(hwnd, GWL_EXSTYLE, s->fs_saved_exstyle);
   }
   RECT r = s->fs_saved_rect;
   SetWindowPos(hwnd,
                (s->fs_saved_exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_ResizeTarget(void *self, const DXGI_MODE_DESC *pNewTargetParameters)
{
   if (!pNewTargetParameters)
      return NPT_DXGI_ERROR_INVALID_CALL;
   struct npt_guest_swapchain *s = gsc(self);
   struct npt_guest_swapchain_common *c = &s->base;
   HWND hwnd = c->hwnd;
   if (!hwnd || !IsWindow(hwnd))
      return NPT_DXGI_ERROR_INVALID_CALL;

   uint32_t w = pNewTargetParameters->Width;
   uint32_t h = pNewTargetParameters->Height;

   if (!c->fullscreen) {
      /* Windowed: resize the client area to the requested size. */
      if (w && h)
         npt_gsc_window_resize(hwnd, w, h);
      return NPT_S_OK;
   }

   /* Fullscreen: optionally re-program the mode, then re-cover the output.
    * Read fs_monitor once so a concurrent leave_fullscreen cannot NULL it
    * between uses; the transition itself is not excluded. */
   HMONITOR mon = s->fs_monitor;
   if (!mon)
      return NPT_S_OK;
   if ((c->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) && w && h) {
      uint32_t rr = pNewTargetParameters->RefreshRate.Denominator
         ? pNewTargetParameters->RefreshRate.Numerator /
           pNewTargetParameters->RefreshRate.Denominator : 0;
      /* Record the modeset: this is the same display-mode change
       * gsc_enter_fullscreen makes, and both restore paths are gated on the
       * flag, so without it a mode programmed only here outlives the app. */
      if (gsc_set_display_mode(mon, w, h, rr))
         s->fs_did_modeset = true;
   }
   RECT r;
   if (gsc_desktop_rect(mon, &r))
      MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* fullscreen / outputs (guest-fab wrappers never reach the host)      */
/* ------------------------------------------------------------------ */

/* device -> IDXGIDevice -> adapter -> EnumOutputs(0); the adapter
 * override fabricates the output, so the result is identity-compatible
 * with EnumOutputs(0) callers.  Caller owns the returned ref. */
static IDXGIOutput *
gsc_resolve_first_output(struct npt_guest_swapchain *s)
{
   IDXGIDevice *dxgi_dev = NULL;
   if (NPT_FAILED(npt_gsc_qi_held(s->device, &NPT_IID_IDXGIDevice,
                                  (void **)&dxgi_dev)) || !dxgi_dev)
      return NULL;
   const struct npt_idxgidevice_client_vtbl *dv =
      (const void *)((struct npt_com_base *)dxgi_dev)->lpVtbl;

   IDXGIAdapter *adp = NULL;
   HRESULT hr = dv->GetAdapter(dxgi_dev, &adp);
   if (NPT_FAILED(hr) || !adp) {
      dv->Release(dxgi_dev);
      return NULL;
   }
   const struct npt_idxgiadapter_client_vtbl *av =
      (const void *)((struct npt_com_base *)adp)->lpVtbl;

   IDXGIOutput *out = NULL;
   hr = av->EnumOutputs(adp, 0, &out);
   av->Release(adp);
   dv->Release(dxgi_dev);
   return NPT_SUCCEEDED(hr) ? out : NULL;
}

/* Windowed -> fullscreen.  pTarget (may be NULL) is the app-requested output;
 * the real Win32 monitor is resolved from the window so guest-fab HMONITOR
 * cookies never reach GetMonitorInfoW. */
static HRESULT
gsc_enter_fullscreen(void *self, struct npt_guest_swapchain *s,
                     IDXGIOutput *pTarget)
{
   struct npt_guest_swapchain_common *c = &s->base;

   if (atomic_exchange(&s->mode_change_in_progress, 1))
      return NPT_DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

   HRESULT hr = NPT_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
   HWND hwnd = c->hwnd;
   if (hwnd && IsWindow(hwnd)) {
      bool mode_switch =
         (c->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0;

      /* Hold a ref on the containing output for GetFullscreenState. */
      IDXGIOutput *out = pTarget;
      if (out)
         npt_gsc_output_vtbl(out)->AddRef(out);
      else
         out = gsc_resolve_first_output(s);

      HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

      if (mode_switch && out) {
         /* Snap the requested backbuffer size to a supported (capped) mode,
          * then program it.  Non-fatal: borderless still works on failure. */
         DXGI_MODE_DESC want, got;
         memset(&want, 0, sizeof(want));
         memset(&got, 0, sizeof(got));
         want.Width       = c->desc.Width;
         want.Height      = c->desc.Height;
         want.Format      = c->desc.Format;
         want.RefreshRate = c->fs_desc.RefreshRate;
         if (NPT_SUCCEEDED(npt_gsc_output_vtbl(out)->FindClosestMatchingMode(
                out, &want, &got, NULL))) {
            uint32_t rr = got.RefreshRate.Denominator
               ? got.RefreshRate.Numerator / got.RefreshRate.Denominator : 0;
            if (gsc_set_display_mode(mon, got.Width, got.Height, rr))
               s->fs_did_modeset = true;
         }
      }

      if (gsc_window_enter_fullscreen(s, hwnd, mon)) {
         s->fs_monitor  = mon;
         c->fullscreen  = TRUE;
         npt_gsc_assign_output(c, &c->fullscreen_target, out);
         RECT dr = { 0 };
         gsc_desktop_rect(mon, &dr);
         npt_log("guest swapchain %p entered fullscreen: window -> %ldx%ld (mode_switch=%d, modeset=%d)",
                 self, dr.right - dr.left, dr.bottom - dr.top,
                 mode_switch, s->fs_did_modeset);
         hr = NPT_S_OK;
      } else if (s->fs_did_modeset) {
         gsc_restore_display_mode(mon);
         s->fs_did_modeset = false;
      }
      if (out)
         npt_gsc_output_vtbl(out)->Release(out);
   }
   atomic_store(&s->mode_change_in_progress, 0);
   return hr;
}

/* Fullscreen -> windowed. */
static HRESULT
gsc_leave_fullscreen(void *self, struct npt_guest_swapchain *s)
{
   struct npt_guest_swapchain_common *c = &s->base;

   if (atomic_exchange(&s->mode_change_in_progress, 1))
      return NPT_DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

   if (s->fs_did_modeset) {
      gsc_restore_display_mode(s->fs_monitor);
      s->fs_did_modeset = false;
   }
   c->fullscreen = FALSE;
   HWND hwnd = c->hwnd;
   if (hwnd && IsWindow(hwnd))
      gsc_window_leave_fullscreen(s, hwnd);
   npt_gsc_assign_output(c, &c->fullscreen_target, NULL);
   s->fs_monitor = NULL;
   npt_log("guest swapchain %p left fullscreen", self);
   atomic_store(&s->mode_change_in_progress, 0);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetFullscreenState(void *self, BOOL Fullscreen, IDXGIOutput *pTarget)
{
   struct npt_guest_swapchain *s = gsc(self);

   if (!Fullscreen && pTarget)
      return NPT_DXGI_ERROR_INVALID_CALL;

   /* Dispatch the windowed <-> fullscreen transition; both directions place
    * and style the app's own OutputWindow guest-side. */
   if (!s->base.fullscreen && Fullscreen)
      return gsc_enter_fullscreen(self, s, pTarget);
   if (s->base.fullscreen && !Fullscreen)
      return gsc_leave_fullscreen(self, s);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetContainingOutput(void *self, IDXGIOutput **ppOutput)
{
   struct npt_guest_swapchain *s = gsc(self);
   struct npt_guest_swapchain_common *c = &s->base;
   if (!ppOutput)
      return NPT_E_POINTER;

   *ppOutput = npt_gsc_load_output_addref(c, &c->containing_output);
   if (*ppOutput)
      return NPT_S_OK;

   IDXGIOutput *first = gsc_resolve_first_output(s);
   if (!first)
      return NPT_DXGI_ERROR_NOT_FOUND;
   /* assign_output AddRefs into the cache; the resolver's ref
    * transfers to the caller. */
   npt_gsc_assign_output(c, &c->containing_output, first);
   *ppOutput = first;
   return NPT_S_OK;
}

static UINT NPT_STDMETHODCALLTYPE
gsc_GetCurrentBackBufferIndex(void *self)
{
   (void)self;
   return 0;
}

/* ------------------------------------------------------------------ */
/* vtbl                                                                */
/* ------------------------------------------------------------------ */

static const struct npt_idxgiswapchain4_client_vtbl gsc_vtbl = {
   .QueryInterface = npt_gsc_QueryInterface,
   .AddRef = npt_gsc_AddRef,
   .Release = npt_gsc_Release,
   .SetPrivateData = npt_gsc_SetPrivateData,
   .SetPrivateDataInterface = npt_gsc_SetPrivateDataInterface,
   .GetPrivateData = npt_gsc_GetPrivateData,
   .GetParent = gsc_GetParent,
   .GetDevice = gsc_GetDevice,
   .Present = gsc_Present,
   .GetBuffer = gsc_GetBuffer,
   .SetFullscreenState = gsc_SetFullscreenState,
   .GetFullscreenState = npt_gsc_GetFullscreenState,
   .GetDesc = npt_gsc_GetDesc,
   .ResizeBuffers = gsc_ResizeBuffers,
   .ResizeTarget = gsc_ResizeTarget,
   .GetContainingOutput = gsc_GetContainingOutput,
   .GetFrameStatistics = npt_gsc_GetFrameStatistics,
   .GetLastPresentCount = npt_gsc_GetLastPresentCount,
   .GetDesc1 = npt_gsc_GetDesc1,
   .GetFullscreenDesc = npt_gsc_GetFullscreenDesc,
   .GetHwnd = npt_gsc_GetHwnd,
   .GetCoreWindow = npt_gsc_GetCoreWindow,
   .Present1 = gsc_Present1,
   .IsTemporaryMonoSupported = npt_gsc_IsTemporaryMonoSupported,
   .GetRestrictToOutput = npt_gsc_GetRestrictToOutput,
   .SetBackgroundColor = npt_gsc_SetBackgroundColor,
   .GetBackgroundColor = npt_gsc_GetBackgroundColor,
   .SetRotation = npt_gsc_SetRotation,
   .GetRotation = npt_gsc_GetRotation,
   .SetSourceSize = npt_gsc_SetSourceSize,
   .GetSourceSize = npt_gsc_GetSourceSize,
   .SetMaximumFrameLatency = npt_gsc_SetMaximumFrameLatency,
   .GetMaximumFrameLatency = npt_gsc_GetMaximumFrameLatency,
   .GetFrameLatencyWaitableObject = npt_gsc_GetFrameLatencyWaitableObject,
   .SetMatrixTransform = npt_gsc_SetMatrixTransform,
   .GetMatrixTransform = npt_gsc_GetMatrixTransform,
   .GetCurrentBackBufferIndex = gsc_GetCurrentBackBufferIndex,
   .CheckColorSpaceSupport = npt_gsc_CheckColorSpaceSupport,
   .SetColorSpace1 = npt_gsc_SetColorSpace1,
   .ResizeBuffers1 = gsc_ResizeBuffers1,
   .SetHDRMetaData = npt_gsc_SetHDRMetaData,
};

/* ------------------------------------------------------------------ */
/* creation / destruction                                              */
/* ------------------------------------------------------------------ */

static void
gsc_aux_destroy(void *aux)
{
   struct npt_guest_swapchain *s = aux;
   struct npt_guest_swapchain_common *c = &s->base;
   struct npt_device *dev = c->com ? c->com->base.device : NULL;
   const bool shutting_down = dev && npt_device_is_shutting_down(dev);

   HRESULT drain_hr = gsc_teardown_wsi(s, shutting_down, false);

   if (shutting_down) {
      /* Device teardown frees wrappers via its own drain; retain the aux
       * bookkeeping rather than touch wrappers during shutdown. A
       * CDS_FULLSCREEN display mode is restored by the OS at process
       * exit, so the modeset needs no undo here. */
      return;
   }

   /* A swap chain destroyed while still fullscreen leaves the display in the
    * app's mode; restore the desktop mode like DXGI does on teardown. */
   if (s->fs_did_modeset) {
      gsc_restore_display_mode(s->fs_monitor);
      s->fs_did_modeset = false;
   }

   npt_gsc_release_outputs(c);
   if (NPT_FAILED(drain_hr)) {
      /* Keep the GPU resources and their device alive if completion is unknown. */
      return;
   }
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      npt_gsc_release_held(dev, (void **)&s->slot_query[i],
                           &s->slot_query_id[i]);
   npt_gsc_release_held(dev, (void **)&s->drain_query, &s->drain_query_id);


   npt_gsc_release_held(dev, &s->resolved, &s->resolved_id);
   npt_gsc_release_held(dev, &s->backbuffer, &s->backbuffer_id);
   npt_gsc_release_held(dev, &s->fence, &s->fence_id);
   npt_gsc_release_held(dev, &s->dc4, &s->dc4_id);
   npt_gsc_release_held(dev, &s->dc, &s->dc_id);
   npt_gsc_release_held(dev, &s->device, &s->device_id);
   npt_gsc_release_held(dev, &s->factory, &s->factory_id);

   npt_gsc_fini(c);
   free(s);
}

/* Prefer a D3D11 fence; event queries provide worker-polled completion if
 * the context or device does not support the newer fence interfaces. */
static void
gsc_setup_fence(struct npt_guest_swapchain *s)
{
   HRESULT hr = npt_gsc_qi_held(s->dc, &NPT_IID_ID3D11DeviceContext4,
                                &s->dc4);
   if (NPT_FAILED(hr) || !s->dc4) {
      s->dc4 = NULL;
      npt_log("guest swapchain: no ID3D11DeviceContext4; using event query");
      return;
   }
   s->dc4_id = npt_com_self_id(s->dc4);

   void *device5 = NULL;
   hr = npt_gsc_qi_held(s->device, &NPT_IID_ID3D11Device5, &device5);
   if (NPT_FAILED(hr) || !device5) {
      npt_log("guest swapchain: no ID3D11Device5; using event query");
      return;
   }

   const struct npt_id3d11device5_client_vtbl *v5 =
      (const void *)((struct npt_com_base *)device5)->lpVtbl;
   ID3D11Fence *fence = NULL;
   hr = v5->CreateFence(device5, 0, D3D11_FENCE_FLAG_NONE,
                        &NPT_IID_ID3D11Fence, (void **)&fence);
   v5->Release(device5);
   if (NPT_FAILED(hr) || !fence) {
      npt_log("guest swapchain: CreateFence failed hr=0x%x; using event query",
              hr);
      return;
   }
   s->fence = fence;
   s->fence_id = npt_com_self_id(fence);
}

HRESULT
npt_guest_swapchain_create(void *device_unknown,
                           const DXGI_SWAP_CHAIN_DESC1 *desc1,
                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                           HWND hwnd,
                           void *factory_wrapper,
                           void **out_swapchain)
{
   if (!out_swapchain)
      return NPT_E_POINTER;
   *out_swapchain = NULL;
   if (!device_unknown || !desc1)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (!hwnd) {
      npt_log("guest swapchain: no output window");
      return NPT_DXGI_ERROR_INVALID_CALL;
   }

   /* The caller's pDevice may be any interface of the D3D11 device;
    * resolve the ID3D11Device wrapper we render through. */
   void *device = NULL;
   {
      const struct npt_idxgiswapchain_client_vtbl *v =
         (const void *)((struct npt_com_base *)device_unknown)->lpVtbl;
      HRESULT hr = v->QueryInterface(device_unknown, &NPT_IID_ID3D11Device,
                                     &device);
      if (NPT_FAILED(hr) || !device)
         return NPT_DXGI_ERROR_INVALID_CALL;
   }

   struct npt_device *dev = npt_com_self_device(device);
   if (!dev) {
      npt_com_default_release(device);
      return NPT_E_INVALIDARG;
   }

   const uint64_t guest_id = npt_com_make_guest_id(
      NPT_GUEST_KIND_SWAPCHAIN, npt_com_allocate_next_id());
   struct npt_com_base *com =
      npt_com_get_or_wrap(dev, &NPT_IID_IDXGISwapChain4, guest_id,
                          (struct npt_com_base *)device);
   if (!com) {
      npt_com_default_release(device);
      return NPT_E_OUTOFMEMORY;
   }

   /* The drain and WSI workers run detached and dereference the device and its
    * renderer, so both must outlive this swapchain.  Today that holds only as a
    * side effect of the wrapper graph pinning the device wrapper; make it a
    * reference this object owns, so the device cannot be destroyed underneath a
    * live swapchain no matter how the graph is refactored. */
   npt_device_acquire();
   atomic_fetch_add_explicit(&com->base.device_ref_holds, 1,
                             memory_order_relaxed);

   struct npt_guest_swapchain *s = calloc(1, sizeof(*s));
   if (!s) {
      npt_com_default_release(com);
      npt_com_default_release(device);
      return NPT_E_OUTOFMEMORY;
   }

   /* Not published to any other thread yet; safe to swap the vtbl and
    * attach aux without ordering concerns. */
   com->aux = s;
   com->aux_destroy = gsc_aux_destroy;
   com->lpVtbl = (const void **)&gsc_vtbl;

   npt_gsc_init(&s->base, com, desc1, fs_desc, hwnd, "guest swapchain");
   /* DXGI needs at least one buffer. */
   if (!s->base.desc.BufferCount)
      s->base.desc.BufferCount = 1;

   s->device = device;
   s->device_id = npt_com_self_id(device);

   {
      const struct npt_id3d11device_client_vtbl *dv =
         (const void *)((struct npt_com_base *)device)->lpVtbl;
      ID3D11DeviceContext *dc = NULL;
      dv->GetImmediateContext(device, &dc);
      if (!dc) {
         npt_com_default_release(com);
         return NPT_E_FAIL;
      }
      s->dc = dc;
      s->dc_id = npt_com_self_id(dc);
   }

   if (factory_wrapper) {
      npt_com_default_addref(factory_wrapper);
      s->factory = factory_wrapper;
      s->factory_id = npt_com_self_id(factory_wrapper);
   }

   gsc_setup_fence(s);
   s->base.gpu_poll = gsc_gpu_poll;
   s->base.gpu_poll_ctx = s;

   HRESULT hr = gsc_create_backbuffer(s);
   if (NPT_FAILED(hr)) {
      npt_com_default_release(com);
      return hr;
   }

   npt_log("guest swapchain: created %ux%u fmt=%u buffers=%u hwnd=%p",
           s->base.desc.Width, s->base.desc.Height, s->base.desc.Format,
           s->base.desc.BufferCount, (void *)hwnd);

   *out_swapchain = com;
   return NPT_S_OK;
}

#else /* !__WINE__ */

/* Native Win32: the OS DXGI runtime + Triton own presentation, so
 * COM-level swapchain creation is unimplemented here. */
HRESULT
npt_guest_swapchain_create(void *device_unknown,
                           const DXGI_SWAP_CHAIN_DESC1 *desc1,
                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                           HWND hwnd,
                           void *factory_wrapper,
                           void **out_swapchain)
{
   (void)device_unknown; (void)desc1; (void)fs_desc; (void)hwnd;
   (void)factory_wrapper;
   if (out_swapchain)
      *out_swapchain = NULL;
   return NPT_E_NOTIMPL;
}

#endif /* __WINE__ */

HRESULT
npt_guest_swapchain_create_legacy(void *device_unknown,
                                  const DXGI_SWAP_CHAIN_DESC *desc,
                                  void *factory_wrapper,
                                  void **out_swapchain)
{
   if (!desc)
      return NPT_DXGI_ERROR_INVALID_CALL;

   DXGI_SWAP_CHAIN_DESC1 d1;
   memset(&d1, 0, sizeof(d1));
   d1.Width = desc->BufferDesc.Width;
   d1.Height = desc->BufferDesc.Height;
   d1.Format = desc->BufferDesc.Format;
   d1.SampleDesc = desc->SampleDesc;
   d1.BufferUsage = desc->BufferUsage;
   d1.BufferCount = desc->BufferCount;
   d1.SwapEffect = desc->SwapEffect;
   d1.Flags = desc->Flags;

   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs;
   memset(&fs, 0, sizeof(fs));
   fs.RefreshRate = desc->BufferDesc.RefreshRate;
   fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
   fs.Scaling = desc->BufferDesc.Scaling;
   fs.Windowed = desc->Windowed;

   return npt_guest_swapchain_create(device_unknown, &d1, &fs,
                                     desc->OutputWindow, factory_wrapper,
                                     out_swapchain);
}
