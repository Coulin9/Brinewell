#ifdef _WIN32
#include <d3d10_1.h>
#include <d3d11_4.h>
#define NPT_D3D10_USE_SDK_HEADERS 1
#include "npt_d3d10_facets.h"
#include "npt_d3d10_device_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static ID3D11Device *device11(void *iface)
{
   return (ID3D11Device *)((struct npt_d3d10_facet_base *)iface)->owner;
}

static HRESULT finish_create(HRESULT hr, IUnknown *object, const GUID *iid, void **out)
{
   if (SUCCEEDED(hr) && out && object)
      hr = npt_d3d10_wrap_d3d11_object(object, iid, out);
   if (object)
      object->lpVtbl->Release(object);
   return hr;
}

static UINT full_mip_count(UINT w, UINT h, UINT d)
{
   UINT n = 1, max = w > h ? w : h;
   if (d > max) max = d;
   while (max > 1) { max >>= 1; ++n; }
   return n;
}

static HRESULT texture_subresources(const D3D10_SUBRESOURCE_DATA *src,
                                    UINT w, UINT h, UINT d,
                                    UINT mip_levels, UINT array_size,
                                    D3D11_SUBRESOURCE_DATA **out)
{
   *out = NULL;
   if (!src) return S_OK;
   if (!w || !h || !d || !array_size) return E_INVALIDARG;
   UINT max_mips = full_mip_count(w, h, d);
   if (mip_levels > max_mips) return E_INVALIDARG;
   UINT levels = mip_levels ? mip_levels : max_mips;
   if ((size_t)levels > SIZE_MAX / (size_t)array_size) return E_OUTOFMEMORY;
   size_t count = (size_t)levels * (size_t)array_size;
   if (count > SIZE_MAX / sizeof(**out)) return E_OUTOFMEMORY;
   *out = malloc(count * sizeof(**out));
   if (!*out) return E_OUTOFMEMORY;
   for (size_t i = 0; i < count; ++i) {
      (*out)[i].pSysMem = src[i].pSysMem;
      (*out)[i].SysMemPitch = src[i].SysMemPitch;
      (*out)[i].SysMemSlicePitch = src[i].SysMemSlicePitch;
   }
   return S_OK;
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateBuffer(ID3D10Device *iface, const D3D10_BUFFER_DESC *desc, const D3D10_SUBRESOURCE_DATA *initial, ID3D10Buffer **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   D3D11_BUFFER_DESC d = {desc->ByteWidth, (D3D11_USAGE)desc->Usage, desc->BindFlags,
                          desc->CPUAccessFlags, npt_d3d10_misc_flags_to_d3d11(desc->MiscFlags), 0};
   D3D11_SUBRESOURCE_DATA data;
   if (initial) { data.pSysMem = initial->pSysMem; data.SysMemPitch = initial->SysMemPitch; data.SysMemSlicePitch = initial->SysMemSlicePitch; }
   ID3D11Device *dev = device11(iface);
   ID3D11Buffer *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateBuffer(dev, &d, initial ? &data : NULL, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Buffer, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateTexture1D(ID3D10Device *iface, const D3D10_TEXTURE1D_DESC *desc, const D3D10_SUBRESOURCE_DATA *initial, ID3D10Texture1D **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   D3D11_TEXTURE1D_DESC d = {0};
   d.Width = desc->Width;
   d.MipLevels = desc->MipLevels;
   d.ArraySize = desc->ArraySize;
   d.Format = desc->Format;
   d.Usage = (D3D11_USAGE)desc->Usage;
   d.BindFlags = desc->BindFlags;
   d.CPUAccessFlags = desc->CPUAccessFlags;
   d.MiscFlags = npt_d3d10_misc_flags_to_d3d11(desc->MiscFlags);
   D3D11_SUBRESOURCE_DATA *data = NULL;
   HRESULT hr = texture_subresources(initial, desc->Width, 1, 1, desc->MipLevels, desc->ArraySize, &data);
   if (FAILED(hr)) return hr;
   ID3D11Device *dev = device11(iface);
   ID3D11Texture1D *obj = NULL;
   hr = dev->lpVtbl->CreateTexture1D(dev, &d, data, out ? &obj : NULL);
   free(data);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Texture1D, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateTexture2D(ID3D10Device *iface, const D3D10_TEXTURE2D_DESC *desc, const D3D10_SUBRESOURCE_DATA *initial, ID3D10Texture2D **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   D3D11_TEXTURE2D_DESC d = {0};
   d.Width = desc->Width;
   d.Height = desc->Height;
   d.MipLevels = desc->MipLevels;
   d.ArraySize = desc->ArraySize;
   d.Format = desc->Format;
   d.SampleDesc = desc->SampleDesc;
   d.Usage = (D3D11_USAGE)desc->Usage;
   d.BindFlags = desc->BindFlags;
   d.CPUAccessFlags = desc->CPUAccessFlags;
   d.MiscFlags = npt_d3d10_misc_flags_to_d3d11(desc->MiscFlags);
   D3D11_SUBRESOURCE_DATA *data = NULL;
   HRESULT hr = texture_subresources(initial, desc->Width, desc->Height, 1, desc->MipLevels, desc->ArraySize, &data);
   if (FAILED(hr)) return hr;
   ID3D11Device *dev = device11(iface);
   ID3D11Texture2D *obj = NULL;
   hr = dev->lpVtbl->CreateTexture2D(dev, &d, data, out ? &obj : NULL);
   free(data);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Texture2D, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateTexture3D(ID3D10Device *iface, const D3D10_TEXTURE3D_DESC *desc, const D3D10_SUBRESOURCE_DATA *initial, ID3D10Texture3D **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   D3D11_TEXTURE3D_DESC d = {0};
   d.Width = desc->Width;
   d.Height = desc->Height;
   d.Depth = desc->Depth;
   d.MipLevels = desc->MipLevels;
   d.Format = desc->Format;
   d.Usage = (D3D11_USAGE)desc->Usage;
   d.BindFlags = desc->BindFlags;
   d.CPUAccessFlags = desc->CPUAccessFlags;
   d.MiscFlags = npt_d3d10_misc_flags_to_d3d11(desc->MiscFlags);
   D3D11_SUBRESOURCE_DATA *data = NULL;
   HRESULT hr = texture_subresources(initial, desc->Width, desc->Height, desc->Depth, desc->MipLevels, 1, &data);
   if (FAILED(hr)) return hr;
   ID3D11Device *dev = device11(iface);
   ID3D11Texture3D *obj = NULL;
   hr = dev->lpVtbl->CreateTexture3D(dev, &d, data, out ? &obj : NULL);
   free(data);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Texture3D, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateShaderResourceView(ID3D10Device *iface, ID3D10Resource *resource, const D3D10_SHADER_RESOURCE_VIEW_DESC *desc, ID3D10ShaderResourceView **out)
{
   if (out) *out = NULL;
   D3D11_SHADER_RESOURCE_VIEW_DESC d = {0};
   if (desc) {
      d.Format = desc->Format;
      d.ViewDimension = (D3D11_SRV_DIMENSION)desc->ViewDimension;
      size_t bytes = sizeof(*desc) - offsetof(D3D10_SHADER_RESOURCE_VIEW_DESC, Buffer);
      size_t capacity = sizeof(d) - offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Buffer);
      memcpy(&d.Buffer, &desc->Buffer, bytes < capacity ? bytes : capacity);
   }
   ID3D11Resource *resource11 = NULL;
   HRESULT hr = resource ? npt_d3d10_unwrap_to_d3d11((IUnknown *)resource, &IID_ID3D11Resource, (void **)&resource11) : S_OK;
   if (FAILED(hr)) { if (resource11) resource11->lpVtbl->Release(resource11); return hr; }
   ID3D11Device *dev = device11(iface);
   ID3D11ShaderResourceView *obj = NULL;
   hr = dev->lpVtbl->CreateShaderResourceView(dev, resource11, desc ? &d : NULL, out ? &obj : NULL);
   if (resource11) resource11->lpVtbl->Release(resource11);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10ShaderResourceView, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateShaderResourceView1(ID3D10Device1 *iface, ID3D10Resource *resource, const D3D10_SHADER_RESOURCE_VIEW_DESC1 *desc, ID3D10ShaderResourceView1 **out)
{
   if (out) *out = NULL;
   D3D11_SHADER_RESOURCE_VIEW_DESC d = {0};
   if (desc) {
      d.Format = desc->Format;
      d.ViewDimension = (D3D11_SRV_DIMENSION)desc->ViewDimension;
      size_t bytes = sizeof(*desc) - offsetof(D3D10_SHADER_RESOURCE_VIEW_DESC1, Buffer);
      size_t capacity = sizeof(d) - offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Buffer);
      memcpy(&d.Buffer, &desc->Buffer, bytes < capacity ? bytes : capacity);
   }
   ID3D11Resource *resource11 = NULL;
   HRESULT hr = resource ? npt_d3d10_unwrap_to_d3d11((IUnknown *)resource, &IID_ID3D11Resource, (void **)&resource11) : S_OK;
   if (FAILED(hr)) { if (resource11) resource11->lpVtbl->Release(resource11); return hr; }
   ID3D11Device *dev = device11(iface);
   ID3D11ShaderResourceView *obj = NULL;
   hr = dev->lpVtbl->CreateShaderResourceView(dev, resource11, desc ? &d : NULL, out ? &obj : NULL);
   if (resource11) resource11->lpVtbl->Release(resource11);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10ShaderResourceView1, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateRenderTargetView(ID3D10Device *iface, ID3D10Resource *resource, const D3D10_RENDER_TARGET_VIEW_DESC *desc, ID3D10RenderTargetView **out)
{
   if (out) *out = NULL;
   D3D11_RENDER_TARGET_VIEW_DESC d = {0};
   if (desc) {
      d.Format = desc->Format;
      d.ViewDimension = (D3D11_RTV_DIMENSION)desc->ViewDimension;
      size_t bytes = sizeof(*desc) - offsetof(D3D10_RENDER_TARGET_VIEW_DESC, Buffer);
      size_t capacity = sizeof(d) - offsetof(D3D11_RENDER_TARGET_VIEW_DESC, Buffer);
      memcpy(&d.Buffer, &desc->Buffer, bytes < capacity ? bytes : capacity);
   }
   ID3D11Resource *resource11 = NULL;
   HRESULT hr = resource ? npt_d3d10_unwrap_to_d3d11((IUnknown *)resource, &IID_ID3D11Resource, (void **)&resource11) : S_OK;
   if (FAILED(hr)) { if (resource11) resource11->lpVtbl->Release(resource11); return hr; }
   ID3D11Device *dev = device11(iface);
   ID3D11RenderTargetView *obj = NULL;
   hr = dev->lpVtbl->CreateRenderTargetView(dev, resource11, desc ? &d : NULL, out ? &obj : NULL);
   if (resource11) resource11->lpVtbl->Release(resource11);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10RenderTargetView, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateDepthStencilView(ID3D10Device *iface, ID3D10Resource *resource, const D3D10_DEPTH_STENCIL_VIEW_DESC *desc, ID3D10DepthStencilView **out)
{
   if (out) *out = NULL;
   D3D11_DEPTH_STENCIL_VIEW_DESC d = {0};
   if (desc) {
      d.Format = desc->Format;
      d.ViewDimension = (D3D11_DSV_DIMENSION)desc->ViewDimension;
      size_t bytes = sizeof(*desc) - offsetof(D3D10_DEPTH_STENCIL_VIEW_DESC, Texture1D);
      size_t capacity = sizeof(d) - offsetof(D3D11_DEPTH_STENCIL_VIEW_DESC, Texture1D);
      memcpy(&d.Texture1D, &desc->Texture1D, bytes < capacity ? bytes : capacity);
   }
   ID3D11Resource *resource11 = NULL;
   HRESULT hr = resource ? npt_d3d10_unwrap_to_d3d11((IUnknown *)resource, &IID_ID3D11Resource, (void **)&resource11) : S_OK;
   if (FAILED(hr)) { if (resource11) resource11->lpVtbl->Release(resource11); return hr; }
   ID3D11Device *dev = device11(iface);
   ID3D11DepthStencilView *obj = NULL;
   hr = dev->lpVtbl->CreateDepthStencilView(dev, resource11, desc ? &d : NULL, out ? &obj : NULL);
   if (resource11) resource11->lpVtbl->Release(resource11);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10DepthStencilView, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateInputLayout(ID3D10Device *iface, const D3D10_INPUT_ELEMENT_DESC *elements, UINT count, const void *bytecode, SIZE_T length, ID3D10InputLayout **out)
{
   if (out) *out = NULL;
   D3D11_INPUT_ELEMENT_DESC *items = NULL;
   if (elements && count) {
      if ((size_t)count > SIZE_MAX / sizeof(*items)) return E_OUTOFMEMORY;
      items = malloc((size_t)count * sizeof(*items));
      if (!items) return E_OUTOFMEMORY;
      for (UINT i = 0; i < count; ++i) {
         items[i].SemanticName = elements[i].SemanticName;
         items[i].SemanticIndex = elements[i].SemanticIndex;
         items[i].Format = elements[i].Format;
         items[i].InputSlot = elements[i].InputSlot;
         items[i].AlignedByteOffset = elements[i].AlignedByteOffset;
         items[i].InputSlotClass = (D3D11_INPUT_CLASSIFICATION)elements[i].InputSlotClass;
         items[i].InstanceDataStepRate = elements[i].InstanceDataStepRate;
      }
   }
   ID3D11Device *dev = device11(iface);
   ID3D11InputLayout *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateInputLayout(dev, elements ? items : NULL, count, bytecode, length, out ? &obj : NULL);
   free(items);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10InputLayout, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateVertexShader(ID3D10Device *iface, const void *bytecode, SIZE_T length, ID3D10VertexShader **out)
{
   if (out) *out = NULL;
   ID3D11Device *dev = device11(iface);
   ID3D11VertexShader *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateVertexShader(dev, bytecode, length, NULL, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10VertexShader, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateGeometryShader(ID3D10Device *iface, const void *bytecode, SIZE_T length, ID3D10GeometryShader **out)
{
   if (out) *out = NULL;
   ID3D11Device *dev = device11(iface);
   ID3D11GeometryShader *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateGeometryShader(dev, bytecode, length, NULL, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10GeometryShader, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreatePixelShader(ID3D10Device *iface, const void *bytecode, SIZE_T length, ID3D10PixelShader **out)
{
   if (out) *out = NULL;
   ID3D11Device *dev = device11(iface);
   ID3D11PixelShader *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreatePixelShader(dev, bytecode, length, NULL, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10PixelShader, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateGeometryShaderWithStreamOutput(ID3D10Device *iface, const void *bytecode, SIZE_T length, const D3D10_SO_DECLARATION_ENTRY *entries, UINT count, UINT stride, ID3D10GeometryShader **out)
{
   if (out) *out = NULL;
   D3D11_SO_DECLARATION_ENTRY *items = NULL;
   if (entries && count) {
      if ((size_t)count > SIZE_MAX / sizeof(*items)) return E_OUTOFMEMORY;
      items = malloc((size_t)count * sizeof(*items));
      if (!items) return E_OUTOFMEMORY;
      for (UINT i = 0; i < count; ++i) {
         items[i].Stream = 0;
         items[i].SemanticName = entries[i].SemanticName;
         items[i].SemanticIndex = entries[i].SemanticIndex;
         items[i].StartComponent = entries[i].StartComponent;
         items[i].ComponentCount = entries[i].ComponentCount;
         items[i].OutputSlot = entries[i].OutputSlot;
      }
   }
   ID3D11Device *dev = device11(iface);
   ID3D11GeometryShader *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateGeometryShaderWithStreamOutput(dev, bytecode, length, entries ? items : NULL, count, &stride, stride ? 1 : 0, 0, NULL, out ? &obj : NULL);
   free(items);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10GeometryShader, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateBlendState(ID3D10Device *iface, const D3D10_BLEND_DESC *desc, ID3D10BlendState **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   D3D11_BLEND_DESC d = {0};
   d.AlphaToCoverageEnable = desc->AlphaToCoverageEnable;
   d.IndependentBlendEnable = TRUE;
   for (UINT i = 0; i < 8; ++i) {
      d.RenderTarget[i].BlendEnable = desc->BlendEnable[i];
      d.RenderTarget[i].SrcBlend = (D3D11_BLEND)desc->SrcBlend;
      d.RenderTarget[i].DestBlend = (D3D11_BLEND)desc->DestBlend;
      d.RenderTarget[i].BlendOp = (D3D11_BLEND_OP)desc->BlendOp;
      d.RenderTarget[i].SrcBlendAlpha = (D3D11_BLEND)desc->SrcBlendAlpha;
      d.RenderTarget[i].DestBlendAlpha = (D3D11_BLEND)desc->DestBlendAlpha;
      d.RenderTarget[i].BlendOpAlpha = (D3D11_BLEND_OP)desc->BlendOpAlpha;
      d.RenderTarget[i].RenderTargetWriteMask = desc->RenderTargetWriteMask[i];
   }
   ID3D11Device *dev = device11(iface);
   ID3D11BlendState *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateBlendState(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10BlendState, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateBlendState1(ID3D10Device1 *iface, const D3D10_BLEND_DESC1 *desc, ID3D10BlendState1 **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   D3D11_BLEND_DESC d = {0};
   d.AlphaToCoverageEnable = desc->AlphaToCoverageEnable;
   d.IndependentBlendEnable = desc->IndependentBlendEnable;
   for (UINT i = 0; i < 8; ++i) {
      d.RenderTarget[i].BlendEnable = desc->RenderTarget[i].BlendEnable;
      d.RenderTarget[i].SrcBlend = (D3D11_BLEND)desc->RenderTarget[i].SrcBlend;
      d.RenderTarget[i].DestBlend = (D3D11_BLEND)desc->RenderTarget[i].DestBlend;
      d.RenderTarget[i].BlendOp = (D3D11_BLEND_OP)desc->RenderTarget[i].BlendOp;
      d.RenderTarget[i].SrcBlendAlpha = (D3D11_BLEND)desc->RenderTarget[i].SrcBlendAlpha;
      d.RenderTarget[i].DestBlendAlpha = (D3D11_BLEND)desc->RenderTarget[i].DestBlendAlpha;
      d.RenderTarget[i].BlendOpAlpha = (D3D11_BLEND_OP)desc->RenderTarget[i].BlendOpAlpha;
      d.RenderTarget[i].RenderTargetWriteMask = desc->RenderTarget[i].RenderTargetWriteMask;
   }
   ID3D11Device *dev = device11(iface);
   ID3D11BlendState *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateBlendState(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10BlendState1, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateDepthStencilState(ID3D10Device *iface, const D3D10_DEPTH_STENCIL_DESC *desc, ID3D10DepthStencilState **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   _Static_assert(sizeof(D3D10_DEPTH_STENCIL_DESC) == sizeof(D3D11_DEPTH_STENCIL_DESC), "descriptor layout mismatch");
   D3D11_DEPTH_STENCIL_DESC d;
   memcpy(&d, desc, sizeof(d));
   ID3D11Device *dev = device11(iface);
   ID3D11DepthStencilState *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateDepthStencilState(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10DepthStencilState, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateRasterizerState(ID3D10Device *iface, const D3D10_RASTERIZER_DESC *desc, ID3D10RasterizerState **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   _Static_assert(sizeof(D3D10_RASTERIZER_DESC) == sizeof(D3D11_RASTERIZER_DESC), "descriptor layout mismatch");
   D3D11_RASTERIZER_DESC d;
   memcpy(&d, desc, sizeof(d));
   ID3D11Device *dev = device11(iface);
   ID3D11RasterizerState *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateRasterizerState(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10RasterizerState, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateSamplerState(ID3D10Device *iface, const D3D10_SAMPLER_DESC *desc, ID3D10SamplerState **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   _Static_assert(sizeof(D3D10_SAMPLER_DESC) == sizeof(D3D11_SAMPLER_DESC), "descriptor layout mismatch");
   D3D11_SAMPLER_DESC d;
   memcpy(&d, desc, sizeof(d));
   ID3D11Device *dev = device11(iface);
   ID3D11SamplerState *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateSamplerState(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10SamplerState, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateQuery(ID3D10Device *iface, const D3D10_QUERY_DESC *desc, ID3D10Query **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   _Static_assert(sizeof(D3D10_QUERY_DESC) == sizeof(D3D11_QUERY_DESC), "descriptor layout mismatch");
   D3D11_QUERY_DESC d;
   memcpy(&d, desc, sizeof(d));
   ID3D11Device *dev = device11(iface);
   ID3D11Query *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateQuery(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Query, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreatePredicate(ID3D10Device *iface, const D3D10_QUERY_DESC *desc, ID3D10Predicate **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   _Static_assert(sizeof(D3D10_QUERY_DESC) == sizeof(D3D11_QUERY_DESC), "descriptor layout mismatch");
   D3D11_QUERY_DESC d;
   memcpy(&d, desc, sizeof(d));
   ID3D11Device *dev = device11(iface);
   ID3D11Predicate *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreatePredicate(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Predicate, (void **)out);
}

HRESULT STDMETHODCALLTYPE
npt_d3d10_dev_CreateCounter(ID3D10Device *iface, const D3D10_COUNTER_DESC *desc, ID3D10Counter **out)
{
   if (out) *out = NULL;
   if (!desc) return E_INVALIDARG;
   _Static_assert(sizeof(D3D10_COUNTER_DESC) == sizeof(D3D11_COUNTER_DESC), "descriptor layout mismatch");
   D3D11_COUNTER_DESC d;
   memcpy(&d, desc, sizeof(d));
   ID3D11Device *dev = device11(iface);
   ID3D11Counter *obj = NULL;
   HRESULT hr = dev->lpVtbl->CreateCounter(dev, &d, out ? &obj : NULL);
   return finish_create(hr, (IUnknown *)obj, &IID_ID3D10Counter, (void **)out);
}

#else
typedef int npt_d3d10_device_create_unused;
#endif
