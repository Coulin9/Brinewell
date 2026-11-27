#ifdef _WIN32
#include <d3d10_1.h>
#include <d3d11_4.h>
#define NPT_D3D10_USE_SDK_HEADERS
#include "npt_d3d10_facets.h"
#include "npt_d3d10_device_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

struct npt_d3d10_device_facet {
   struct npt_d3d10_facet_base base;
   UINT text_filter_width;
   UINT text_filter_height;
};

void
npt_d3d10_facet_destroy(struct npt_com_base *primary)
{
   free(npt_com_take_local_d3d10(primary));
}

void *
npt_d3d10_publish_facet(struct npt_com_base *primary,
                        struct npt_d3d10_facet_base *candidate)
{
   void *result = npt_com_publish_local_d3d10(primary, candidate);
   if (result != candidate)
      free(candidate);
   return result;
}

HRESULT
npt_d3d10_wrap_d3d11_object(IUnknown *object, const GUID *riid, void **out)
{
   if (!out)
      return E_POINTER;
   *out = NULL;
   if (!object)
      return S_OK;
   return object->lpVtbl->QueryInterface(object, riid, out);
}

HRESULT
npt_d3d10_unwrap_to_d3d11(IUnknown *object, const GUID *riid, void **out)
{
   if (!out)
      return E_POINTER;
   *out = NULL;
   if (!object)
      return S_OK;
   return object->lpVtbl->QueryInterface(object, riid, out);
}

UINT
npt_d3d10_misc_flags_to_d3d11(UINT flags)
{
   UINT result = 0;
   if (flags & D3D10_RESOURCE_MISC_GENERATE_MIPS)
      result |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
   if (flags & D3D10_RESOURCE_MISC_SHARED)
      result |= D3D11_RESOURCE_MISC_SHARED;
   if (flags & D3D10_RESOURCE_MISC_TEXTURECUBE)
      result |= D3D11_RESOURCE_MISC_TEXTURECUBE;
   if (flags & D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX)
      result |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
   if (flags & D3D10_RESOURCE_MISC_GDI_COMPATIBLE)
      result |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
   return result;
}

UINT
npt_d3d11_misc_flags_to_d3d10(UINT flags)
{
   UINT result = 0;
   if (flags & D3D11_RESOURCE_MISC_GENERATE_MIPS)
      result |= D3D10_RESOURCE_MISC_GENERATE_MIPS;
   if (flags & D3D11_RESOURCE_MISC_SHARED)
      result |= D3D10_RESOURCE_MISC_SHARED;
   if (flags & D3D11_RESOURCE_MISC_TEXTURECUBE)
      result |= D3D10_RESOURCE_MISC_TEXTURECUBE;
   if (flags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
      result |= D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;
   if (flags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE)
      result |= D3D10_RESOURCE_MISC_GDI_COMPATIBLE;
   return result;
}
void
npt_d3d11_blend_desc_to_d3d10(const D3D11_BLEND_DESC *src,
                               D3D10_BLEND_DESC *dst)
{
   const D3D11_RENDER_TARGET_BLEND_DESC *first = &src->RenderTarget[0];
   dst->AlphaToCoverageEnable = src->AlphaToCoverageEnable;
   dst->SrcBlend = (D3D10_BLEND)first->SrcBlend;
   dst->DestBlend = (D3D10_BLEND)first->DestBlend;
   dst->BlendOp = (D3D10_BLEND_OP)first->BlendOp;
   dst->SrcBlendAlpha = (D3D10_BLEND)first->SrcBlendAlpha;
   dst->DestBlendAlpha = (D3D10_BLEND)first->DestBlendAlpha;
   dst->BlendOpAlpha = (D3D10_BLEND_OP)first->BlendOpAlpha;
   for (unsigned i = 0; i < 8; ++i) {
      const D3D11_RENDER_TARGET_BLEND_DESC *rt =
         &src->RenderTarget[src->IndependentBlendEnable ? i : 0];
      dst->BlendEnable[i] = rt->BlendEnable;
      dst->RenderTargetWriteMask[i] = rt->RenderTargetWriteMask;
   }
}

void
npt_d3d11_blend_desc_to_d3d10_1(const D3D11_BLEND_DESC *src,
                                 D3D10_BLEND_DESC1 *dst)
{
   dst->AlphaToCoverageEnable = src->AlphaToCoverageEnable;
   dst->IndependentBlendEnable = src->IndependentBlendEnable;
   for (unsigned i = 0; i < 8; ++i) {
      const D3D11_RENDER_TARGET_BLEND_DESC *s = &src->RenderTarget[i];
      D3D10_RENDER_TARGET_BLEND_DESC1 *d = &dst->RenderTarget[i];
      d->BlendEnable = s->BlendEnable;
      d->SrcBlend = (D3D10_BLEND)s->SrcBlend;
      d->DestBlend = (D3D10_BLEND)s->DestBlend;
      d->BlendOp = (D3D10_BLEND_OP)s->BlendOp;
      d->SrcBlendAlpha = (D3D10_BLEND)s->SrcBlendAlpha;
      d->DestBlendAlpha = (D3D10_BLEND)s->DestBlendAlpha;
      d->BlendOpAlpha = (D3D10_BLEND_OP)s->BlendOpAlpha;
      d->RenderTargetWriteMask = s->RenderTargetWriteMask;
   }
}
_Static_assert(offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Buffer) ==
               offsetof(D3D10_SHADER_RESOURCE_VIEW_DESC, Buffer),
               "SRV union offset mismatch");
_Static_assert(sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC) ==
               sizeof(D3D10_SHADER_RESOURCE_VIEW_DESC),
               "SRV descriptor size mismatch");
_Static_assert(offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Buffer) ==
               offsetof(D3D10_SHADER_RESOURCE_VIEW_DESC1, Buffer),
               "SRV1 union offset mismatch");
_Static_assert(sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC) ==
               sizeof(D3D10_SHADER_RESOURCE_VIEW_DESC1),
               "SRV1 descriptor size mismatch");

void
npt_d3d11_srv_desc_to_d3d10(const D3D11_SHADER_RESOURCE_VIEW_DESC *src,
                             D3D10_SHADER_RESOURCE_VIEW_DESC *dst)
{
   memset(dst, 0, sizeof(*dst));
   dst->Format = src->Format;
   if (src->ViewDimension > D3D11_SRV_DIMENSION_TEXTURECUBE)
      return;
   dst->ViewDimension = (D3D10_SRV_DIMENSION)src->ViewDimension;
   memcpy(&dst->Buffer, &src->Buffer,
          sizeof(*dst) - offsetof(D3D10_SHADER_RESOURCE_VIEW_DESC, Buffer));
}

void
npt_d3d11_srv_desc_to_d3d10_1(const D3D11_SHADER_RESOURCE_VIEW_DESC *src,
                               D3D10_SHADER_RESOURCE_VIEW_DESC1 *dst)
{
   memset(dst, 0, sizeof(*dst));
   dst->Format = src->Format;
   if (src->ViewDimension > D3D11_SRV_DIMENSION_TEXTURECUBEARRAY)
      return;
   dst->ViewDimension = (D3D10_SRV_DIMENSION1)src->ViewDimension;
   memcpy(&dst->Buffer, &src->Buffer,
          sizeof(*dst) - offsetof(D3D10_SHADER_RESOURCE_VIEW_DESC1, Buffer));
}

static ID3D11Device *
facet_device(ID3D10Device1 *iface)
{
   return (ID3D11Device *)((struct npt_d3d10_device_facet *)iface)->base.owner;
}

static HRESULT STDMETHODCALLTYPE
device_QueryInterface(ID3D10Device1 *iface, REFIID riid, void **out)
{
   IUnknown *owner = (IUnknown *)facet_device(iface);
   return owner->lpVtbl->QueryInterface(owner, riid, out);
}

static ULONG STDMETHODCALLTYPE
device_AddRef(ID3D10Device1 *iface)
{
   IUnknown *owner = (IUnknown *)facet_device(iface);
   return owner->lpVtbl->AddRef(owner);
}

static ULONG STDMETHODCALLTYPE
device_Release(ID3D10Device1 *iface)
{
   IUnknown *owner = (IUnknown *)facet_device(iface);
   return owner->lpVtbl->Release(owner);
}

static HRESULT STDMETHODCALLTYPE
device_CheckFormatSupport(ID3D10Device1 *iface, DXGI_FORMAT format, UINT *support)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->CheckFormatSupport(dev, format, support);
}

static HRESULT STDMETHODCALLTYPE
device_CheckMultisampleQualityLevels(ID3D10Device1 *iface, DXGI_FORMAT format,
                                    UINT samples, UINT *levels)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->CheckMultisampleQualityLevels(dev, format, samples, levels);
}

static void STDMETHODCALLTYPE
device_CheckCounterInfo(ID3D10Device1 *iface, D3D10_COUNTER_INFO *info)
{
   ID3D11Device *dev = facet_device(iface);
   D3D11_COUNTER_INFO info11;
   if (!info) return;
   dev->lpVtbl->CheckCounterInfo(dev, &info11);
   info->LastDeviceDependentCounter = (D3D10_COUNTER)info11.LastDeviceDependentCounter;
   info->NumSimultaneousCounters = info11.NumSimultaneousCounters;
   info->NumDetectableParallelUnits = info11.NumDetectableParallelUnits;
}

static HRESULT STDMETHODCALLTYPE
device_CheckCounter(ID3D10Device1 *iface, const D3D10_COUNTER_DESC *desc,
                    D3D10_COUNTER_TYPE *type, UINT *active, char *name,
                    UINT *name_len, char *units, UINT *units_len,
                    char *description, UINT *description_len)
{
   ID3D11Device *dev = facet_device(iface);
   D3D11_COUNTER_DESC desc11;
   D3D11_COUNTER_TYPE type11;
   if (desc) {
      desc11.Counter = (D3D11_COUNTER)desc->Counter;
      desc11.MiscFlags = desc->MiscFlags;
   }
   HRESULT hr = dev->lpVtbl->CheckCounter(dev, desc ? &desc11 : NULL,
                     type ? &type11 : NULL, active, name, name_len, units,
                     units_len, description, description_len);
   if (SUCCEEDED(hr) && type) *type = (D3D10_COUNTER_TYPE)type11;
   return hr;
}

static UINT STDMETHODCALLTYPE
device_GetCreationFlags(ID3D10Device1 *iface)
{
   ID3D11Device *dev = facet_device(iface);
   UINT flags = dev->lpVtbl->GetCreationFlags(dev);
   UINT result = 0;
   if (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) result |= D3D10_CREATE_DEVICE_SINGLETHREADED;
   if (flags & D3D11_CREATE_DEVICE_DEBUG) result |= D3D10_CREATE_DEVICE_DEBUG;
   if (flags & D3D11_CREATE_DEVICE_SWITCH_TO_REF) result |= D3D10_CREATE_DEVICE_SWITCH_TO_REF;
   if (flags & D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS) result |= D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
   if (flags & D3D11_CREATE_DEVICE_BGRA_SUPPORT) result |= D3D10_CREATE_DEVICE_BGRA_SUPPORT;
   if (flags & D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY) result |= D3D10_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY;
   if (flags & D3D11_CREATE_DEVICE_DEBUGGABLE) result |= D3D10_CREATE_DEVICE_DEBUGGABLE;
   return result;
}

static HRESULT STDMETHODCALLTYPE
device_GetDeviceRemovedReason(ID3D10Device1 *iface)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->GetDeviceRemovedReason(dev);
}

static HRESULT STDMETHODCALLTYPE
device_SetExceptionMode(ID3D10Device1 *iface, UINT flags)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->SetExceptionMode(dev, flags);
}

static UINT STDMETHODCALLTYPE
device_GetExceptionMode(ID3D10Device1 *iface)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->GetExceptionMode(dev);
}

static HRESULT STDMETHODCALLTYPE
device_GetPrivateData(ID3D10Device1 *iface, REFGUID guid, UINT *size, void *data)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->GetPrivateData(dev, guid, size, data);
}

static HRESULT STDMETHODCALLTYPE
device_SetPrivateData(ID3D10Device1 *iface, REFGUID guid, UINT size, const void *data)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->SetPrivateData(dev, guid, size, data);
}

static HRESULT STDMETHODCALLTYPE
device_SetPrivateDataInterface(ID3D10Device1 *iface, REFGUID guid, const IUnknown *data)
{
   ID3D11Device *dev = facet_device(iface);
   return dev->lpVtbl->SetPrivateDataInterface(dev, guid, data);
}

static HRESULT STDMETHODCALLTYPE
device_OpenSharedResource(ID3D10Device1 *iface, HANDLE handle, REFIID riid, void **out)
{
   if (out) *out = NULL;
   if (!riid || !out) return E_INVALIDARG;

   const GUID *open_iid = riid;
   int generic_d3d10 = 0;
   if (IsEqualGUID(riid, &IID_ID3D10Buffer)) {
      open_iid = &IID_ID3D11Buffer;
   } else if (IsEqualGUID(riid, &IID_ID3D10Texture1D)) {
      open_iid = &IID_ID3D11Texture1D;
   } else if (IsEqualGUID(riid, &IID_ID3D10Texture2D)) {
      open_iid = &IID_ID3D11Texture2D;
   } else if (IsEqualGUID(riid, &IID_ID3D10Texture3D)) {
      open_iid = &IID_ID3D11Texture3D;
   } else if (IsEqualGUID(riid, &IID_ID3D10Resource) ||
              IsEqualGUID(riid, &IID_ID3D10DeviceChild)) {
      open_iid = &IID_ID3D11Resource;
      generic_d3d10 = 1;
   } else if (IsEqualGUID(riid, &IID_IDXGIResource) ||
              IsEqualGUID(riid, &IID_IUnknown)) {
      open_iid = &IID_ID3D11Resource;
   }

   ID3D11Device *dev = facet_device(iface);
   IUnknown *object = NULL;
   HRESULT hr = dev->lpVtbl->OpenSharedResource(dev, handle, open_iid, (void **)&object);
   if (FAILED(hr) || !object)
      return FAILED(hr) ? hr : E_FAIL;

   if (generic_d3d10) {
      ID3D11Resource *res = (ID3D11Resource *)object;
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      res->lpVtbl->GetType(res, &dim);
      const GUID *concrete_iid = NULL;
      switch (dim) {
      case D3D11_RESOURCE_DIMENSION_BUFFER:
         concrete_iid = &IID_ID3D11Buffer;
         break;
      case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
         concrete_iid = &IID_ID3D11Texture1D;
         break;
      case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
         concrete_iid = &IID_ID3D11Texture2D;
         break;
      case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
         concrete_iid = &IID_ID3D11Texture3D;
         break;
      default:
         object->lpVtbl->Release(object);
         return E_NOINTERFACE;
      }
      IUnknown *concrete = NULL;
      hr = object->lpVtbl->QueryInterface(object, concrete_iid, (void **)&concrete);
      object->lpVtbl->Release(object);
      if (FAILED(hr) || !concrete)
         return FAILED(hr) ? hr : E_NOINTERFACE;
      hr = concrete->lpVtbl->QueryInterface(concrete, riid, out);
      concrete->lpVtbl->Release(concrete);
      return hr;
   }

   hr = object->lpVtbl->QueryInterface(object, riid, out);
   object->lpVtbl->Release(object);
   return hr;
}

static void STDMETHODCALLTYPE
device_SetTextFilterSize(ID3D10Device1 *iface, UINT width, UINT height)
{
   struct npt_d3d10_device_facet *facet = (struct npt_d3d10_device_facet *)iface;
   if (width < 1 || width > 7 || height < 1 || height > 7) return;
   facet->text_filter_width = width;
   facet->text_filter_height = height;
}

static void STDMETHODCALLTYPE
device_GetTextFilterSize(ID3D10Device1 *iface, UINT *width, UINT *height)
{
   struct npt_d3d10_device_facet *facet = (struct npt_d3d10_device_facet *)iface;
   if (width) *width = facet->text_filter_width;
   if (height) *height = facet->text_filter_height;
}

static D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE
device_GetFeatureLevel(ID3D10Device1 *iface)
{
   ID3D11Device *dev = facet_device(iface);
   return (D3D10_FEATURE_LEVEL1)dev->lpVtbl->GetFeatureLevel(dev);
}

static void STDMETHODCALLTYPE
device_VSSetConstantBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers)
{
   npt_d3d10_dev_VSSetConstantBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppConstantBuffers);
}

static void STDMETHODCALLTYPE
device_PSSetShaderResources(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews)
{
   npt_d3d10_dev_PSSetShaderResources((ID3D10Device *)This, StartSlot, NumViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE
device_PSSetShader(
   ID3D10Device1 *This,
   ID3D10PixelShader *pPixelShader)
{
   npt_d3d10_dev_PSSetShader((ID3D10Device *)This, pPixelShader);
}

static void STDMETHODCALLTYPE
device_PSSetSamplers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers)
{
   npt_d3d10_dev_PSSetSamplers((ID3D10Device *)This, StartSlot, NumSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE
device_VSSetShader(
   ID3D10Device1 *This,
   ID3D10VertexShader *pVertexShader)
{
   npt_d3d10_dev_VSSetShader((ID3D10Device *)This, pVertexShader);
}

static void STDMETHODCALLTYPE
device_DrawIndexed(
   ID3D10Device1 *This,
   UINT IndexCount,
   UINT StartIndexLocation,
   INT BaseVertexLocation)
{
   npt_d3d10_dev_DrawIndexed((ID3D10Device *)This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE
device_Draw(
   ID3D10Device1 *This,
   UINT VertexCount,
   UINT StartVertexLocation)
{
   npt_d3d10_dev_Draw((ID3D10Device *)This, VertexCount, StartVertexLocation);
}

static void STDMETHODCALLTYPE
device_PSSetConstantBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers)
{
   npt_d3d10_dev_PSSetConstantBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppConstantBuffers);
}

static void STDMETHODCALLTYPE
device_IASetInputLayout(
   ID3D10Device1 *This,
   ID3D10InputLayout *pInputLayout)
{
   npt_d3d10_dev_IASetInputLayout((ID3D10Device *)This, pInputLayout);
}

static void STDMETHODCALLTYPE
device_IASetVertexBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppVertexBuffers,
   const UINT *pStrides,
   const UINT *pOffsets)
{
   npt_d3d10_dev_IASetVertexBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

static void STDMETHODCALLTYPE
device_IASetIndexBuffer(
   ID3D10Device1 *This,
   ID3D10Buffer *pIndexBuffer,
   DXGI_FORMAT Format,
   UINT Offset)
{
   npt_d3d10_dev_IASetIndexBuffer((ID3D10Device *)This, pIndexBuffer, Format, Offset);
}

static void STDMETHODCALLTYPE
device_DrawIndexedInstanced(
   ID3D10Device1 *This,
   UINT IndexCountPerInstance,
   UINT InstanceCount,
   UINT StartIndexLocation,
   INT BaseVertexLocation,
   UINT StartInstanceLocation)
{
   npt_d3d10_dev_DrawIndexedInstanced((ID3D10Device *)This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE
device_DrawInstanced(
   ID3D10Device1 *This,
   UINT VertexCountPerInstance,
   UINT InstanceCount,
   UINT StartVertexLocation,
   UINT StartInstanceLocation)
{
   npt_d3d10_dev_DrawInstanced((ID3D10Device *)This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE
device_GSSetConstantBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers)
{
   npt_d3d10_dev_GSSetConstantBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppConstantBuffers);
}

static void STDMETHODCALLTYPE
device_GSSetShader(
   ID3D10Device1 *This,
   ID3D10GeometryShader *pShader)
{
   npt_d3d10_dev_GSSetShader((ID3D10Device *)This, pShader);
}

static void STDMETHODCALLTYPE
device_IASetPrimitiveTopology(
   ID3D10Device1 *This,
   D3D10_PRIMITIVE_TOPOLOGY Topology)
{
   npt_d3d10_dev_IASetPrimitiveTopology((ID3D10Device *)This, Topology);
}

static void STDMETHODCALLTYPE
device_VSSetShaderResources(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews)
{
   npt_d3d10_dev_VSSetShaderResources((ID3D10Device *)This, StartSlot, NumViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE
device_VSSetSamplers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers)
{
   npt_d3d10_dev_VSSetSamplers((ID3D10Device *)This, StartSlot, NumSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE
device_SetPredication(
   ID3D10Device1 *This,
   ID3D10Predicate *pPredicate,
   WINBOOL PredicateValue)
{
   npt_d3d10_dev_SetPredication((ID3D10Device *)This, pPredicate, PredicateValue);
}

static void STDMETHODCALLTYPE
device_GSSetShaderResources(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews)
{
   npt_d3d10_dev_GSSetShaderResources((ID3D10Device *)This, StartSlot, NumViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE
device_GSSetSamplers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers)
{
   npt_d3d10_dev_GSSetSamplers((ID3D10Device *)This, StartSlot, NumSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE
device_OMSetRenderTargets(
   ID3D10Device1 *This,
   UINT NumViews,
   ID3D10RenderTargetView *const *ppRenderTargetViews,
   ID3D10DepthStencilView *pDepthStencilView)
{
   npt_d3d10_dev_OMSetRenderTargets((ID3D10Device *)This, NumViews, ppRenderTargetViews, pDepthStencilView);
}

static void STDMETHODCALLTYPE
device_OMSetBlendState(
   ID3D10Device1 *This,
   ID3D10BlendState *pBlendState,
   const FLOAT BlendFactor[4],
   UINT SampleMask)
{
   npt_d3d10_dev_OMSetBlendState((ID3D10Device *)This, pBlendState, BlendFactor, SampleMask);
}

static void STDMETHODCALLTYPE
device_OMSetDepthStencilState(
   ID3D10Device1 *This,
   ID3D10DepthStencilState *pDepthStencilState,
   UINT StencilRef)
{
   npt_d3d10_dev_OMSetDepthStencilState((ID3D10Device *)This, pDepthStencilState, StencilRef);
}

static void STDMETHODCALLTYPE
device_SOSetTargets(
   ID3D10Device1 *This,
   UINT NumBuffers,
   ID3D10Buffer *const *ppSOTargets,
   const UINT *pOffsets)
{
   npt_d3d10_dev_SOSetTargets((ID3D10Device *)This, NumBuffers, ppSOTargets, pOffsets);
}

static void STDMETHODCALLTYPE
device_DrawAuto(
   ID3D10Device1 *This)
{
   npt_d3d10_dev_DrawAuto((ID3D10Device *)This);
}

static void STDMETHODCALLTYPE
device_RSSetState(
   ID3D10Device1 *This,
   ID3D10RasterizerState *pRasterizerState)
{
   npt_d3d10_dev_RSSetState((ID3D10Device *)This, pRasterizerState);
}

static void STDMETHODCALLTYPE
device_RSSetViewports(
   ID3D10Device1 *This,
   UINT NumViewports,
   const D3D10_VIEWPORT *pViewports)
{
   npt_d3d10_dev_RSSetViewports((ID3D10Device *)This, NumViewports, pViewports);
}

static void STDMETHODCALLTYPE
device_RSSetScissorRects(
   ID3D10Device1 *This,
   UINT NumRects,
   const D3D10_RECT *pRects)
{
   npt_d3d10_dev_RSSetScissorRects((ID3D10Device *)This, NumRects, pRects);
}

static void STDMETHODCALLTYPE
device_CopySubresourceRegion(
   ID3D10Device1 *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   UINT DstX,
   UINT DstY,
   UINT DstZ,
   ID3D10Resource *pSrcResource,
   UINT SrcSubresource,
   const D3D10_BOX *pSrcBox)
{
   npt_d3d10_dev_CopySubresourceRegion((ID3D10Device *)This, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
}

static void STDMETHODCALLTYPE
device_CopyResource(
   ID3D10Device1 *This,
   ID3D10Resource *pDstResource,
   ID3D10Resource *pSrcResource)
{
   npt_d3d10_dev_CopyResource((ID3D10Device *)This, pDstResource, pSrcResource);
}

static void STDMETHODCALLTYPE
device_UpdateSubresource(
   ID3D10Device1 *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   const D3D10_BOX *pDstBox,
   const void *pSrcData,
   UINT SrcRowPitch,
   UINT SrcDepthPitch)
{
   npt_d3d10_dev_UpdateSubresource((ID3D10Device *)This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

static void STDMETHODCALLTYPE
device_ClearRenderTargetView(
   ID3D10Device1 *This,
   ID3D10RenderTargetView *pRenderTargetView,
   const FLOAT ColorRGBA[4])
{
   npt_d3d10_dev_ClearRenderTargetView((ID3D10Device *)This, pRenderTargetView, ColorRGBA);
}

static void STDMETHODCALLTYPE
device_ClearDepthStencilView(
   ID3D10Device1 *This,
   ID3D10DepthStencilView *pDepthStencilView,
   UINT ClearFlags,
   FLOAT Depth,
   UINT8 Stencil)
{
   npt_d3d10_dev_ClearDepthStencilView((ID3D10Device *)This, pDepthStencilView, ClearFlags, Depth, Stencil);
}

static void STDMETHODCALLTYPE
device_GenerateMips(
   ID3D10Device1 *This,
   ID3D10ShaderResourceView *pShaderResourceView)
{
   npt_d3d10_dev_GenerateMips((ID3D10Device *)This, pShaderResourceView);
}

static void STDMETHODCALLTYPE
device_ResolveSubresource(
   ID3D10Device1 *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   ID3D10Resource *pSrcResource,
   UINT SrcSubresource,
   DXGI_FORMAT Format)
{
   npt_d3d10_dev_ResolveSubresource((ID3D10Device *)This, pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

static void STDMETHODCALLTYPE
device_VSGetConstantBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers)
{
   npt_d3d10_dev_VSGetConstantBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppConstantBuffers);
}

static void STDMETHODCALLTYPE
device_PSGetShaderResources(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews)
{
   npt_d3d10_dev_PSGetShaderResources((ID3D10Device *)This, StartSlot, NumViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE
device_PSGetShader(
   ID3D10Device1 *This,
   ID3D10PixelShader **ppPixelShader)
{
   npt_d3d10_dev_PSGetShader((ID3D10Device *)This, ppPixelShader);
}

static void STDMETHODCALLTYPE
device_PSGetSamplers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers)
{
   npt_d3d10_dev_PSGetSamplers((ID3D10Device *)This, StartSlot, NumSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE
device_VSGetShader(
   ID3D10Device1 *This,
   ID3D10VertexShader **ppVertexShader)
{
   npt_d3d10_dev_VSGetShader((ID3D10Device *)This, ppVertexShader);
}

static void STDMETHODCALLTYPE
device_PSGetConstantBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers)
{
   npt_d3d10_dev_PSGetConstantBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppConstantBuffers);
}

static void STDMETHODCALLTYPE
device_IAGetInputLayout(
   ID3D10Device1 *This,
   ID3D10InputLayout **ppInputLayout)
{
   npt_d3d10_dev_IAGetInputLayout((ID3D10Device *)This, ppInputLayout);
}

static void STDMETHODCALLTYPE
device_IAGetVertexBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppVertexBuffers,
   UINT *pStrides,
   UINT *pOffsets)
{
   npt_d3d10_dev_IAGetVertexBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

static void STDMETHODCALLTYPE
device_IAGetIndexBuffer(
   ID3D10Device1 *This,
   ID3D10Buffer **pIndexBuffer,
   DXGI_FORMAT *Format,
   UINT *Offset)
{
   npt_d3d10_dev_IAGetIndexBuffer((ID3D10Device *)This, pIndexBuffer, Format, Offset);
}

static void STDMETHODCALLTYPE
device_GSGetConstantBuffers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers)
{
   npt_d3d10_dev_GSGetConstantBuffers((ID3D10Device *)This, StartSlot, NumBuffers, ppConstantBuffers);
}

static void STDMETHODCALLTYPE
device_GSGetShader(
   ID3D10Device1 *This,
   ID3D10GeometryShader **ppGeometryShader)
{
   npt_d3d10_dev_GSGetShader((ID3D10Device *)This, ppGeometryShader);
}

static void STDMETHODCALLTYPE
device_IAGetPrimitiveTopology(
   ID3D10Device1 *This,
   D3D10_PRIMITIVE_TOPOLOGY *pTopology)
{
   npt_d3d10_dev_IAGetPrimitiveTopology((ID3D10Device *)This, pTopology);
}

static void STDMETHODCALLTYPE
device_VSGetShaderResources(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews)
{
   npt_d3d10_dev_VSGetShaderResources((ID3D10Device *)This, StartSlot, NumViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE
device_VSGetSamplers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers)
{
   npt_d3d10_dev_VSGetSamplers((ID3D10Device *)This, StartSlot, NumSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE
device_GetPredication(
   ID3D10Device1 *This,
   ID3D10Predicate **ppPredicate,
   WINBOOL *pPredicateValue)
{
   npt_d3d10_dev_GetPredication((ID3D10Device *)This, ppPredicate, pPredicateValue);
}

static void STDMETHODCALLTYPE
device_GSGetShaderResources(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews)
{
   npt_d3d10_dev_GSGetShaderResources((ID3D10Device *)This, StartSlot, NumViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE
device_GSGetSamplers(
   ID3D10Device1 *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers)
{
   npt_d3d10_dev_GSGetSamplers((ID3D10Device *)This, StartSlot, NumSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE
device_OMGetRenderTargets(
   ID3D10Device1 *This,
   UINT NumViews,
   ID3D10RenderTargetView **ppRenderTargetViews,
   ID3D10DepthStencilView **ppDepthStencilView)
{
   npt_d3d10_dev_OMGetRenderTargets((ID3D10Device *)This, NumViews, ppRenderTargetViews, ppDepthStencilView);
}

static void STDMETHODCALLTYPE
device_OMGetBlendState(
   ID3D10Device1 *This,
   ID3D10BlendState **ppBlendState,
   FLOAT BlendFactor[4],
   UINT *pSampleMask)
{
   npt_d3d10_dev_OMGetBlendState((ID3D10Device *)This, ppBlendState, BlendFactor, pSampleMask);
}

static void STDMETHODCALLTYPE
device_OMGetDepthStencilState(
   ID3D10Device1 *This,
   ID3D10DepthStencilState **ppDepthStencilState,
   UINT *pStencilRef)
{
   npt_d3d10_dev_OMGetDepthStencilState((ID3D10Device *)This, ppDepthStencilState, pStencilRef);
}

static void STDMETHODCALLTYPE
device_SOGetTargets(
   ID3D10Device1 *This,
   UINT NumBuffers,
   ID3D10Buffer **ppSOTargets,
   UINT *pOffsets)
{
   npt_d3d10_dev_SOGetTargets((ID3D10Device *)This, NumBuffers, ppSOTargets, pOffsets);
}

static void STDMETHODCALLTYPE
device_RSGetState(
   ID3D10Device1 *This,
   ID3D10RasterizerState **ppRasterizerState)
{
   npt_d3d10_dev_RSGetState((ID3D10Device *)This, ppRasterizerState);
}

static void STDMETHODCALLTYPE
device_RSGetViewports(
   ID3D10Device1 *This,
   UINT *NumViewports,
   D3D10_VIEWPORT *pViewports)
{
   npt_d3d10_dev_RSGetViewports((ID3D10Device *)This, NumViewports, pViewports);
}

static void STDMETHODCALLTYPE
device_RSGetScissorRects(
   ID3D10Device1 *This,
   UINT *NumRects,
   D3D10_RECT *pRects)
{
   npt_d3d10_dev_RSGetScissorRects((ID3D10Device *)This, NumRects, pRects);
}

static void STDMETHODCALLTYPE
device_ClearState(
   ID3D10Device1 *This)
{
   npt_d3d10_dev_ClearState((ID3D10Device *)This);
}

static void STDMETHODCALLTYPE
device_Flush(
   ID3D10Device1 *This)
{
   npt_d3d10_dev_Flush((ID3D10Device *)This);
}

static HRESULT STDMETHODCALLTYPE
device_CreateBuffer(
   ID3D10Device1 *This,
   const D3D10_BUFFER_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Buffer **ppBuffer)
{
   return npt_d3d10_dev_CreateBuffer((ID3D10Device *)This, pDesc, pInitialData, ppBuffer);
}

static HRESULT STDMETHODCALLTYPE
device_CreateTexture1D(
   ID3D10Device1 *This,
   const D3D10_TEXTURE1D_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Texture1D **ppTexture1D)
{
   return npt_d3d10_dev_CreateTexture1D((ID3D10Device *)This, pDesc, pInitialData, ppTexture1D);
}

static HRESULT STDMETHODCALLTYPE
device_CreateTexture2D(
   ID3D10Device1 *This,
   const D3D10_TEXTURE2D_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Texture2D **ppTexture2D)
{
   return npt_d3d10_dev_CreateTexture2D((ID3D10Device *)This, pDesc, pInitialData, ppTexture2D);
}

static HRESULT STDMETHODCALLTYPE
device_CreateTexture3D(
   ID3D10Device1 *This,
   const D3D10_TEXTURE3D_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Texture3D **ppTexture3D)
{
   return npt_d3d10_dev_CreateTexture3D((ID3D10Device *)This, pDesc, pInitialData, ppTexture3D);
}

static HRESULT STDMETHODCALLTYPE
device_CreateShaderResourceView(
   ID3D10Device1 *This,
   ID3D10Resource *pResource,
   const D3D10_SHADER_RESOURCE_VIEW_DESC *pDesc,
   ID3D10ShaderResourceView **ppSRView)
{
   return npt_d3d10_dev_CreateShaderResourceView((ID3D10Device *)This, pResource, pDesc, ppSRView);
}

static HRESULT STDMETHODCALLTYPE
device_CreateRenderTargetView(
   ID3D10Device1 *This,
   ID3D10Resource *pResource,
   const D3D10_RENDER_TARGET_VIEW_DESC *pDesc,
   ID3D10RenderTargetView **ppRTView)
{
   return npt_d3d10_dev_CreateRenderTargetView((ID3D10Device *)This, pResource, pDesc, ppRTView);
}

static HRESULT STDMETHODCALLTYPE
device_CreateDepthStencilView(
   ID3D10Device1 *This,
   ID3D10Resource *pResource,
   const D3D10_DEPTH_STENCIL_VIEW_DESC *pDesc,
   ID3D10DepthStencilView **ppDepthStencilView)
{
   return npt_d3d10_dev_CreateDepthStencilView((ID3D10Device *)This, pResource, pDesc, ppDepthStencilView);
}

static HRESULT STDMETHODCALLTYPE
device_CreateInputLayout(
   ID3D10Device1 *This,
   const D3D10_INPUT_ELEMENT_DESC *pInputElementDescs,
   UINT NumElements,
   const void *pShaderBytecodeWithInputSignature,
   SIZE_T BytecodeLength,
   ID3D10InputLayout **ppInputLayout)
{
   return npt_d3d10_dev_CreateInputLayout((ID3D10Device *)This, pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
}

static HRESULT STDMETHODCALLTYPE
device_CreateVertexShader(
   ID3D10Device1 *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   ID3D10VertexShader **ppVertexShader)
{
   return npt_d3d10_dev_CreateVertexShader((ID3D10Device *)This, pShaderBytecode, BytecodeLength, ppVertexShader);
}

static HRESULT STDMETHODCALLTYPE
device_CreateGeometryShader(
   ID3D10Device1 *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   ID3D10GeometryShader **ppGeometryShader)
{
   return npt_d3d10_dev_CreateGeometryShader((ID3D10Device *)This, pShaderBytecode, BytecodeLength, ppGeometryShader);
}

static HRESULT STDMETHODCALLTYPE
device_CreateGeometryShaderWithStreamOutput(
   ID3D10Device1 *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   const D3D10_SO_DECLARATION_ENTRY *pSODeclaration,
   UINT NumEntries,
   UINT OutputStreamStride,
   ID3D10GeometryShader **ppGeometryShader)
{
   return npt_d3d10_dev_CreateGeometryShaderWithStreamOutput((ID3D10Device *)This, pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, OutputStreamStride, ppGeometryShader);
}

static HRESULT STDMETHODCALLTYPE
device_CreatePixelShader(
   ID3D10Device1 *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   ID3D10PixelShader **ppPixelShader)
{
   return npt_d3d10_dev_CreatePixelShader((ID3D10Device *)This, pShaderBytecode, BytecodeLength, ppPixelShader);
}

static HRESULT STDMETHODCALLTYPE
device_CreateBlendState(
   ID3D10Device1 *This,
   const D3D10_BLEND_DESC *pBlendStateDesc,
   ID3D10BlendState **ppBlendState)
{
   return npt_d3d10_dev_CreateBlendState((ID3D10Device *)This, pBlendStateDesc, ppBlendState);
}

static HRESULT STDMETHODCALLTYPE
device_CreateDepthStencilState(
   ID3D10Device1 *This,
   const D3D10_DEPTH_STENCIL_DESC *pDepthStencilDesc,
   ID3D10DepthStencilState **ppDepthStencilState)
{
   return npt_d3d10_dev_CreateDepthStencilState((ID3D10Device *)This, pDepthStencilDesc, ppDepthStencilState);
}

static HRESULT STDMETHODCALLTYPE
device_CreateRasterizerState(
   ID3D10Device1 *This,
   const D3D10_RASTERIZER_DESC *pRasterizerDesc,
   ID3D10RasterizerState **ppRasterizerState)
{
   return npt_d3d10_dev_CreateRasterizerState((ID3D10Device *)This, pRasterizerDesc, ppRasterizerState);
}

static HRESULT STDMETHODCALLTYPE
device_CreateSamplerState(
   ID3D10Device1 *This,
   const D3D10_SAMPLER_DESC *pSamplerDesc,
   ID3D10SamplerState **ppSamplerState)
{
   return npt_d3d10_dev_CreateSamplerState((ID3D10Device *)This, pSamplerDesc, ppSamplerState);
}

static HRESULT STDMETHODCALLTYPE
device_CreateQuery(
   ID3D10Device1 *This,
   const D3D10_QUERY_DESC *pQueryDesc,
   ID3D10Query **ppQuery)
{
   return npt_d3d10_dev_CreateQuery((ID3D10Device *)This, pQueryDesc, ppQuery);
}

static HRESULT STDMETHODCALLTYPE
device_CreatePredicate(
   ID3D10Device1 *This,
   const D3D10_QUERY_DESC *pPredicateDesc,
   ID3D10Predicate **ppPredicate)
{
   return npt_d3d10_dev_CreatePredicate((ID3D10Device *)This, pPredicateDesc, ppPredicate);
}

static HRESULT STDMETHODCALLTYPE
device_CreateCounter(
   ID3D10Device1 *This,
   const D3D10_COUNTER_DESC *pCounterDesc,
   ID3D10Counter **ppCounter)
{
   return npt_d3d10_dev_CreateCounter((ID3D10Device *)This, pCounterDesc, ppCounter);
}

static HRESULT STDMETHODCALLTYPE
device_CreateShaderResourceView1(
   ID3D10Device1 *This,
   ID3D10Resource *pResource,
   const D3D10_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
   ID3D10ShaderResourceView1 **ppSRView)
{
   return npt_d3d10_dev_CreateShaderResourceView1(This, pResource, pDesc, ppSRView);
}

static HRESULT STDMETHODCALLTYPE
device_CreateBlendState1(
   ID3D10Device1 *This,
   const D3D10_BLEND_DESC1 *pBlendStateDesc,
   ID3D10BlendState1 **ppBlendState)
{
   return npt_d3d10_dev_CreateBlendState1(This, pBlendStateDesc, ppBlendState);
}

static const ID3D10Device1Vtbl npt_d3d10_device1_vtbl = {
   .QueryInterface = device_QueryInterface,
   .AddRef = device_AddRef,
   .Release = device_Release,
   .VSSetConstantBuffers = device_VSSetConstantBuffers,
   .PSSetShaderResources = device_PSSetShaderResources,
   .PSSetShader = device_PSSetShader,
   .PSSetSamplers = device_PSSetSamplers,
   .VSSetShader = device_VSSetShader,
   .DrawIndexed = device_DrawIndexed,
   .Draw = device_Draw,
   .PSSetConstantBuffers = device_PSSetConstantBuffers,
   .IASetInputLayout = device_IASetInputLayout,
   .IASetVertexBuffers = device_IASetVertexBuffers,
   .IASetIndexBuffer = device_IASetIndexBuffer,
   .DrawIndexedInstanced = device_DrawIndexedInstanced,
   .DrawInstanced = device_DrawInstanced,
   .GSSetConstantBuffers = device_GSSetConstantBuffers,
   .GSSetShader = device_GSSetShader,
   .IASetPrimitiveTopology = device_IASetPrimitiveTopology,
   .VSSetShaderResources = device_VSSetShaderResources,
   .VSSetSamplers = device_VSSetSamplers,
   .SetPredication = device_SetPredication,
   .GSSetShaderResources = device_GSSetShaderResources,
   .GSSetSamplers = device_GSSetSamplers,
   .OMSetRenderTargets = device_OMSetRenderTargets,
   .OMSetBlendState = device_OMSetBlendState,
   .OMSetDepthStencilState = device_OMSetDepthStencilState,
   .SOSetTargets = device_SOSetTargets,
   .DrawAuto = device_DrawAuto,
   .RSSetState = device_RSSetState,
   .RSSetViewports = device_RSSetViewports,
   .RSSetScissorRects = device_RSSetScissorRects,
   .CopySubresourceRegion = device_CopySubresourceRegion,
   .CopyResource = device_CopyResource,
   .UpdateSubresource = device_UpdateSubresource,
   .ClearRenderTargetView = device_ClearRenderTargetView,
   .ClearDepthStencilView = device_ClearDepthStencilView,
   .GenerateMips = device_GenerateMips,
   .ResolveSubresource = device_ResolveSubresource,
   .VSGetConstantBuffers = device_VSGetConstantBuffers,
   .PSGetShaderResources = device_PSGetShaderResources,
   .PSGetShader = device_PSGetShader,
   .PSGetSamplers = device_PSGetSamplers,
   .VSGetShader = device_VSGetShader,
   .PSGetConstantBuffers = device_PSGetConstantBuffers,
   .IAGetInputLayout = device_IAGetInputLayout,
   .IAGetVertexBuffers = device_IAGetVertexBuffers,
   .IAGetIndexBuffer = device_IAGetIndexBuffer,
   .GSGetConstantBuffers = device_GSGetConstantBuffers,
   .GSGetShader = device_GSGetShader,
   .IAGetPrimitiveTopology = device_IAGetPrimitiveTopology,
   .VSGetShaderResources = device_VSGetShaderResources,
   .VSGetSamplers = device_VSGetSamplers,
   .GetPredication = device_GetPredication,
   .GSGetShaderResources = device_GSGetShaderResources,
   .GSGetSamplers = device_GSGetSamplers,
   .OMGetRenderTargets = device_OMGetRenderTargets,
   .OMGetBlendState = device_OMGetBlendState,
   .OMGetDepthStencilState = device_OMGetDepthStencilState,
   .SOGetTargets = device_SOGetTargets,
   .RSGetState = device_RSGetState,
   .RSGetViewports = device_RSGetViewports,
   .RSGetScissorRects = device_RSGetScissorRects,
   .GetDeviceRemovedReason = device_GetDeviceRemovedReason,
   .SetExceptionMode = device_SetExceptionMode,
   .GetExceptionMode = device_GetExceptionMode,
   .GetPrivateData = device_GetPrivateData,
   .SetPrivateData = device_SetPrivateData,
   .SetPrivateDataInterface = device_SetPrivateDataInterface,
   .ClearState = device_ClearState,
   .Flush = device_Flush,
   .CreateBuffer = device_CreateBuffer,
   .CreateTexture1D = device_CreateTexture1D,
   .CreateTexture2D = device_CreateTexture2D,
   .CreateTexture3D = device_CreateTexture3D,
   .CreateShaderResourceView = device_CreateShaderResourceView,
   .CreateRenderTargetView = device_CreateRenderTargetView,
   .CreateDepthStencilView = device_CreateDepthStencilView,
   .CreateInputLayout = device_CreateInputLayout,
   .CreateVertexShader = device_CreateVertexShader,
   .CreateGeometryShader = device_CreateGeometryShader,
   .CreateGeometryShaderWithStreamOutput = device_CreateGeometryShaderWithStreamOutput,
   .CreatePixelShader = device_CreatePixelShader,
   .CreateBlendState = device_CreateBlendState,
   .CreateDepthStencilState = device_CreateDepthStencilState,
   .CreateRasterizerState = device_CreateRasterizerState,
   .CreateSamplerState = device_CreateSamplerState,
   .CreateQuery = device_CreateQuery,
   .CreatePredicate = device_CreatePredicate,
   .CreateCounter = device_CreateCounter,
   .CheckFormatSupport = device_CheckFormatSupport,
   .CheckMultisampleQualityLevels = device_CheckMultisampleQualityLevels,
   .CheckCounterInfo = device_CheckCounterInfo,
   .CheckCounter = device_CheckCounter,
   .GetCreationFlags = device_GetCreationFlags,
   .OpenSharedResource = device_OpenSharedResource,
   .SetTextFilterSize = device_SetTextFilterSize,
   .GetTextFilterSize = device_GetTextFilterSize,
   .CreateShaderResourceView1 = device_CreateShaderResourceView1,
   .CreateBlendState1 = device_CreateBlendState1,
   .GetFeatureLevel = device_GetFeatureLevel,
};

_Static_assert(sizeof(ID3D10Device1Vtbl) == 101 * sizeof(void *), "device vtable size");
_Static_assert(offsetof(ID3D10Device1Vtbl, GetFeatureLevel) == 100 * sizeof(void *), "device vtable layout");

HRESULT
npt_d3d10_facet_qi(struct npt_com_base *primary, const GUID *riid, void **out)
{
   if (!out) return E_POINTER;
   *out = NULL;
   const GUID *base_iid = npt_com_primary_base_iid(primary);
   if (base_iid && IsEqualGUID(base_iid, &IID_ID3D11Device) &&
       (IsEqualGUID(riid, &IID_ID3D10Device) || IsEqualGUID(riid, &IID_ID3D10Device1))) {
      struct npt_d3d10_device_facet *facet = npt_com_load_local_d3d10(primary);
      if (!facet) {
         facet = calloc(1, sizeof(*facet));
         if (!facet) return E_OUTOFMEMORY;
         facet->base.lpVtbl = &npt_d3d10_device1_vtbl;
         facet->base.host_id = npt_com_primary_host_id(primary);
         facet->base.owner = primary;
         facet->base.kind = NPT_D3D10_FACET_DEVICE1;
         facet->text_filter_width = 1;
         facet->text_filter_height = 1;
         facet = npt_d3d10_publish_facet(primary, &facet->base);
      }
      IUnknown *owner = (IUnknown *)primary;
      owner->lpVtbl->AddRef(owner);
      *out = facet;
      return S_OK;
   }
   HRESULT hr = npt_d3d10_resource_qi(primary, riid, out);
   if (hr != E_NOINTERFACE) return hr;
   return npt_d3d10_state_qi(primary, riid, out);
}

#else
#include "npt_d3d10_facets.h"

HRESULT
npt_d3d10_facet_qi(struct npt_com_base *primary, const GUID *riid, void **out)
{
   (void)primary;
   (void)riid;
   if (out)
      *out = NULL;
   return NPT_E_NOINTERFACE;
}

void
npt_d3d10_facet_destroy(struct npt_com_base *primary)
{
   (void)primary;
}
#endif
