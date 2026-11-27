/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIAdapter{,1..4} overrides.  Each tier's wrapper carries its own
 * vtbl since QI goes through npt_com_get_or_wrap with the wider IID.
 * EnumOutputs is overridden to return guest-fabricated IDXGIOutput
 * wrappers, one per local DRM connector (or one synthetic fallback when
 * no DRM is available).  All other adapter methods pass through to the
 * host -- the host DXGI is itself DXVK, which already supplies the
 * vendor-keyed UMD versions etc.
 */

#include "ngx/npt_ngx_visibility.h"

#include "npt_com.h"
#include "npt_display.h"
#include "npt_env.h"
#include "npt_ngx_bridge_v1.h"
#include "npt_overrides.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_transport.h"

#if defined(__WINE__)
#include "nptunix/npt_unixlib.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TRUE
#define TRUE  ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

struct npt_adapter_aux {
   mtx_t lock;
   bool populated;
   struct npt_output_list outputs;
   /* NGX SR capability cache for the opt-in adapter identity:
    * 0 unknown, 1 positive, -1 negative. */
   int ngx_caps;
};

static void
adp_aux_destroy(void *p)
{
   struct npt_adapter_aux *aux = p;
   if (!aux)
      return;
   mtx_destroy(&aux->lock);
   free(aux);
}

static void
adp_aux_init(struct npt_com_base *com, struct npt_device *dev, uint64_t host_id)
{
   (void)dev; (void)host_id;
   struct npt_adapter_aux *aux = com->aux;
   mtx_init(&aux->lock, mtx_plain);
   com->aux_destroy = adp_aux_destroy;
}

static void
fill_synthetic_output(struct npt_output_info *o)
{
   memset(o, 0, sizeof(*o));
   snprintf(o->name, sizeof(o->name), "Synthetic-1");
   o->current_width  = 1920;
   o->current_height = 1080;
   o->num_modes      = 1;
   o->modes[0].width        = 1920;
   o->modes[0].height       = 1080;
   o->modes[0].refresh_num  = 60;
   o->modes[0].refresh_den  = 1;
   o->modes[0].bpp          = 32;
   o->modes[0].preferred    = true;
}

static void
fetch_output_list(struct npt_output_list *list)
{
   memset(list, 0, sizeof(*list));
#if defined(__WINE__)
   if (npt_unixlib_ensure_init() == 0) {
      struct npt_unix_enumerate_outputs_params p;
      memset(&p, 0, sizeof(p));
      p.expose_all_modes = NPT_DEBUG(EXPOSE_ALL_MODES) ? 1 : 0;
      npt_wine_unix_call(npt_unix_enumerate_outputs, &p);
      *list = p.list;
   }
#endif
   if (list->num_outputs == 0) {
      /* DXVK dmabuf-backend behavior: at least one synthetic output so
       * D3D mode-enumeration paths see something. */
      fill_synthetic_output(&list->outputs[0]);
      list->num_outputs = 1;
   }
}

static bool
adp_ensure_outputs(struct npt_adapter_aux *aux)
{
   mtx_lock(&aux->lock);
   if (!aux->populated) {
      fetch_output_list(&aux->outputs);
      aux->populated = true;
   }
   bool ok = aux->outputs.num_outputs > 0;
   mtx_unlock(&aux->lock);
   return ok;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_EnumOutputs_override(void *self, UINT Output, IDXGIOutput **ppOutput)
{
   if (!ppOutput)
      return NPT_E_POINTER;
   *ppOutput = NULL;

   struct npt_com_base *adp = self;
   struct npt_adapter_aux *aux = adp->aux;
   if (!aux || !adp_ensure_outputs(aux))
      return NPT_DXGI_ERROR_NOT_FOUND;
   if (Output >= aux->outputs.num_outputs)
      return NPT_DXGI_ERROR_NOT_FOUND;

   /* Synthetic id is unique per (parent adapter, connector index). */
   uint64_t out_id = npt_com_make_guest_id(
      NPT_GUEST_KIND_OUTPUT,
      (adp->base.id << 8) ^ Output);

   struct npt_device *dev = npt_com_self_device(self);
   void *wrapper = npt_com_get_or_wrap(dev, &NPT_IID_IDXGIOutput6,
                                       out_id, adp);
   if (!wrapper)
      return NPT_E_OUTOFMEMORY;

   npt_output_aux_populate(wrapper, Output, &aux->outputs.outputs[Output]);
   *ppOutput = (IDXGIOutput *)wrapper;
   return NPT_S_OK;
}

/* NGX SR opt-in adapter identity: when the process-side visibility
 * check passes and a QUERY_CAPS on this adapter is positive, GetDesc*
 * report a compatible NVIDIA identity so games enable NGX input paths.
 * Only Description/VendorId/DeviceId change; the LUID, memory numbers,
 * flags and enumeration order stay the host's.  The Caps result is
 * cached for the adapter's lifetime. */

/* x64 guest only; NPT_NGX_TEST_X64 lets the host unit test exercise the
 * same code path. */
#if defined(__x86_64__) || defined(_M_X64) || defined(NPT_NGX_TEST_X64)
#define NPT_NGX_SPOOF_ARCH_OK 1
#else
#define NPT_NGX_SPOOF_ARCH_OK 0
#endif

static bool
adp_ngx_spoof_active(struct npt_com_base *adp)
{
#if !NPT_NGX_SPOOF_ARCH_OK
   return false;
#else
   if (!npt_ngx_visible_for_process())
      return false;
   struct npt_adapter_aux *aux = adp->aux;
   if (!aux)
      return false;
   mtx_lock(&aux->lock);
   int caps = aux->ngx_caps;
   mtx_unlock(&aux->lock);
   if (caps)
      return caps > 0;

   int positive = 0;
   void *br = NULL;
   const GUID *iid = (const GUID *)&NPT_SR_IID_NGX_BRIDGE_V1;
   if (npt_ngx_bridge_query_interface(adp, iid, &br) == NPT_S_OK && br) {
      NptNgxBridgeV1 *b = br;
      uint32_t sr = 0;
      if (b->lpVtbl->Caps(b, &sr) == NPT_SR_RESULT_SUCCESS &&
           (sr & (NPT_NGX_CAP_SUPER_SAMPLING_D3D12 |
                  NPT_NGX_CAP_SUPER_SAMPLING_D3D11)))
         positive = 1;
      b->lpVtbl->Release(b);
   }
   mtx_lock(&aux->lock);
   aux->ngx_caps = positive ? 1 : -1;
   mtx_unlock(&aux->lock);
   return positive != 0;
#endif
}

static void
adp_spoof_desc(WCHAR description[128], UINT *vendor_id, UINT *device_id)
{
   static const char name[] = "NVIDIA GeForce RTX 3070";
   size_t i;
   for (i = 0; i < sizeof(name) - 1 && i < 127; i++)
      description[i] = (WCHAR)(uint8_t)name[i];
   description[i] = 0;
   *vendor_id = 0x10de;
   *device_id = 0x2484;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_GetDesc_override(void *self, DXGI_ADAPTER_DESC *pDesc)
{
   HRESULT hr = npt_idxgiadapter_default_GetDesc(self, pDesc);
   if (hr == NPT_S_OK && pDesc && adp_ngx_spoof_active(self))
      adp_spoof_desc(pDesc->Description, &pDesc->VendorId,
                     &pDesc->DeviceId);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_GetDesc1_override(void *self, DXGI_ADAPTER_DESC1 *pDesc)
{
   HRESULT hr = npt_idxgiadapter1_default_GetDesc1(self, pDesc);
   if (hr == NPT_S_OK && pDesc && adp_ngx_spoof_active(self))
      adp_spoof_desc(pDesc->Description, &pDesc->VendorId,
                     &pDesc->DeviceId);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_GetDesc2_override(void *self, DXGI_ADAPTER_DESC2 *pDesc)
{
   HRESULT hr = npt_idxgiadapter2_default_GetDesc2(self, pDesc);
   if (hr == NPT_S_OK && pDesc && adp_ngx_spoof_active(self))
      adp_spoof_desc(pDesc->Description, &pDesc->VendorId,
                     &pDesc->DeviceId);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_GetDesc3_override(void *self, DXGI_ADAPTER_DESC3 *pDesc)
{
   HRESULT hr = npt_idxgiadapter4_default_GetDesc3(self, pDesc);
   if (hr == NPT_S_OK && pDesc && adp_ngx_spoof_active(self))
      adp_spoof_desc(pDesc->Description, &pDesc->VendorId,
                     &pDesc->DeviceId);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp3_RegisterHardwareContentProtectionTeardownStatusEvent_override(
   void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
adp3_UnregisterHardwareContentProtectionTeardownStatus_override(
   void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

static const GUID *const adapter_tiers[] = {
   &NPT_IID_IDXGIAdapter,  &NPT_IID_IDXGIAdapter1, &NPT_IID_IDXGIAdapter2,
   &NPT_IID_IDXGIAdapter3, &NPT_IID_IDXGIAdapter4, NULL,
};

void
npt_overrides_dxgi_adapter_init(void)
{
   NPT_REGISTER_OVERRIDE(idxgiadapter, 
      EnumOutputs, adp_EnumOutputs_override);
   NPT_REGISTER_OVERRIDE(idxgiadapter,
      GetDesc, adp_GetDesc_override);
   NPT_REGISTER_OVERRIDE(idxgiadapter,
      GetDesc1, adp_GetDesc1_override);
   NPT_REGISTER_OVERRIDE(idxgiadapter,
      GetDesc2, adp_GetDesc2_override);
   NPT_REGISTER_OVERRIDE(idxgiadapter,
      GetDesc3, adp_GetDesc3_override);

   NPT_REGISTER_OVERRIDE(idxgiadapter3, 
      RegisterHardwareContentProtectionTeardownStatusEvent,
      adp3_RegisterHardwareContentProtectionTeardownStatusEvent_override);
   NPT_REGISTER_OVERRIDE(idxgiadapter3, 
      UnregisterHardwareContentProtectionTeardownStatus,
      adp3_UnregisterHardwareContentProtectionTeardownStatus_override);

   npt_com_register_family(adapter_tiers,
                           sizeof(struct npt_adapter_aux),
                           adp_aux_init);
}
