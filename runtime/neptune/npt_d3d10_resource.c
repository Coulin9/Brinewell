#ifdef _WIN32
#include <d3d10_1.h>
#include <d3d11_4.h>
#define NPT_D3D10_USE_SDK_HEADERS 1
#endif
#include "npt_d3d10_facets.h"

#ifdef _WIN32
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static struct npt_com_base *owner(void *self)
{
   return ((struct npt_d3d10_facet_base *)self)->owner;
}

static HRESULT get_context(void *self, ID3D11Device **device,
                           ID3D11DeviceContext **context)
{
   ID3D11DeviceChild *child = (ID3D11DeviceChild *)owner(self);
   *device = NULL;
   *context = NULL;
   child->lpVtbl->GetDevice(child, device);
   if (!*device)
      return E_FAIL;
   (*device)->lpVtbl->GetImmediateContext(*device, context);
   if (!*context) {
      (*device)->lpVtbl->Release(*device);
      *device = NULL;
      return E_FAIL;
   }
   return S_OK;
}

static HRESULT map_resource(void *self, UINT subresource, D3D10_MAP type,
                            UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
   ID3D11Device *device;
   ID3D11DeviceContext *context;
   HRESULT hr = get_context(self, &device, &context);
   if (FAILED(hr))
      return hr;
   hr = context->lpVtbl->Map(context, (ID3D11Resource *)owner(self),
                             subresource, (D3D11_MAP)type, flags, mapped);
   context->lpVtbl->Release(context);
   device->lpVtbl->Release(device);
   return hr;
}

static void unmap_resource(void *self, UINT subresource)
{
   ID3D11Device *device;
   ID3D11DeviceContext *context;
   if (FAILED(get_context(self, &device, &context)))
      return;
   context->lpVtbl->Unmap(context, (ID3D11Resource *)owner(self), subresource);
   context->lpVtbl->Release(context);
   device->lpVtbl->Release(device);
}

#define RESOURCE_COMMON(prefix, iface, dimension) \
static HRESULT STDMETHODCALLTYPE prefix##_qi(iface *self, REFIID iid, void **out) \
{ IUnknown *p = (IUnknown *)owner(self); return p->lpVtbl->QueryInterface(p, iid, out); } \
static ULONG STDMETHODCALLTYPE prefix##_addref(iface *self) \
{ IUnknown *p = (IUnknown *)owner(self); return p->lpVtbl->AddRef(p); } \
static ULONG STDMETHODCALLTYPE prefix##_release(iface *self) \
{ IUnknown *p = (IUnknown *)owner(self); return p->lpVtbl->Release(p); } \
static void STDMETHODCALLTYPE prefix##_device(iface *self, ID3D10Device **out) \
{ \
   if (!out) return; \
   *out = NULL; \
   ID3D11DeviceChild *child = (ID3D11DeviceChild *)owner(self); \
   ID3D11Device *device = NULL; \
   child->lpVtbl->GetDevice(child, &device); \
   if (device) { \
      npt_d3d10_wrap_d3d11_object((IUnknown *)device, &IID_ID3D10Device, (void **)out); \
      device->lpVtbl->Release(device); \
   } \
} \
static HRESULT STDMETHODCALLTYPE prefix##_get_data(iface *self, REFGUID guid, UINT *size, void *data) \
{ ID3D11DeviceChild *p = (ID3D11DeviceChild *)owner(self); return p->lpVtbl->GetPrivateData(p, guid, size, data); } \
static HRESULT STDMETHODCALLTYPE prefix##_set_data(iface *self, REFGUID guid, UINT size, const void *data) \
{ ID3D11DeviceChild *p = (ID3D11DeviceChild *)owner(self); return p->lpVtbl->SetPrivateData(p, guid, size, data); } \
static HRESULT STDMETHODCALLTYPE prefix##_set_interface(iface *self, REFGUID guid, const IUnknown *value) \
{ ID3D11DeviceChild *p = (ID3D11DeviceChild *)owner(self); return p->lpVtbl->SetPrivateDataInterface(p, guid, value); } \
static void STDMETHODCALLTYPE prefix##_type(iface *self, D3D10_RESOURCE_DIMENSION *out) \
{ (void)self; if (out) *out = dimension; } \
static void STDMETHODCALLTYPE prefix##_set_priority(iface *self, UINT priority) \
{ ID3D11Resource *p = (ID3D11Resource *)owner(self); p->lpVtbl->SetEvictionPriority(p, priority); } \
static UINT STDMETHODCALLTYPE prefix##_get_priority(iface *self) \
{ ID3D11Resource *p = (ID3D11Resource *)owner(self); return p->lpVtbl->GetEvictionPriority(p); }

RESOURCE_COMMON(buffer, ID3D10Buffer, D3D10_RESOURCE_DIMENSION_BUFFER)
RESOURCE_COMMON(tex1d, ID3D10Texture1D, D3D10_RESOURCE_DIMENSION_TEXTURE1D)
RESOURCE_COMMON(tex2d, ID3D10Texture2D, D3D10_RESOURCE_DIMENSION_TEXTURE2D)
RESOURCE_COMMON(tex3d, ID3D10Texture3D, D3D10_RESOURCE_DIMENSION_TEXTURE3D)

static HRESULT STDMETHODCALLTYPE buffer_map(ID3D10Buffer *self, D3D10_MAP type, UINT flags, void **out)
{
   if (!out) return E_INVALIDARG;
   *out = NULL;
   D3D11_MAPPED_SUBRESOURCE mapped = {0};
   HRESULT hr = map_resource(self, 0, type, flags, &mapped);
   if (SUCCEEDED(hr)) *out = mapped.pData;
   return hr;
}
static void STDMETHODCALLTYPE buffer_unmap(ID3D10Buffer *self) { unmap_resource(self, 0); }
static HRESULT STDMETHODCALLTYPE tex1d_map(ID3D10Texture1D *self, UINT subresource, D3D10_MAP type, UINT flags, void **out)
{
   if (!out) return E_INVALIDARG;
   *out = NULL;
   D3D11_MAPPED_SUBRESOURCE mapped = {0};
   HRESULT hr = map_resource(self, subresource, type, flags, &mapped);
   if (SUCCEEDED(hr)) *out = mapped.pData;
   return hr;
}
static void STDMETHODCALLTYPE tex1d_unmap(ID3D10Texture1D *self, UINT subresource) { unmap_resource(self, subresource); }
static HRESULT STDMETHODCALLTYPE tex2d_map(ID3D10Texture2D *self, UINT subresource, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE2D *out)
{
   if (!out) return E_INVALIDARG;
   memset(out, 0, sizeof(*out));
   D3D11_MAPPED_SUBRESOURCE mapped = {0};
   HRESULT hr = map_resource(self, subresource, type, flags, &mapped);
   if (SUCCEEDED(hr)) { out->pData = mapped.pData; out->RowPitch = mapped.RowPitch; }
   return hr;
}
static void STDMETHODCALLTYPE tex2d_unmap(ID3D10Texture2D *self, UINT subresource) { unmap_resource(self, subresource); }
static HRESULT STDMETHODCALLTYPE tex3d_map(ID3D10Texture3D *self, UINT subresource, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE3D *out)
{
   if (!out) return E_INVALIDARG;
   memset(out, 0, sizeof(*out));
   D3D11_MAPPED_SUBRESOURCE mapped = {0};
   HRESULT hr = map_resource(self, subresource, type, flags, &mapped);
   if (SUCCEEDED(hr)) { out->pData = mapped.pData; out->RowPitch = mapped.RowPitch; out->DepthPitch = mapped.DepthPitch; }
   return hr;
}
static void STDMETHODCALLTYPE tex3d_unmap(ID3D10Texture3D *self, UINT subresource) { unmap_resource(self, subresource); }

static void STDMETHODCALLTYPE buffer_desc(ID3D10Buffer *self, D3D10_BUFFER_DESC *out)
{
   if (!out) return;
   ID3D11Buffer *p = (ID3D11Buffer *)owner(self);
   D3D11_BUFFER_DESC d = {0}; p->lpVtbl->GetDesc(p, &d);
   out->ByteWidth = d.ByteWidth; out->Usage = (D3D10_USAGE)d.Usage;
   out->BindFlags = d.BindFlags; out->CPUAccessFlags = d.CPUAccessFlags;
   out->MiscFlags = npt_d3d11_misc_flags_to_d3d10(d.MiscFlags);
}
static void STDMETHODCALLTYPE tex1d_desc(ID3D10Texture1D *self, D3D10_TEXTURE1D_DESC *out)
{
   if (!out) return;
   ID3D11Texture1D *p = (ID3D11Texture1D *)owner(self);
   D3D11_TEXTURE1D_DESC d = {0}; p->lpVtbl->GetDesc(p, &d);
   out->Width = d.Width; out->MipLevels = d.MipLevels; out->ArraySize = d.ArraySize;
   out->Format = d.Format; out->Usage = (D3D10_USAGE)d.Usage; out->BindFlags = d.BindFlags;
   out->CPUAccessFlags = d.CPUAccessFlags; out->MiscFlags = npt_d3d11_misc_flags_to_d3d10(d.MiscFlags);
}
static void STDMETHODCALLTYPE tex2d_desc(ID3D10Texture2D *self, D3D10_TEXTURE2D_DESC *out)
{
   if (!out) return;
   ID3D11Texture2D *p = (ID3D11Texture2D *)owner(self);
   D3D11_TEXTURE2D_DESC d = {0}; p->lpVtbl->GetDesc(p, &d);
   out->Width = d.Width; out->Height = d.Height; out->MipLevels = d.MipLevels; out->ArraySize = d.ArraySize;
   out->Format = d.Format; out->SampleDesc = d.SampleDesc; out->Usage = (D3D10_USAGE)d.Usage;
   out->BindFlags = d.BindFlags; out->CPUAccessFlags = d.CPUAccessFlags;
   out->MiscFlags = npt_d3d11_misc_flags_to_d3d10(d.MiscFlags);
}
static void STDMETHODCALLTYPE tex3d_desc(ID3D10Texture3D *self, D3D10_TEXTURE3D_DESC *out)
{
   if (!out) return;
   ID3D11Texture3D *p = (ID3D11Texture3D *)owner(self);
   D3D11_TEXTURE3D_DESC d = {0}; p->lpVtbl->GetDesc(p, &d);
   out->Width = d.Width; out->Height = d.Height; out->Depth = d.Depth; out->MipLevels = d.MipLevels;
   out->Format = d.Format; out->Usage = (D3D10_USAGE)d.Usage; out->BindFlags = d.BindFlags;
   out->CPUAccessFlags = d.CPUAccessFlags; out->MiscFlags = npt_d3d11_misc_flags_to_d3d10(d.MiscFlags);
}

#define RESOURCE_VTABLE(prefix, type) \
static const type prefix##_vtbl = { \
   prefix##_qi, prefix##_addref, prefix##_release, prefix##_device, \
   prefix##_get_data, prefix##_set_data, prefix##_set_interface, \
   prefix##_type, prefix##_set_priority, prefix##_get_priority, \
   prefix##_map, prefix##_unmap, prefix##_desc \
}; \
_Static_assert(sizeof(type) == 13 * sizeof(void *), #type " slot count"); \
_Static_assert(offsetof(type, Map) == 10 * sizeof(void *), #type " Map slot"); \
_Static_assert(offsetof(type, GetDesc) == 12 * sizeof(void *), #type " GetDesc slot")
RESOURCE_VTABLE(buffer, ID3D10BufferVtbl);
RESOURCE_VTABLE(tex1d, ID3D10Texture1DVtbl);
RESOURCE_VTABLE(tex2d, ID3D10Texture2DVtbl);
RESOURCE_VTABLE(tex3d, ID3D10Texture3DVtbl);

HRESULT npt_d3d10_resource_qi(struct npt_com_base *primary, const GUID *riid, void **out)
{
   if (!out) return E_INVALIDARG;
   *out = NULL;
   if (!primary || !riid)
      return E_NOINTERFACE;
   const GUID *family = npt_com_primary_base_iid(primary);
   const GUID *concrete;
   const void *vtbl;
   uint32_t kind;
   if (family && IsEqualGUID(family, &IID_ID3D11Buffer)) {
      concrete = &IID_ID3D10Buffer; vtbl = &buffer_vtbl; kind = NPT_D3D10_FACET_BUFFER;
   } else if (family && IsEqualGUID(family, &IID_ID3D11Texture1D)) {
      concrete = &IID_ID3D10Texture1D; vtbl = &tex1d_vtbl; kind = NPT_D3D10_FACET_TEXTURE1D;
   } else if (family && IsEqualGUID(family, &IID_ID3D11Texture2D)) {
      concrete = &IID_ID3D10Texture2D; vtbl = &tex2d_vtbl; kind = NPT_D3D10_FACET_TEXTURE2D;
   } else if (family && IsEqualGUID(family, &IID_ID3D11Texture3D)) {
      concrete = &IID_ID3D10Texture3D; vtbl = &tex3d_vtbl; kind = NPT_D3D10_FACET_TEXTURE3D;
   } else {
      return E_NOINTERFACE;
   }
   if (!IsEqualGUID(riid, concrete) && !IsEqualGUID(riid, &IID_ID3D10Resource) &&
       !IsEqualGUID(riid, &IID_ID3D10DeviceChild))
      return E_NOINTERFACE;
   void *facet = npt_com_load_local_d3d10(primary);
   if (!facet) {
      struct npt_d3d10_facet_base *candidate = calloc(1, sizeof(*candidate));
      if (!candidate) return E_OUTOFMEMORY;
      candidate->lpVtbl = vtbl;
      candidate->host_id = npt_com_primary_host_id(primary);
      candidate->owner = primary;
      candidate->kind = kind;
      facet = npt_d3d10_publish_facet(primary, candidate);
      if (!facet) return E_OUTOFMEMORY;
   }
   IUnknown *p = (IUnknown *)primary;
   p->lpVtbl->AddRef(p);
   *out = facet;
   return S_OK;
}
#else
HRESULT npt_d3d10_resource_qi(struct npt_com_base *primary, const GUID *riid, void **out)
{
   (void)primary; (void)riid; (void)out;
   return E_NOINTERFACE;
}
#endif
