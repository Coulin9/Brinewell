/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Private in-process bridge between the guest NGX shims (nvngx.dll,
 * nvapi64.dll) and the Neptune COM wrappers that own the host objects.
 * A wrapper hands out this interface from QueryInterface under the
 * private IID only; it is a guest-local tear-off, never a wire object.
 * Interface versioning is the IID itself.
 *
 * The header is self-contained on purpose: nvngx.dll is built without
 * the npt_* sources, so nothing here may name their types.
 */

#ifndef NPT_NGX_BRIDGE_V1_H
#define NPT_NGX_BRIDGE_V1_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#define NPT_SR_METHODCALLTYPE __stdcall
typedef HRESULT NPT_SR_HRESULT;
#else
#define NPT_SR_METHODCALLTYPE
typedef int32_t NPT_SR_HRESULT;
#endif

/* Plain 16-byte GUID so this header needs no COM headers. */
typedef struct NptSrGuid {
   uint8_t bytes[16];
} NptSrGuid;

/* {7ec368f5-2e66-4b9f-9e97-cff40bb92c81} */
static const NptSrGuid NPT_SR_IID_NGX_BRIDGE_V1 = {
   {0xf5, 0x68, 0xc3, 0x7e, 0x66, 0x2e, 0x9f, 0x4b,
    0x9e, 0x97, 0xcf, 0xf4, 0x0b, 0xb9, 0x2c, 0x81}};

/* {00000000-0000-0000-c000-000000000046} */
static const NptSrGuid NPT_SR_IID_IUNKNOWN = {
   {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

#define NPT_SR_S_OK          ((NPT_SR_HRESULT)0x00000000)
#define NPT_SR_E_NOINTERFACE ((NPT_SR_HRESULT)(int32_t)0x80004002)
#define NPT_SR_E_POINTER     ((NPT_SR_HRESULT)(int32_t)0x80004003)

/* Method results mirror the subgroup-8 wire reply codes. */
#define NPT_SR_RESULT_SUCCESS           1u
#define NPT_SR_RESULT_FAIL              0xBAD00000u
#define NPT_SR_RESULT_NOT_SUPPORTED     0xBAD00001u
#define NPT_SR_RESULT_INVALID_PARAMETER 0xBAD00005u

/* Guest-local sentinel: no valid reply arrived (transport lost, short
 * or mismatched reply), so the remote effect of the command is unknown.
 * Never appears on the wire. */
#define NPT_SR_RESULT_TRANSPORT_LOST  0x0bad00ffu

/* GetRef `kind` values. */
enum NptSrBridgeKind {
   NPT_SR_KIND_ADAPTER  = 1,
   NPT_SR_KIND_DEVICE   = 2,
   NPT_SR_KIND_LIST     = 3,
   NPT_SR_KIND_RESOURCE = 4,
   NPT_NGX_BRIDGE_KIND_D3D11_DEVICE = 5,
   NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT = 6,
   NPT_NGX_BRIDGE_KIND_D3D11_RESOURCE = 7,
};

typedef struct NptSrCreateV1 {
   uint32_t in_w;
   uint32_t in_h;
   uint32_t out_w;
   uint32_t out_h;
   int32_t  quality;
   uint32_t flags;
} NptSrCreateV1;

typedef struct NptSrEvalIdsV1 {
   /* Neptune COM object ids of the resources; exposure 0 = absent. */
   uint64_t color;
   uint64_t output;
   uint64_t depth;
   uint64_t motion;
   uint64_t exposure;
   uint32_t render_w;
   uint32_t render_h;
   float    jitter_x;
   float    jitter_y;
   float    mv_scale_x;
   float    mv_scale_y;
   float    pre_exposure; /* 0 is normalized to 1 */
   uint32_t reset;
} NptSrEvalIdsV1;

typedef struct NptNgxBridgeV1 NptNgxBridgeV1;

typedef struct NptNgxBridgeV1Vtbl {
   NPT_SR_HRESULT (NPT_SR_METHODCALLTYPE *QueryInterface)(
      NptNgxBridgeV1 *self, const NptSrGuid *riid, void **out);
   uint32_t (NPT_SR_METHODCALLTYPE *AddRef)(NptNgxBridgeV1 *self);
   uint32_t (NPT_SR_METHODCALLTYPE *Release)(NptNgxBridgeV1 *self);

   /* Always available.  `context_cookie` identifies the owning
    * transport context within this process (same cookie = same
    * context); `object_id` is the underlying object's Neptune COM id. */
   NPT_SR_HRESULT (NPT_SR_METHODCALLTYPE *GetRef)(
      NptNgxBridgeV1 *self, uint32_t *kind, uintptr_t *context_cookie,
      uint64_t *object_id);

   /* Adapter only: NPT_SR_RESULT_* and the capability bit set. */
   uint32_t (NPT_SR_METHODCALLTYPE *Caps)(NptNgxBridgeV1 *self,
                                        uint32_t *sr);

   /* Device only. */
   uint32_t (NPT_SR_METHODCALLTYPE *Init)(NptNgxBridgeV1 *self,
                                        uint64_t application_id,
                                        uint32_t sdk_version);

   /* List only, and only while the list is recording.  device_id is
    * the session device's COM id (from its own bridge GetRef). */
   uint32_t (NPT_SR_METHODCALLTYPE *Create)(NptNgxBridgeV1 *self,
                                          uint64_t device_id,
                                          const NptSrCreateV1 *p,
                                          uint64_t *token);
   uint32_t (NPT_SR_METHODCALLTYPE *Eval)(NptNgxBridgeV1 *self,
                                        uint64_t token,
                                        const NptSrEvalIdsV1 *p);

   /* Device only. */
   uint32_t (NPT_SR_METHODCALLTYPE *ReleaseFeature)(NptNgxBridgeV1 *self,
                                                  uint64_t token);
   uint32_t (NPT_SR_METHODCALLTYPE *Shutdown)(NptNgxBridgeV1 *self);
} NptNgxBridgeV1Vtbl;

struct NptNgxBridgeV1 {
   const NptNgxBridgeV1Vtbl *lpVtbl;
};

#endif /* NPT_NGX_BRIDGE_V1_H */
