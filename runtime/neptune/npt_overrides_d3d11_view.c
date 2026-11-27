/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * RTV / DSV / SRV / UAV GetDesc caching.  A view's desc is immutable,
 * so the first wire GetDesc fills an aux cache and later calls answer
 * guest-side; deferred contexts poll these several times a frame.
 * GetDesc1 stays on the wire.
 */

#include "npt_com.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d11rendertargetview.h"
#include "neptune-protocol/npt_protocol_client_id3d11depthstencilview.h"
#include "neptune-protocol/npt_protocol_client_id3d11shaderresourceview.h"
#include "neptune-protocol/npt_protocol_client_id3d11unorderedaccessview.h"
#include "neptune-protocol/npt_protocol_defs.h"

static const GUID *const rtv_tiers[] = {
   &NPT_IID_ID3D11RenderTargetView, &NPT_IID_ID3D11RenderTargetView1,
   NULL };
static const GUID *const dsv_tiers[] = {
   &NPT_IID_ID3D11DepthStencilView, NULL };
static const GUID *const srv_tiers[] = {
   &NPT_IID_ID3D11ShaderResourceView, &NPT_IID_ID3D11ShaderResourceView1,
   NULL };
static const GUID *const uav_tiers[] = {
   &NPT_IID_ID3D11UnorderedAccessView, &NPT_IID_ID3D11UnorderedAccessView1,
   NULL };

enum {
   NPT_VIEW_DESC_EMPTY = 0,
   NPT_VIEW_DESC_FILLING,
   NPT_VIEW_DESC_READY,
};

struct npt_d3d11_view_aux {
   _Atomic uint32_t state;
   union {
      D3D11_RENDER_TARGET_VIEW_DESC rtv;
      D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
      D3D11_SHADER_RESOURCE_VIEW_DESC srv;
      D3D11_UNORDERED_ACCESS_VIEW_DESC uav;
   } desc;
};

static void
npt_d3d11_view_aux_destroy(void *aux)
{
   free(aux);
}

static void
npt_d3d11_view_aux_init(struct npt_com_base *com, struct npt_device *dev,
                        uint64_t host_id)
{
   com->aux_destroy = npt_d3d11_view_aux_destroy;
   (void)dev; (void)host_id;
}

static struct npt_d3d11_view_aux *
view_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d11_view_aux_destroy);
}

/* Answers *pDesc from the aux cache when ready; otherwise runs the
 * wire GetDesc and caches its reply.  A reply with ViewDimension 0
 * (UNKNOWN) is a failed or missing reply, never a created view's
 * dimension, so it is not latched. */
#define NPT_VIEW_GETDESC_OVERRIDE(name, desc_type, member, default_fn)   \
   static void NPT_STDMETHODCALLTYPE                                     \
   name(void *self, desc_type *pDesc)                                    \
   {                                                                    \
      if (!pDesc) return;                                               \
      struct npt_d3d11_view_aux *aux = view_aux(self);                  \
      if (aux && atomic_load_explicit(&aux->state, memory_order_acquire) \
                 == NPT_VIEW_DESC_READY) {                              \
         memcpy(pDesc, &aux->desc.member, sizeof(*pDesc));              \
         return;                                                        \
      }                                                                 \
      memset(pDesc, 0, sizeof(*pDesc));                                 \
      default_fn(self, pDesc);                                          \
      if (aux && pDesc->ViewDimension != 0) {                           \
         uint32_t expect = NPT_VIEW_DESC_EMPTY;                         \
         if (atomic_compare_exchange_strong(&aux->state, &expect,       \
                                            NPT_VIEW_DESC_FILLING)) {   \
            memcpy(&aux->desc.member, pDesc, sizeof(*pDesc));           \
            atomic_store_explicit(&aux->state, NPT_VIEW_DESC_READY,     \
                                  memory_order_release);                \
         }                                                              \
      }                                                                 \
   }

NPT_VIEW_GETDESC_OVERRIDE(rtv_GetDesc_override, D3D11_RENDER_TARGET_VIEW_DESC,
                          rtv, npt_id3d11rendertargetview_default_GetDesc)
NPT_VIEW_GETDESC_OVERRIDE(dsv_GetDesc_override, D3D11_DEPTH_STENCIL_VIEW_DESC,
                          dsv, npt_id3d11depthstencilview_default_GetDesc)
NPT_VIEW_GETDESC_OVERRIDE(srv_GetDesc_override, D3D11_SHADER_RESOURCE_VIEW_DESC,
                          srv, npt_id3d11shaderresourceview_default_GetDesc)
NPT_VIEW_GETDESC_OVERRIDE(uav_GetDesc_override, D3D11_UNORDERED_ACCESS_VIEW_DESC,
                          uav, npt_id3d11unorderedaccessview_default_GetDesc)

void
npt_overrides_d3d11_view_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d11rendertargetview, GetDesc,
                         rtv_GetDesc_override);
   npt_com_register_family(rtv_tiers, sizeof(struct npt_d3d11_view_aux),
                           npt_d3d11_view_aux_init);

   NPT_REGISTER_OVERRIDE(id3d11depthstencilview, GetDesc,
                         dsv_GetDesc_override);
   npt_com_register_family(dsv_tiers, sizeof(struct npt_d3d11_view_aux),
                           npt_d3d11_view_aux_init);

   NPT_REGISTER_OVERRIDE(id3d11shaderresourceview, GetDesc,
                         srv_GetDesc_override);
   npt_com_register_family(srv_tiers, sizeof(struct npt_d3d11_view_aux),
                           npt_d3d11_view_aux_init);

   NPT_REGISTER_OVERRIDE(id3d11unorderedaccessview, GetDesc,
                         uav_GetDesc_override);
   npt_com_register_family(uav_tiers, sizeof(struct npt_d3d11_view_aux),
                           npt_d3d11_view_aux_init);
}
