/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Private NGX super-resolution bridge: a small tear-off interface the
 * family QueryInterface path answers locally for opted-in games
 * (BRINEWELL_NGX_SR_ACTIVE=1), so an nvngx.dll in the same process can
 * reach a wrapper's ring and COM id without linking the npt_* sources.
 * Each method encodes a subgroup-8 command and submits it on the
 * owner's ring (list recording point for CREATE/EVAL), waiting for the
 * 16-byte reply.  Nothing but the frozen npt_ngx_bridge_v1.h crosses
 * the DLL boundary -- no ring pointers, TLS, or npt_com internals.
 */

#include "npt_ngx_bridge_v1.h"

#include "ngx/npt_ngx_visibility.h"

#include "npt_com.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"
#include "npt_ring.h"
#include "npt_resource.h"

#include "neptune-protocol/npt_protocol_client_table.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_transport.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct npt_ngx_bridge {
   struct NptNgxBridgeV1 base;
   _Atomic uint32_t ref;
   struct npt_com_base *owner;
   uint32_t kind; /* NPT_SR_KIND_* */
};

typedef uint32_t (NPT_STDMETHODCALLTYPE *npt_ngx_ref_fn)(void *);
typedef HRESULT (NPT_STDMETHODCALLTYPE *npt_ngx_qi_fn)(void *,
                                                     const GUID *, void **);

static uint32_t
npt_ngx_owner_addref(struct npt_com_base *owner)
{
   return ((npt_ngx_ref_fn)owner->lpVtbl[1])(owner);
}

static uint32_t
npt_ngx_owner_release(struct npt_com_base *owner)
{
   return ((npt_ngx_ref_fn)owner->lpVtbl[2])(owner);
}

/* Process-side opt-in: verified once per process against the deployed
 * payload files; see npt_ngx_visibility.h. */
static bool
npt_ngx_bridge_opt_in(void)
{
   return npt_ngx_visible_for_process();
}

static NPT_SR_HRESULT NPT_SR_METHODCALLTYPE
npt_ngx_bridge_qi(NptNgxBridgeV1 *base, const NptSrGuid *riid, void **out)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (!out)
      return NPT_SR_E_POINTER;
   *out = NULL;
   if (!riid)
      return NPT_SR_E_POINTER;
   if (memcmp(riid->bytes, NPT_SR_IID_IUNKNOWN.bytes,
              sizeof(riid->bytes)) == 0) {
      /* Identity belongs to the owner wrapper. */
      npt_ngx_qi_fn qi = (npt_ngx_qi_fn)b->owner->lpVtbl[0];
      return qi(b->owner, &NPT_IID_IUnknown, out);
   }
   if (memcmp(riid->bytes, NPT_SR_IID_NGX_BRIDGE_V1.bytes,
              sizeof(riid->bytes)) == 0) {
      atomic_fetch_add_explicit(&b->ref, 1, memory_order_relaxed);
      *out = b;
      return NPT_SR_S_OK;
   }
   return NPT_SR_E_NOINTERFACE;
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_addref(NptNgxBridgeV1 *base)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   return atomic_fetch_add_explicit(&b->ref, 1, memory_order_relaxed) + 1;
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_release(NptNgxBridgeV1 *base)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   uint32_t cur = atomic_fetch_sub_explicit(&b->ref, 1,
                                            memory_order_acq_rel) - 1;
   if (cur == 0) {
      struct npt_com_base *owner = b->owner;
      free(b);
      npt_ngx_owner_release(owner);
   }
   return cur;
}

static NPT_SR_HRESULT NPT_SR_METHODCALLTYPE
npt_ngx_bridge_get_ref(NptNgxBridgeV1 *base, uint32_t *kind,
                       uintptr_t *context_cookie, uint64_t *object_id)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (!kind || !context_cookie || !object_id)
      return NPT_SR_E_POINTER;
   *kind = b->kind;
   /* Process-local identity of the transport context; never on the wire. */
   *context_cookie = (uintptr_t)b->owner->base.device;
   *object_id = b->owner->base.id;
   return NPT_SR_S_OK;
}

static uint32_t
npt_ngx_submit(struct npt_ngx_bridge *b, const void *cmd, uint32_t cmd_size,
               uint32_t cmd_type, uint64_t *out_value)
{
   struct npt_ring *ring = npt_com_self_ring(b->owner);
   if (!ring)
      return NPT_NGX_RESULT_FAIL;
   return npt_dispatch_ngx(ring, cmd, cmd_size, cmd_type, out_value);
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_caps(NptNgxBridgeV1 *base, uint32_t *sr)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (b->kind != NPT_SR_KIND_ADAPTER)
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   if (!sr)
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   *sr = 0;
   struct npt_cmd_ngx_query_caps cmd;
   npt_ngx_encode_query_caps(&cmd, npt_com_self_id(b->owner));
   uint64_t value;
   uint32_t r = npt_ngx_submit(b, &cmd, sizeof(cmd), cmd.header.cmd_type,
                             &value);
   if (r == NPT_SR_RESULT_SUCCESS)
      *sr = (uint32_t)value;
   return r;
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_init(NptNgxBridgeV1 *base, uint64_t application_id,
                    uint32_t sdk_version)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (b->kind != NPT_SR_KIND_DEVICE &&
       b->kind != NPT_NGX_BRIDGE_KIND_D3D11_DEVICE)
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   struct npt_cmd_ngx_init cmd;
   if (b->kind == NPT_NGX_BRIDGE_KIND_D3D11_DEVICE)
      npt_ngx_encode_d3d11_init(&cmd, npt_com_self_id(b->owner), application_id,
                       sdk_version);
   else
      npt_ngx_encode_init(&cmd, npt_com_self_id(b->owner), application_id,
                       sdk_version);
   return npt_ngx_submit(b, &cmd, sizeof(cmd), cmd.header.cmd_type, NULL);
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_create(NptNgxBridgeV1 *base, uint64_t device_id,
                      const NptSrCreateV1 *p, uint64_t *token)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (b->kind != NPT_SR_KIND_LIST &&
       b->kind != NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT)
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   if (!p || !token || !device_id)
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   *token = 0;
   if (b->kind == NPT_SR_KIND_LIST &&
       !npt_d3d12_list_ngx_recording(b->owner))
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   /* Outside the advertised whitelist is NOT_SUPPORTED; inside it but
    * malformed is INVALID_PARAMETER. */
   if (p->quality < 0 || p->quality > NPT_NGX_QUALITY_MAX ||
       (p->flags & ~NPT_NGX_CREATE_FLAGS_MASK))
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   struct npt_cmd_ngx_create cmd;
   if (b->kind == NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT)
      npt_ngx_encode_d3d11_create(&cmd, npt_com_self_id(b->owner), device_id,
                         p->in_w, p->in_h, p->out_w, p->out_h, p->quality,
                         p->flags);
   else
      npt_ngx_encode_create(&cmd, npt_com_self_id(b->owner), device_id,
                         p->in_w, p->in_h, p->out_w, p->out_h, p->quality,
                         p->flags);
   if (!npt_ngx_valid_create(&cmd))
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   uint64_t value;
   uint32_t r = npt_ngx_submit(b, &cmd, sizeof(cmd), cmd.header.cmd_type,
                             &value);
   if (r == NPT_SR_RESULT_SUCCESS) {
      if (!value)
         return NPT_NGX_RESULT_FAIL;
      *token = value;
   }
   return r;
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_eval(NptNgxBridgeV1 *base, uint64_t token,
                    const NptSrEvalIdsV1 *p)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (b->kind != NPT_SR_KIND_LIST &&
       b->kind != NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT)
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   if (!p || !token)
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   if (b->kind == NPT_SR_KIND_LIST &&
       !npt_d3d12_list_ngx_recording(b->owner))
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   const float pre_exposure = p->pre_exposure == 0.0f ? 1.0f :
                              p->pre_exposure;
   struct npt_cmd_ngx_eval cmd;
   if (b->kind == NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT)
      npt_ngx_encode_d3d11_eval(&cmd, npt_com_self_id(b->owner), token,
                       p->color, p->output, p->depth, p->motion,
                       p->exposure, p->render_w, p->render_h,
                       p->jitter_x, p->jitter_y, p->mv_scale_x,
                       p->mv_scale_y, pre_exposure, p->reset);
   else
      npt_ngx_encode_eval(&cmd, npt_com_self_id(b->owner), token,
                       p->color, p->output, p->depth, p->motion,
                       p->exposure, p->render_w, p->render_h,
                       p->jitter_x, p->jitter_y, p->mv_scale_x,
                       p->mv_scale_y, pre_exposure, p->reset);
   if (!npt_ngx_valid_eval(&cmd))
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   return npt_ngx_submit(b, &cmd, sizeof(cmd), cmd.header.cmd_type, NULL);
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_release_feature(NptNgxBridgeV1 *base, uint64_t token)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (b->kind != NPT_SR_KIND_DEVICE &&
       b->kind != NPT_NGX_BRIDGE_KIND_D3D11_DEVICE)
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   if (!token)
      return NPT_NGX_RESULT_INVALID_PARAMETER;
   struct npt_cmd_ngx_release cmd;
   if (b->kind == NPT_NGX_BRIDGE_KIND_D3D11_DEVICE)
      npt_ngx_encode_d3d11_release(&cmd, npt_com_self_id(b->owner), token);
   else
      npt_ngx_encode_release(&cmd, npt_com_self_id(b->owner), token);
   return npt_ngx_submit(b, &cmd, sizeof(cmd), cmd.header.cmd_type, NULL);
}

static uint32_t NPT_SR_METHODCALLTYPE
npt_ngx_bridge_shutdown(NptNgxBridgeV1 *base)
{
   struct npt_ngx_bridge *b = (struct npt_ngx_bridge *)base;
   if (b->kind != NPT_SR_KIND_DEVICE &&
       b->kind != NPT_NGX_BRIDGE_KIND_D3D11_DEVICE)
      return NPT_NGX_RESULT_NOT_SUPPORTED;
   struct npt_cmd_ngx_shutdown cmd;
   if (b->kind == NPT_NGX_BRIDGE_KIND_D3D11_DEVICE)
      npt_ngx_encode_d3d11_shutdown(&cmd, npt_com_self_id(b->owner));
   else
      npt_ngx_encode_shutdown(&cmd, npt_com_self_id(b->owner));
   return npt_ngx_submit(b, &cmd, sizeof(cmd), cmd.header.cmd_type, NULL);
}

static const NptNgxBridgeV1Vtbl npt_ngx_bridge_vtbl = {
   npt_ngx_bridge_qi,
   npt_ngx_bridge_addref,
   npt_ngx_bridge_release,
   npt_ngx_bridge_get_ref,
   npt_ngx_bridge_caps,
   npt_ngx_bridge_init,
   npt_ngx_bridge_create,
   npt_ngx_bridge_eval,
   npt_ngx_bridge_release_feature,
   npt_ngx_bridge_shutdown,
};

HRESULT
npt_ngx_bridge_query_interface(void *self, const GUID *riid, void **out)
{
   struct npt_com_base *com = self;
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!com || !riid ||
       memcmp(riid, NPT_SR_IID_NGX_BRIDGE_V1.bytes, sizeof(GUID)) != 0)
      return NPT_E_NOINTERFACE;
   const struct npt_com_family *fam = com->family;
   if (!fam || !com->base.id || npt_com_id_is_guest_fab(com->base.id))
      return NPT_E_NOINTERFACE;
   uint32_t kind;
   switch (fam->index) {
   case NPT_IFACE_FAMILY_IDXGIADAPTER:
      kind = NPT_SR_KIND_ADAPTER;
      break;
   case NPT_IFACE_FAMILY_ID3D12DEVICE:
      kind = NPT_SR_KIND_DEVICE;
      break;
   case NPT_IFACE_FAMILY_ID3D12GRAPHICSCOMMANDLIST:
      kind = NPT_SR_KIND_LIST;
      break;
   case NPT_IFACE_FAMILY_ID3D12RESOURCE:
      kind = NPT_SR_KIND_RESOURCE;
      break;
   case NPT_IFACE_FAMILY_ID3D11DEVICE:
      kind = NPT_NGX_BRIDGE_KIND_D3D11_DEVICE;
      break;
   case NPT_IFACE_FAMILY_ID3D11DEVICECONTEXT:
      if (!npt_d3d11_context_is_immediate(self))
         return NPT_E_NOINTERFACE;
      kind = NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT;
      break;
   case NPT_IFACE_FAMILY_ID3D11TEXTURE2D:
      kind = NPT_NGX_BRIDGE_KIND_D3D11_RESOURCE;
      break;
   case NPT_IFACE_FAMILY_ID3D11RESOURCE: {
      void *texture = NULL;
      npt_ngx_qi_fn qi = (npt_ngx_qi_fn)com->lpVtbl[0];
      if (qi(com, &NPT_IID_ID3D11Texture2D, &texture) != NPT_S_OK || !texture)
         return NPT_E_NOINTERFACE;
      npt_ngx_owner_release(texture);
      kind = NPT_NGX_BRIDGE_KIND_D3D11_RESOURCE;
      break;
   }
   default:
      return NPT_E_NOINTERFACE;
   }
   if (!npt_ngx_bridge_opt_in())
      return NPT_E_NOINTERFACE;

   struct npt_ngx_bridge *b = calloc(1, sizeof(*b));
   if (!b)
      return NPT_E_OUTOFMEMORY;
   b->base.lpVtbl = &npt_ngx_bridge_vtbl;
   atomic_init(&b->ref, 1);
   b->owner = com;
   b->kind = kind;
   npt_ngx_owner_addref(com);
   *out = b;
   return NPT_S_OK;
}
