/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11Device{,1..5} overrides:
 *  - CreateBuffer/CreateTexture*: pInitialData->pSysMem can't be
 *    auto-marshalled; create with NULL data + ship payload via
 *    RESOURCE_UPDATE.
 *  - Device4::RegisterDeviceRemovedEvent: short-circuit to the
 *    host library's S_OK + cookie 0xdeadbeef stub.
 *  - Device4::UnregisterDeviceRemoved: noop.
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_object.h"
#include "npt_overrides.h"
#include "npt_resource.h"

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_guest_id3d11device.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Sentinel cookie matching the host library's stub. */
#define NPT_DEVICE_REMOVED_COOKIE 0xdeadbeef

static const GUID *const device_tiers[] = {
   &NPT_IID_ID3D11Device,  &NPT_IID_ID3D11Device1, &NPT_IID_ID3D11Device2,
   &NPT_IID_ID3D11Device3, &NPT_IID_ID3D11Device4, &NPT_IID_ID3D11Device5,
   NULL,
};

/* Usage to put on the wire for a Create that ships its initial data
 * via RESOURCE_UPDATE.  IMMUTABLE requires the data at Create time,
 * which the protocol can't carry (unsized pSysMem goes over as NULL),
 * so the host sees DEFAULT instead and the post-Create update becomes
 * a legal UpdateSubresource-style fill.  Only rewrite a desc that is
 * fully valid for IMMUTABLE -- a bind flag is present, no CPU access,
 * no RT/DS/UAV/SO binds, no shared/keyed-mutex/generate-mips misc
 * bits, no multisample, and every subresource carries pSysMem -- so
 * the rewrite never accepts a desc the backend would reject;
 * anything else goes through unchanged for the backend to validate.
 * num_subresources is the caller's full mip*array count (the same
 * value the post-Create upload loop iterates). */
static D3D11_USAGE
dev_wire_usage(D3D11_USAGE usage, UINT cpu_access, UINT bind_flags,
               UINT misc_flags, UINT sample_count,
               const D3D11_SUBRESOURCE_DATA *init,
               uint32_t num_subresources)
{
   if (usage != D3D11_USAGE_IMMUTABLE ||
       !bind_flags ||
       cpu_access != 0 ||
       (bind_flags & (D3D11_BIND_RENDER_TARGET |
                      D3D11_BIND_DEPTH_STENCIL |
                      D3D11_BIND_UNORDERED_ACCESS |
                      D3D11_BIND_STREAM_OUTPUT)) ||
       (misc_flags & (D3D11_RESOURCE_MISC_SHARED |
                      D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                      D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                      D3D11_RESOURCE_MISC_GENERATE_MIPS)) ||
       sample_count > 1 ||
       !init || !num_subresources)
      return usage;
   for (uint32_t i = 0; i < num_subresources; i++) {
      if (!init[i].pSysMem)
         return usage;
   }
   return D3D11_USAGE_DEFAULT;
}

/* A DYNAMIC buffer the host can rename onto guest shmems
 * (npt_d3d11_buffer_set_external_eligible).  Small ones stay on the map
 * ring: what sharing saves is the host copy of the whole buffer at each
 * NO_OVERWRITE Unmap, which only matters for a large one. */
static bool
dev_buffer_external_eligible(const struct npt_device *dev,
                             const D3D11_BUFFER_DESC *desc)
{
   return (dev->renderer->info.caps_flags &
           NPT_CAPSET_CAP_D3D11_EXTERNAL_BUFFERS) &&
          desc && desc->Usage == D3D11_USAGE_DYNAMIC &&
          (desc->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) &&
          !(desc->CPUAccessFlags & D3D11_CPU_ACCESS_READ) &&
          desc->ByteWidth >= NPT_D3D11_EXTERNAL_BUFFER_MIN_BYTES;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateBuffer_override(void *self,
                          const D3D11_BUFFER_DESC *pDesc,
                          const D3D11_SUBRESOURCE_DATA *pInitialData,
                          ID3D11Buffer **ppBuffer)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Wire desc; pDesc stays authoritative for everything below. */
   D3D11_BUFFER_DESC wire;
   const D3D11_BUFFER_DESC *wire_desc = pDesc;
   if (pDesc) {
      wire = *pDesc;
      wire.Usage = dev_wire_usage(pDesc->Usage, pDesc->CPUAccessFlags,
                                  pDesc->BindFlags, pDesc->MiscFlags,
                                  /*sample_count=*/1, pInitialData,
                                  /*num_subresources=*/1);
      wire_desc = &wire;
   }

   /* Sync under multi-ring (cross-ring Create->Use race), async on
    * single-ring (FIFO ordering covers it). */
   ID3D11Buffer *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateBuffer(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateBuffer(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;  /* deferred-fatal: assume S_OK */
   }

   if (NPT_SUCCEEDED(hr) && raw && pInitialData && pInitialData->pSysMem &&
       pDesc && pDesc->ByteWidth > 0) {
      /* MSDN: SysMemPitch / SysMemSlicePitch ignored for buffers. */
      const struct npt_update_split split = {
         .extent = { 0, 0, 0, pDesc->ByteWidth, 1, 1 },
         .rows = 1, .row_texels = 1, .buffer = true,
      };
      npt_resource_update(npt_device_method_ring(dev), /*context_id=*/0,
                          (uint64_t)(uintptr_t)raw, /*subresource=*/0,
                          /*box=*/NULL, /*row_pitch=*/0, /*depth_pitch=*/0,
                          pInitialData->pSysMem, pDesc->ByteWidth,
                          pDesc->ByteWidth, &split);
   }

   if (NPT_SUCCEEDED(hr) && raw) {
      struct npt_d3d11_buffer *b = (struct npt_d3d11_buffer *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Buffer,
                                        (uint64_t)(uintptr_t)raw,
                                        (struct npt_com_base *)self);
      if (b && pDesc)
         npt_d3d11_buffer_set_desc(b, pDesc);
      if (b && ppBuffer && dev_buffer_external_eligible(dev, pDesc))
         npt_d3d11_buffer_set_external_eligible(b);
      if (ppBuffer) {
         *ppBuffer = (ID3D11Buffer *)b;
      } else if (b) {
         /* Validation-only call: drop the caller-owned pub_ref. */
         npt_com_default_release(b);
      }
   } else if (ppBuffer) {
      *ppBuffer = NULL;
   }
   return hr;
}

/* Upload pSysMem via RESOURCE_UPDATE post-Create (the protocol can't
 * serialize unsized void*).  Subresource count = MipLevels * ArraySize.
 *
 * D3D defines SysMemSlicePitch for 3D only and SysMemPitch for 2D and 3D
 * only; outside its resource type each field is free to hold stale
 * garbage, so 1D ignores both and sizes one tight row from the format.
 * The ring reservation charges the full pitch for every row -- the
 * host may consume that much -- but the copy stops at the extent D3D
 * guarantees readable in pSysMem, (rows-1)*pitch + RowSizeInBytes per
 * slice.  Planar formats copy the full footprint, which MSDN documents
 * as rowPitch * subresource_rows with no final-row discount.  The
 * app's pitches already account for the format, so no block-size
 * scaling of the pitch is needed -- only the row count, since one row
 * of memory covers four texel rows under block compression. */
static void
npt_upload_texture_initial_data(struct npt_device *dev,
                                uint64_t tex_id,
                                const D3D11_SUBRESOURCE_DATA *init,
                                uint32_t num_subresources,
                                uint32_t width, uint32_t height, uint32_t depth,
                                uint32_t mip_levels, DXGI_FORMAT format)
{
   if (!init || !num_subresources)
      return;
   for (uint32_t sub = 0; sub < num_subresources; sub++) {
      const D3D11_SUBRESOURCE_DATA *d = &init[sub];
      if (!d->pSysMem)
         continue;
      const uint32_t mip = sub % mip_levels;
      const uint32_t mw = width  > (1u << mip) ? (width  >> mip) : 1u;
      const uint32_t mh = height > (1u << mip) ? (height >> mip) : 1u;
      const uint32_t md = depth  > (1u << mip) ? (depth  >> mip) : 1u;
      const uint32_t rows = npt_dxgi_format_subresource_rows(format, mh);
      const uint32_t last_row = npt_dxgi_format_row_bytes(format, mw);
      const bool tight = last_row &&
         rows == npt_dxgi_format_block_rows(format, mh);
      uint64_t full = 0, copy = 0;
      if (depth > 1u) {
         if (!d->SysMemSlicePitch)
            continue;
         const uint64_t strides =
            (uint64_t)d->SysMemSlicePitch * (md - 1u);
         const uint64_t slice = (uint64_t)d->SysMemPitch * rows;
         full = strides + (slice ? slice : d->SysMemSlicePitch);
         copy = tight && d->SysMemPitch
            ? strides + (uint64_t)d->SysMemPitch * (rows - 1u) + last_row
            : full;
      } else if (height > 1u) {
         if (!d->SysMemPitch)
            continue;
         full = (uint64_t)d->SysMemPitch * rows;
         copy = tight
            ? (uint64_t)d->SysMemPitch * (rows - 1u) + last_row
            : full;
      } else {
         /* A single row: neither pitch carries information D3D
          * promises. */
         full = copy = last_row;
      }
      if (copy > full)
         copy = full;
      if (full == 0 || copy == 0)
         continue;
      const struct npt_update_split split = {
         .extent = { 0, 0, 0, mw, mh, md },
         .rows = rows,
         .row_texels = npt_dxgi_format_row_texels(format),
         .buffer = false,
      };
      const bool planar = rows != npt_dxgi_format_block_rows(format, mh);
      const bool banded = !planar && (depth <= 1u || d->SysMemPitch);
      npt_resource_update(npt_device_method_ring(dev), /*context_id=*/0,
                          tex_id, sub,
                          /*box=*/NULL, d->SysMemPitch, d->SysMemSlicePitch,
                          d->pSysMem, full, copy, banded ? &split : NULL);
   }
}

/* Shared post-Create for CreateTexture{1,2,3}D{,1}; only the desc-fill
 * differs.  out_resource=NULL = MSDN validation-only path. */
static void
finish_create_texture(void *self,
                      const GUID *iid, void *raw,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      const struct npt_d3d11_texture_desc *desc,
                      void **out_resource)
{
   if (!raw) {
      if (out_resource) *out_resource = NULL;
      return;
   }
   struct npt_device *dev = npt_com_self_device(self);
   const uint64_t raw_id  = (uint64_t)(uintptr_t)raw;

   if (pInitialData) {
      const uint32_t mips = npt_d3d11_texture_desc_mip_levels(desc);
      const uint32_t arr  = desc->array_size ? desc->array_size : 1u;
      npt_upload_texture_initial_data(dev, raw_id,
                                      pInitialData, mips * arr,
                                      desc->width, desc->height, desc->depth,
                                      mips, desc->format);
   }

   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)
      npt_com_get_or_wrap_or_release(dev, iid, raw_id,
                                     (struct npt_com_base *)self);
   if (t && npt_d3d11_texture_cast(t))
      npt_d3d11_texture_set_desc(t, desc);
   if (out_resource) {
      *out_resource = t;
   } else if (t) {
      /* Validation-only call: drop the caller-owned pub_ref. */
      npt_com_default_release(t);
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture1D_override(void *self,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture1D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   const struct npt_d3d11_texture_desc d = {
      .width = pDesc ? pDesc->Width : 0,
      .height = 1u,
      .depth = 1u,
      .mip_levels = pDesc ? pDesc->MipLevels : 0,
      .array_size = pDesc ? pDesc->ArraySize : 0,
      .format = pDesc ? pDesc->Format : (DXGI_FORMAT)0,
      .sample_count = 1u,
      .sample_quality = 0u,
      .usage = pDesc ? pDesc->Usage : (D3D11_USAGE)0,
      .bind_flags = pDesc ? pDesc->BindFlags : 0,
      .cpu_access_flags = pDesc ? pDesc->CPUAccessFlags : 0,
      .misc_flags = pDesc ? pDesc->MiscFlags : 0,
      .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
   };
   const uint32_t mips = npt_d3d11_texture_desc_mip_levels(&d);
   const uint32_t subs = mips * (d.array_size ? d.array_size : 1u);

   D3D11_TEXTURE1D_DESC wire;
   const D3D11_TEXTURE1D_DESC *wire_desc = pDesc;
   if (pDesc) {
      wire = *pDesc;
      wire.Usage = dev_wire_usage(pDesc->Usage, pDesc->CPUAccessFlags,
                                  pDesc->BindFlags, pDesc->MiscFlags,
                                  d.sample_count, pInitialData, subs);
      wire_desc = &wire;
   }

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture1D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture1D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture1D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      finish_create_texture(self, &NPT_IID_ID3D11Texture1D, raw,
                            pInitialData, &d, (void **)ppTexture1D);
   } else if (ppTexture1D) {
      *ppTexture1D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture2D_override(void *self,
                              const D3D11_TEXTURE2D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   const struct npt_d3d11_texture_desc d = {
      .width = pDesc ? pDesc->Width : 0,
      .height = pDesc ? pDesc->Height : 0,
      .depth = 1u,
      .mip_levels = pDesc ? pDesc->MipLevels : 0,
      .array_size = pDesc ? pDesc->ArraySize : 0,
      .format = pDesc ? pDesc->Format : (DXGI_FORMAT)0,
      .sample_count = pDesc ? pDesc->SampleDesc.Count : 1u,
      .sample_quality = pDesc ? pDesc->SampleDesc.Quality : 0u,
      .usage = pDesc ? pDesc->Usage : (D3D11_USAGE)0,
      .bind_flags = pDesc ? pDesc->BindFlags : 0,
      .cpu_access_flags = pDesc ? pDesc->CPUAccessFlags : 0,
      .misc_flags = pDesc ? pDesc->MiscFlags : 0,
      .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
   };
   const uint32_t mips = npt_d3d11_texture_desc_mip_levels(&d);
   const uint32_t subs = mips * (d.array_size ? d.array_size : 1u);

   D3D11_TEXTURE2D_DESC wire;
   const D3D11_TEXTURE2D_DESC *wire_desc = pDesc;
   if (pDesc) {
      wire = *pDesc;
      wire.Usage = dev_wire_usage(pDesc->Usage, pDesc->CPUAccessFlags,
                                  pDesc->BindFlags, pDesc->MiscFlags,
                                  d.sample_count, pInitialData, subs);
      wire_desc = &wire;
   }

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture2D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D, raw,
                            pInitialData, &d, (void **)ppTexture2D);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture3D_override(void *self,
                              const D3D11_TEXTURE3D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D **ppTexture3D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   const struct npt_d3d11_texture_desc d = {
      .width = pDesc ? pDesc->Width : 0,
      .height = pDesc ? pDesc->Height : 0,
      .depth = pDesc ? pDesc->Depth : 0,
      .mip_levels = pDesc ? pDesc->MipLevels : 0,
      .array_size = 1u,
      .format = pDesc ? pDesc->Format : (DXGI_FORMAT)0,
      .sample_count = 1u,
      .sample_quality = 0u,
      .usage = pDesc ? pDesc->Usage : (D3D11_USAGE)0,
      .bind_flags = pDesc ? pDesc->BindFlags : 0,
      .cpu_access_flags = pDesc ? pDesc->CPUAccessFlags : 0,
      .misc_flags = pDesc ? pDesc->MiscFlags : 0,
      .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
   };
   const uint32_t subs = npt_d3d11_texture_desc_mip_levels(&d);

   D3D11_TEXTURE3D_DESC wire;
   const D3D11_TEXTURE3D_DESC *wire_desc = pDesc;
   if (pDesc) {
      wire = *pDesc;
      wire.Usage = dev_wire_usage(pDesc->Usage, pDesc->CPUAccessFlags,
                                  pDesc->BindFlags, pDesc->MiscFlags,
                                  d.sample_count, pInitialData, subs);
      wire_desc = &wire;
   }

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture3D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture3D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture3D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      finish_create_texture(self, &NPT_IID_ID3D11Texture3D, raw,
                            pInitialData, &d, (void **)ppTexture3D);
   } else if (ppTexture3D) {
      *ppTexture3D = NULL;
   }
   return hr;
}

/* DESC1 = DESC + TextureLayout; otherwise identical to the non-1 path. */
static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateTexture2D1_override(void *self,
                               const D3D11_TEXTURE2D_DESC1 *pDesc1,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture2D1 **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   const struct npt_d3d11_texture_desc d = {
      .width = pDesc1 ? pDesc1->Width : 0,
      .height = pDesc1 ? pDesc1->Height : 0,
      .depth = 1u,
      .mip_levels = pDesc1 ? pDesc1->MipLevels : 0,
      .array_size = pDesc1 ? pDesc1->ArraySize : 0,
      .format = pDesc1 ? pDesc1->Format : (DXGI_FORMAT)0,
      .sample_count = pDesc1 ? pDesc1->SampleDesc.Count : 1u,
      .sample_quality = pDesc1 ? pDesc1->SampleDesc.Quality : 0u,
      .usage = pDesc1 ? pDesc1->Usage : (D3D11_USAGE)0,
      .bind_flags = pDesc1 ? pDesc1->BindFlags : 0,
      .cpu_access_flags = pDesc1 ? pDesc1->CPUAccessFlags : 0,
      .misc_flags = pDesc1 ? pDesc1->MiscFlags : 0,
      .texture_layout = pDesc1 ? pDesc1->TextureLayout : (D3D11_TEXTURE_LAYOUT)0,
   };
   const uint32_t mips = npt_d3d11_texture_desc_mip_levels(&d);
   const uint32_t subs = mips * (d.array_size ? d.array_size : 1u);

   D3D11_TEXTURE2D_DESC1 wire;
   const D3D11_TEXTURE2D_DESC1 *wire_desc = pDesc1;
   if (pDesc1) {
      wire = *pDesc1;
      wire.Usage = dev_wire_usage(pDesc1->Usage, pDesc1->CPUAccessFlags,
                                  pDesc1->BindFlags, pDesc1->MiscFlags,
                                  d.sample_count, pInitialData, subs);
      wire_desc = &wire;
   }

   ID3D11Texture2D1 *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device3_CreateTexture2D1(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device3_CreateTexture2D1(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc1) {
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D1, raw,
                            pInitialData, &d, (void **)ppTexture2D);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateTexture3D1_override(void *self,
                               const D3D11_TEXTURE3D_DESC1 *pDesc1,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture3D1 **ppTexture3D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   const struct npt_d3d11_texture_desc d = {
      .width = pDesc1 ? pDesc1->Width : 0,
      .height = pDesc1 ? pDesc1->Height : 0,
      .depth = pDesc1 ? pDesc1->Depth : 0,
      .mip_levels = pDesc1 ? pDesc1->MipLevels : 0,
      .array_size = 1u,
      .format = pDesc1 ? pDesc1->Format : (DXGI_FORMAT)0,
      .sample_count = 1u,
      .sample_quality = 0u,
      .usage = pDesc1 ? pDesc1->Usage : (D3D11_USAGE)0,
      .bind_flags = pDesc1 ? pDesc1->BindFlags : 0,
      .cpu_access_flags = pDesc1 ? pDesc1->CPUAccessFlags : 0,
      .misc_flags = pDesc1 ? pDesc1->MiscFlags : 0,
      .texture_layout = pDesc1 ? pDesc1->TextureLayout : (D3D11_TEXTURE_LAYOUT)0,
   };
   const uint32_t subs = npt_d3d11_texture_desc_mip_levels(&d);

   D3D11_TEXTURE3D_DESC1 wire;
   const D3D11_TEXTURE3D_DESC1 *wire_desc = pDesc1;
   if (pDesc1) {
      wire = *pDesc1;
      wire.Usage = dev_wire_usage(pDesc1->Usage, pDesc1->CPUAccessFlags,
                                  pDesc1->BindFlags, pDesc1->MiscFlags,
                                  d.sample_count, pInitialData, subs);
      wire_desc = &wire;
   }

   ID3D11Texture3D1 *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device3_CreateTexture3D1(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device3_CreateTexture3D1(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc1) {
      finish_create_texture(self, &NPT_IID_ID3D11Texture3D1, raw,
                            pInitialData, &d, (void **)ppTexture3D);
   } else if (ppTexture3D) {
      *ppTexture3D = NULL;
   }
   return hr;
}

/* Dedicated create for the guest swapchain's back buffer
 * (npt_swapchain.c).  The host twin may use the typeless family format
 * (wire_format) so views typed differently from the chain format --
 * an sRGB RTV on a UNORM flip chain, which DXGI permits -- resolve on
 * the host, while the wrapper keeps and reports the app's typed desc.
 * The public format is recorded in the device's back-buffer format
 * table under the new texture's host id so the view and desc
 * overrides materialize a NULL / UNKNOWN view format -- or a desc
 * read back from the host -- with the public one, even when the
 * request arrives on a wrapper minted after this one died. */
HRESULT
npt_d3d11_device_create_backbuffer(void *self,
                                   const D3D11_TEXTURE2D_DESC *pDesc,
                                   DXGI_FORMAT wire_format,
                                   ID3D11Texture2D **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   const struct npt_d3d11_texture_desc d = {
      .width = pDesc ? pDesc->Width : 0,
      .height = pDesc ? pDesc->Height : 0,
      .depth = 1u,
      .mip_levels = pDesc ? pDesc->MipLevels : 0,
      .array_size = pDesc ? pDesc->ArraySize : 0,
      .format = pDesc ? pDesc->Format : (DXGI_FORMAT)0,
      .sample_count = pDesc ? pDesc->SampleDesc.Count : 1u,
      .sample_quality = pDesc ? pDesc->SampleDesc.Quality : 0u,
      .usage = pDesc ? pDesc->Usage : (D3D11_USAGE)0,
      .bind_flags = pDesc ? pDesc->BindFlags : 0,
      .cpu_access_flags = pDesc ? pDesc->CPUAccessFlags : 0,
      .misc_flags = pDesc ? pDesc->MiscFlags : 0,
      .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
   };

   D3D11_TEXTURE2D_DESC wire;
   const D3D11_TEXTURE2D_DESC *wire_desc = pDesc;
   if (pDesc) {
      wire = *pDesc;
      wire.Format = wire_format;
      wire_desc = &wire;
   }

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture2D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, wire_desc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D, raw,
                            NULL, &d, (void **)ppTexture2D);
      if (ppTexture2D && *ppTexture2D)
         npt_device_backbuffer_format_add(
            dev, npt_com_self_id(*ppTexture2D), pDesc->Format);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

/* The public typed format of a swapchain back buffer whose host twin
 * is typeless, from the device's id-keyed table; DXGI_FORMAT_UNKNOWN
 * for any other resource. */
static DXGI_FORMAT
dev_backbuffer_view_format(ID3D11Resource *res)
{
   return npt_device_backbuffer_format(npt_com_self_device(res),
                                       npt_com_self_id(res));
}

/* A view references its resource by COM's rule, but a wrapper minted
 * for a view parents only the device (npt_com_get_or_wrap): once the
 * app and the swapchain released theirs, nothing kept a back buffer's
 * wrapper -- and with it the recorded host id that keys the format
 * table -- alive.  ID3D11View::GetResource then registered the object
 * under a fresh id outside the table, and a GetDesc on a wrapper made
 * from it reported the host twin's typeless format.  Memoize the
 * resource on the view wrapper under a private IID: the memo holds a
 * public ref until the view's count drains, so the table key stays
 * registered for every path that can still reach the object.  The IID
 * is arbitrary -- it must only differ from any real IID a host QI or
 * memo method could store. */
static const GUID dev_view_resource_hold_iid = {
   0x4e505421, 0x484f, 0x4c44,
   { 0x88, 0x42, 0x52, 0x45, 0x53, 0x48, 0x4f, 0x4c } };

static HRESULT
dev_finish_backbuffer_view(HRESULT hr, ID3D11Resource *res,
                           DXGI_FORMAT fmt, void **out)
{
   if (fmt != DXGI_FORMAT_UNKNOWN && NPT_SUCCEEDED(hr) && out && *out)
      npt_com_memo_store(*out, 0, &dev_view_resource_hold_iid, res);
   return hr;
}

/* A back buffer whose host twin is typeless rejects a NULL view desc
 * and a desc Format of UNKNOWN: the host has no resource format to
 * default them to.  Send the public typed format instead, with the
 * Texture2D / mip-0 defaults D3D11 produces for this single-mip 2D
 * texture.  Explicit view formats pass through unchanged. */
static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateRenderTargetView_override(void *self, ID3D11Resource *pResource,
                                    const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
                                    ID3D11RenderTargetView **ppRTView)
{
   D3D11_RENDER_TARGET_VIEW_DESC local;
   const DXGI_FORMAT fmt = dev_backbuffer_view_format(pResource);
   if (fmt != DXGI_FORMAT_UNKNOWN) {
      if (!pDesc) {
         memset(&local, 0, sizeof(local));
         local.Format = fmt;
         local.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
         local.Texture2D.MipSlice = 0;
         pDesc = &local;
      } else if (pDesc->Format == DXGI_FORMAT_UNKNOWN) {
         local = *pDesc;
         local.Format = fmt;
         pDesc = &local;
      }
   }
   return dev_finish_backbuffer_view(
      npt_id3d11device_default_CreateRenderTargetView(
         self, pResource, pDesc, ppRTView),
      pResource, fmt, (void **)ppRTView);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateShaderResourceView_override(void *self, ID3D11Resource *pResource,
                                      const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
                                      ID3D11ShaderResourceView **ppSRView)
{
   D3D11_SHADER_RESOURCE_VIEW_DESC local;
   const DXGI_FORMAT fmt = dev_backbuffer_view_format(pResource);
   if (fmt != DXGI_FORMAT_UNKNOWN) {
      if (!pDesc) {
         memset(&local, 0, sizeof(local));
         local.Format = fmt;
         local.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
         local.Texture2D.MostDetailedMip = 0;
         local.Texture2D.MipLevels = 1;
         pDesc = &local;
      } else if (pDesc->Format == DXGI_FORMAT_UNKNOWN) {
         local = *pDesc;
         local.Format = fmt;
         pDesc = &local;
      }
   }
   return dev_finish_backbuffer_view(
      npt_id3d11device_default_CreateShaderResourceView(
         self, pResource, pDesc, ppSRView),
      pResource, fmt, (void **)ppSRView);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateUnorderedAccessView_override(void *self, ID3D11Resource *pResource,
                                       const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
                                       ID3D11UnorderedAccessView **ppUAView)
{
   D3D11_UNORDERED_ACCESS_VIEW_DESC local;
   const DXGI_FORMAT fmt = dev_backbuffer_view_format(pResource);
   if (fmt != DXGI_FORMAT_UNKNOWN) {
      /* An explicit UAV format may only alias the public format:
       * DXGI forbids casting a UAV to another type in the typeless
       * family. */
      if (pDesc && pDesc->Format != DXGI_FORMAT_UNKNOWN &&
          pDesc->Format != fmt) {
         if (ppUAView)
            *ppUAView = NULL;
         return NPT_E_INVALIDARG;
      }
      if (!pDesc) {
         memset(&local, 0, sizeof(local));
         local.Format = fmt;
         local.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
         local.Texture2D.MipSlice = 0;
         pDesc = &local;
      } else if (pDesc->Format == DXGI_FORMAT_UNKNOWN) {
         local = *pDesc;
         local.Format = fmt;
         pDesc = &local;
      }
   }
   return dev_finish_backbuffer_view(
      npt_id3d11device_default_CreateUnorderedAccessView(
         self, pResource, pDesc, ppUAView),
      pResource, fmt, (void **)ppUAView);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateRenderTargetView1_override(void *self, ID3D11Resource *pResource,
                                      const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc1,
                                      ID3D11RenderTargetView1 **ppRTView1)
{
   D3D11_RENDER_TARGET_VIEW_DESC1 local;
   const DXGI_FORMAT fmt = dev_backbuffer_view_format(pResource);
   if (fmt != DXGI_FORMAT_UNKNOWN) {
      if (!pDesc1) {
         memset(&local, 0, sizeof(local));
         local.Format = fmt;
         local.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
         local.Texture2D.MipSlice = 0;
         pDesc1 = &local;
      } else if (pDesc1->Format == DXGI_FORMAT_UNKNOWN) {
         local = *pDesc1;
         local.Format = fmt;
         pDesc1 = &local;
      }
   }
   return dev_finish_backbuffer_view(
      npt_id3d11device3_default_CreateRenderTargetView1(
         self, pResource, pDesc1, ppRTView1),
      pResource, fmt, (void **)ppRTView1);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateShaderResourceView1_override(void *self, ID3D11Resource *pResource,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc1,
                                        ID3D11ShaderResourceView1 **ppSRView1)
{
   D3D11_SHADER_RESOURCE_VIEW_DESC1 local;
   const DXGI_FORMAT fmt = dev_backbuffer_view_format(pResource);
   if (fmt != DXGI_FORMAT_UNKNOWN) {
      if (!pDesc1) {
         memset(&local, 0, sizeof(local));
         local.Format = fmt;
         local.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
         local.Texture2D.MostDetailedMip = 0;
         local.Texture2D.MipLevels = 1;
         pDesc1 = &local;
      } else if (pDesc1->Format == DXGI_FORMAT_UNKNOWN) {
         local = *pDesc1;
         local.Format = fmt;
         pDesc1 = &local;
      }
   }
   return dev_finish_backbuffer_view(
      npt_id3d11device3_default_CreateShaderResourceView1(
         self, pResource, pDesc1, ppSRView1),
      pResource, fmt, (void **)ppSRView1);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateUnorderedAccessView1_override(void *self, ID3D11Resource *pResource,
                                         const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc1,
                                         ID3D11UnorderedAccessView1 **ppUAView1)
{
   D3D11_UNORDERED_ACCESS_VIEW_DESC1 local;
   const DXGI_FORMAT fmt = dev_backbuffer_view_format(pResource);
   if (fmt != DXGI_FORMAT_UNKNOWN) {
      /* See dev_CreateUnorderedAccessView_override. */
      if (pDesc1 && pDesc1->Format != DXGI_FORMAT_UNKNOWN &&
          pDesc1->Format != fmt) {
         if (ppUAView1)
            *ppUAView1 = NULL;
         return NPT_E_INVALIDARG;
      }
      if (!pDesc1) {
         memset(&local, 0, sizeof(local));
         local.Format = fmt;
         local.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
         local.Texture2D.MipSlice = 0;
         pDesc1 = &local;
      } else if (pDesc1->Format == DXGI_FORMAT_UNKNOWN) {
         local = *pDesc1;
         local.Format = fmt;
         pDesc1 = &local;
      }
   }
   return dev_finish_backbuffer_view(
      npt_id3d11device3_default_CreateUnorderedAccessView1(
         self, pResource, pDesc1, ppUAView1),
      pResource, fmt, (void **)ppUAView1);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev4_RegisterDeviceRemovedEvent_override(void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie)
      *pdwCookie = NPT_DEVICE_REMOVED_COOKIE;
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
dev4_UnregisterDeviceRemoved_override(void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

/* Tag each deferred context before the app sees it
 * (npt_d3d11_context_is_immediate). */
static HRESULT
dev_tag_deferred(HRESULT hr, void **ppDeferredContext)
{
   if (NPT_SUCCEEDED(hr) && ppDeferredContext && *ppDeferredContext)
      npt_d3d11_context_set_deferred(*ppDeferredContext);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateDeferredContext_override(void *self, UINT ContextFlags,
                                   ID3D11DeviceContext **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device_default_CreateDeferredContext(self, ContextFlags,
                                                     ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev1_CreateDeferredContext1_override(void *self, UINT ContextFlags,
                                     ID3D11DeviceContext1 **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device1_default_CreateDeferredContext1(self, ContextFlags,
                                                       ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev2_CreateDeferredContext2_override(void *self, UINT ContextFlags,
                                     ID3D11DeviceContext2 **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device2_default_CreateDeferredContext2(self, ContextFlags,
                                                       ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateDeferredContext3_override(void *self, UINT ContextFlags,
                                     ID3D11DeviceContext3 **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device3_default_CreateDeferredContext3(self, ContextFlags,
                                                       ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_GetDeviceRemovedReason_override(void *self)
{
   if (npt_device_lost(npt_com_self_device(self)))
      return DXGI_ERROR_DEVICE_REMOVED;
   return npt_id3d11device_default_GetDeviceRemovedReason(self);
}

void
npt_overrides_d3d11_device_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d11device, GetDeviceRemovedReason,
                         dev_GetDeviceRemovedReason_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateDeferredContext,
                                       dev_CreateDeferredContext_override);
   NPT_REGISTER_OVERRIDE(id3d11device1, CreateDeferredContext1,
                                       dev1_CreateDeferredContext1_override);
   NPT_REGISTER_OVERRIDE(id3d11device2, CreateDeferredContext2,
                                       dev2_CreateDeferredContext2_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateDeferredContext3,
                                       dev3_CreateDeferredContext3_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateBuffer,
                                       dev_CreateBuffer_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateTexture1D,
                                       dev_CreateTexture1D_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateTexture2D,
                                       dev_CreateTexture2D_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateTexture3D,
                                       dev_CreateTexture3D_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateTexture2D1,
                                       dev3_CreateTexture2D1_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateTexture3D1,
                                       dev3_CreateTexture3D1_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateRenderTargetView,
                                       dev_CreateRenderTargetView_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateShaderResourceView,
                                       dev_CreateShaderResourceView_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateUnorderedAccessView,
                                       dev_CreateUnorderedAccessView_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateRenderTargetView1,
                                       dev3_CreateRenderTargetView1_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateShaderResourceView1,
                                       dev3_CreateShaderResourceView1_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateUnorderedAccessView1,
                                       dev3_CreateUnorderedAccessView1_override);
   NPT_REGISTER_OVERRIDE(id3d11device4, RegisterDeviceRemovedEvent,
                                       dev4_RegisterDeviceRemovedEvent_override);
   NPT_REGISTER_OVERRIDE(id3d11device4, UnregisterDeviceRemoved,
                                       dev4_UnregisterDeviceRemoved_override);

   npt_com_register_family(device_tiers, 0, NULL);
}
