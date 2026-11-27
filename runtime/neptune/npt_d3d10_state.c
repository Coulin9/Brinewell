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

#define OWNER(self, type) ((type *)((struct npt_d3d10_facet_base *)(self))->owner)
#define COMMON(name, iface, owner_type) \
static HRESULT STDMETHODCALLTYPE name##_qi(iface *self, REFIID iid, void **out) \
{ IUnknown *p = OWNER(self, IUnknown); return p->lpVtbl->QueryInterface(p, iid, out); } \
static ULONG STDMETHODCALLTYPE name##_addref(iface *self) \
{ IUnknown *p = OWNER(self, IUnknown); return p->lpVtbl->AddRef(p); } \
static ULONG STDMETHODCALLTYPE name##_release(iface *self) \
{ IUnknown *p = OWNER(self, IUnknown); return p->lpVtbl->Release(p); } \
static void STDMETHODCALLTYPE name##_device(iface *self, ID3D10Device **out) \
{ owner_type *p = OWNER(self, owner_type); ID3D11Device *dev = NULL; \
  if (!out) return; *out = NULL; p->lpVtbl->GetDevice(p, &dev); \
  if (dev) { npt_d3d10_wrap_d3d11_object((IUnknown *)dev, &IID_ID3D10Device, (void **)out); \
             dev->lpVtbl->Release(dev); } } \
static HRESULT STDMETHODCALLTYPE name##_get_private(iface *self, REFGUID guid, UINT *size, void *data) \
{ owner_type *p = OWNER(self, owner_type); return p->lpVtbl->GetPrivateData(p, guid, size, data); } \
static HRESULT STDMETHODCALLTYPE name##_set_private(iface *self, REFGUID guid, UINT size, const void *data) \
{ owner_type *p = OWNER(self, owner_type); return p->lpVtbl->SetPrivateData(p, guid, size, data); } \
static HRESULT STDMETHODCALLTYPE name##_set_interface(iface *self, REFGUID guid, const IUnknown *data) \
{ owner_type *p = OWNER(self, owner_type); return p->lpVtbl->SetPrivateDataInterface(p, guid, data); }
#define COMMON_VT(name) name##_qi, name##_addref, name##_release, name##_device, \
                        name##_get_private, name##_set_private, name##_set_interface
#define VIEW_RESOURCE(name, iface, owner_type) \
static void STDMETHODCALLTYPE name##_resource(iface *self, ID3D10Resource **out) \
{ owner_type *p = OWNER(self, owner_type); ID3D11Resource *resource = NULL; \
  if (!out) return; *out = NULL; p->lpVtbl->GetResource(p, &resource); \
  if (resource) { npt_d3d10_wrap_d3d11_object((IUnknown *)resource, &IID_ID3D10Resource, (void **)out); \
                  resource->lpVtbl->Release(resource); } }

COMMON(srv, ID3D10ShaderResourceView1, ID3D11ShaderResourceView)
COMMON(rtv, ID3D10RenderTargetView, ID3D11RenderTargetView)
COMMON(dsv, ID3D10DepthStencilView, ID3D11DepthStencilView)
COMMON(layout, ID3D10InputLayout, ID3D11InputLayout)
COMMON(vs, ID3D10VertexShader, ID3D11VertexShader)
COMMON(gs, ID3D10GeometryShader, ID3D11GeometryShader)
COMMON(ps, ID3D10PixelShader, ID3D11PixelShader)
COMMON(blend, ID3D10BlendState1, ID3D11BlendState)
COMMON(depth, ID3D10DepthStencilState, ID3D11DepthStencilState)
COMMON(raster, ID3D10RasterizerState, ID3D11RasterizerState)
COMMON(sampler, ID3D10SamplerState, ID3D11SamplerState)
COMMON(query, ID3D10Query, ID3D11Query)
COMMON(predicate, ID3D10Predicate, ID3D11Predicate)
COMMON(counter, ID3D10Counter, ID3D11Counter)

VIEW_RESOURCE(srv, ID3D10ShaderResourceView1, ID3D11ShaderResourceView)
VIEW_RESOURCE(rtv, ID3D10RenderTargetView, ID3D11RenderTargetView)
VIEW_RESOURCE(dsv, ID3D10DepthStencilView, ID3D11DepthStencilView)

static void STDMETHODCALLTYPE srv_desc(ID3D10ShaderResourceView1 *self, D3D10_SHADER_RESOURCE_VIEW_DESC *out)
{ if (!out) return; ID3D11ShaderResourceView *p = OWNER(self, ID3D11ShaderResourceView); D3D11_SHADER_RESOURCE_VIEW_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); npt_d3d11_srv_desc_to_d3d10(&d, out); }
static void STDMETHODCALLTYPE srv_desc1(ID3D10ShaderResourceView1 *self, D3D10_SHADER_RESOURCE_VIEW_DESC1 *out)
{ if (!out) return; ID3D11ShaderResourceView *p = OWNER(self, ID3D11ShaderResourceView); D3D11_SHADER_RESOURCE_VIEW_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); npt_d3d11_srv_desc_to_d3d10_1(&d, out); }
static void STDMETHODCALLTYPE rtv_desc(ID3D10RenderTargetView *self, D3D10_RENDER_TARGET_VIEW_DESC *out)
{ if (!out) return; ID3D11RenderTargetView *p = OWNER(self, ID3D11RenderTargetView); D3D11_RENDER_TARGET_VIEW_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "RTV descriptor layout"); memcpy(out, &d, sizeof(d)); }
static void STDMETHODCALLTYPE dsv_desc(ID3D10DepthStencilView *self, D3D10_DEPTH_STENCIL_VIEW_DESC *out)
{ if (!out) return; ID3D11DepthStencilView *p = OWNER(self, ID3D11DepthStencilView); D3D11_DEPTH_STENCIL_VIEW_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); memset(out, 0, sizeof(*out)); out->Format = d.Format; out->ViewDimension = (D3D10_DSV_DIMENSION)d.ViewDimension; memcpy(&out->Texture1D, &d.Texture1D, sizeof(*out) - offsetof(D3D10_DEPTH_STENCIL_VIEW_DESC, Texture1D)); }
static void STDMETHODCALLTYPE blend_desc(ID3D10BlendState1 *self, D3D10_BLEND_DESC *out)
{ if (!out) return; ID3D11BlendState *p = OWNER(self, ID3D11BlendState); D3D11_BLEND_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); npt_d3d11_blend_desc_to_d3d10(&d, out); }
static void STDMETHODCALLTYPE blend_desc1(ID3D10BlendState1 *self, D3D10_BLEND_DESC1 *out)
{ if (!out) return; ID3D11BlendState *p = OWNER(self, ID3D11BlendState); D3D11_BLEND_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); npt_d3d11_blend_desc_to_d3d10_1(&d, out); }
static void STDMETHODCALLTYPE depth_desc(ID3D10DepthStencilState *self, D3D10_DEPTH_STENCIL_DESC *out)
{ if (!out) return; ID3D11DepthStencilState *p = OWNER(self, ID3D11DepthStencilState); D3D11_DEPTH_STENCIL_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "DEPTH_STENCIL descriptor layout"); memcpy(out, &d, sizeof(d)); }
static void STDMETHODCALLTYPE raster_desc(ID3D10RasterizerState *self, D3D10_RASTERIZER_DESC *out)
{ if (!out) return; ID3D11RasterizerState *p = OWNER(self, ID3D11RasterizerState); D3D11_RASTERIZER_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "RASTERIZER descriptor layout"); memcpy(out, &d, sizeof(d)); }
static void STDMETHODCALLTYPE sampler_desc(ID3D10SamplerState *self, D3D10_SAMPLER_DESC *out)
{ if (!out) return; ID3D11SamplerState *p = OWNER(self, ID3D11SamplerState); D3D11_SAMPLER_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "SAMPLER descriptor layout"); memcpy(out, &d, sizeof(d)); }
static void STDMETHODCALLTYPE query_desc(ID3D10Query *self, D3D10_QUERY_DESC *out)
{ if (!out) return; ID3D11Query *p = OWNER(self, ID3D11Query); D3D11_QUERY_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "QUERY descriptor layout"); memcpy(out, &d, sizeof(d)); }
static void STDMETHODCALLTYPE predicate_desc(ID3D10Predicate *self, D3D10_QUERY_DESC *out)
{ if (!out) return; ID3D11Predicate *p = OWNER(self, ID3D11Predicate); D3D11_QUERY_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "QUERY descriptor layout"); memcpy(out, &d, sizeof(d)); }
static void STDMETHODCALLTYPE counter_desc(ID3D10Counter *self, D3D10_COUNTER_DESC *out)
{ if (!out) return; ID3D11Counter *p = OWNER(self, ID3D11Counter); D3D11_COUNTER_DESC d = { 0 }; p->lpVtbl->GetDesc(p, &d); _Static_assert(sizeof(d) == sizeof(*out), "COUNTER descriptor layout"); memcpy(out, &d, sizeof(d)); }

#define ASYNC(name, iface, owner_type) \
static void STDMETHODCALLTYPE name##_begin(iface *self) \
{ owner_type *p = OWNER(self, owner_type); ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; \
  p->lpVtbl->GetDevice(p, &dev); if (!dev) return; dev->lpVtbl->GetImmediateContext(dev, &ctx); \
  if (ctx) { ctx->lpVtbl->Begin(ctx, (ID3D11Asynchronous *)p); ctx->lpVtbl->Release(ctx); } dev->lpVtbl->Release(dev); } \
static void STDMETHODCALLTYPE name##_end(iface *self) \
{ owner_type *p = OWNER(self, owner_type); ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; \
  p->lpVtbl->GetDevice(p, &dev); if (!dev) return; dev->lpVtbl->GetImmediateContext(dev, &ctx); \
  if (ctx) { ctx->lpVtbl->End(ctx, (ID3D11Asynchronous *)p); ctx->lpVtbl->Release(ctx); } dev->lpVtbl->Release(dev); } \
static HRESULT STDMETHODCALLTYPE name##_data(iface *self, void *data, UINT size, UINT flags) \
{ owner_type *p = OWNER(self, owner_type); ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; HRESULT hr = E_FAIL; \
  p->lpVtbl->GetDevice(p, &dev); if (!dev) return hr; dev->lpVtbl->GetImmediateContext(dev, &ctx); \
  if (ctx) { hr = ctx->lpVtbl->GetData(ctx, (ID3D11Asynchronous *)p, data, size, flags); ctx->lpVtbl->Release(ctx); } dev->lpVtbl->Release(dev); return hr; } \
static UINT STDMETHODCALLTYPE name##_size(iface *self) \
{ owner_type *p = OWNER(self, owner_type); return p->lpVtbl->GetDataSize(p); }
ASYNC(query, ID3D10Query, ID3D11Query)
ASYNC(predicate, ID3D10Predicate, ID3D11Predicate)
ASYNC(counter, ID3D10Counter, ID3D11Counter)
static const ID3D10ShaderResourceView1Vtbl srv_vtbl = { COMMON_VT(srv), srv_resource, srv_desc, srv_desc1 };
_Static_assert(sizeof(ID3D10ShaderResourceView1Vtbl) == 10 * sizeof(void *), "ID3D10ShaderResourceView1 vtable size");
static const ID3D10RenderTargetViewVtbl rtv_vtbl = { COMMON_VT(rtv), rtv_resource, rtv_desc };
_Static_assert(sizeof(ID3D10RenderTargetViewVtbl) == 9 * sizeof(void *), "ID3D10RenderTargetView vtable size");
static const ID3D10DepthStencilViewVtbl dsv_vtbl = { COMMON_VT(dsv), dsv_resource, dsv_desc };
_Static_assert(sizeof(ID3D10DepthStencilViewVtbl) == 9 * sizeof(void *), "ID3D10DepthStencilView vtable size");
static const ID3D10InputLayoutVtbl layout_vtbl = { COMMON_VT(layout) };
_Static_assert(sizeof(ID3D10InputLayoutVtbl) == 7 * sizeof(void *), "ID3D10InputLayout vtable size");
static const ID3D10VertexShaderVtbl vs_vtbl = { COMMON_VT(vs) };
_Static_assert(sizeof(ID3D10VertexShaderVtbl) == 7 * sizeof(void *), "ID3D10VertexShader vtable size");
static const ID3D10GeometryShaderVtbl gs_vtbl = { COMMON_VT(gs) };
_Static_assert(sizeof(ID3D10GeometryShaderVtbl) == 7 * sizeof(void *), "ID3D10GeometryShader vtable size");
static const ID3D10PixelShaderVtbl ps_vtbl = { COMMON_VT(ps) };
_Static_assert(sizeof(ID3D10PixelShaderVtbl) == 7 * sizeof(void *), "ID3D10PixelShader vtable size");
static const ID3D10BlendState1Vtbl blend_vtbl = { COMMON_VT(blend), blend_desc, blend_desc1 };
_Static_assert(sizeof(ID3D10BlendState1Vtbl) == 9 * sizeof(void *), "ID3D10BlendState1 vtable size");
static const ID3D10DepthStencilStateVtbl depth_vtbl = { COMMON_VT(depth), depth_desc };
_Static_assert(sizeof(ID3D10DepthStencilStateVtbl) == 8 * sizeof(void *), "ID3D10DepthStencilState vtable size");
static const ID3D10RasterizerStateVtbl raster_vtbl = { COMMON_VT(raster), raster_desc };
_Static_assert(sizeof(ID3D10RasterizerStateVtbl) == 8 * sizeof(void *), "ID3D10RasterizerState vtable size");
static const ID3D10SamplerStateVtbl sampler_vtbl = { COMMON_VT(sampler), sampler_desc };
_Static_assert(sizeof(ID3D10SamplerStateVtbl) == 8 * sizeof(void *), "ID3D10SamplerState vtable size");
static const ID3D10QueryVtbl query_vtbl = { COMMON_VT(query), query_begin, query_end, query_data, query_size, query_desc };
_Static_assert(sizeof(ID3D10QueryVtbl) == 12 * sizeof(void *), "ID3D10Query vtable size");
static const ID3D10PredicateVtbl predicate_vtbl = { COMMON_VT(predicate), predicate_begin, predicate_end, predicate_data, predicate_size, predicate_desc };
_Static_assert(sizeof(ID3D10PredicateVtbl) == 12 * sizeof(void *), "ID3D10Predicate vtable size");
static const ID3D10CounterVtbl counter_vtbl = { COMMON_VT(counter), counter_begin, counter_end, counter_data, counter_size, counter_desc };
_Static_assert(sizeof(ID3D10CounterVtbl) == 12 * sizeof(void *), "ID3D10Counter vtable size");

static int iid_equal(const GUID *a, const GUID *b)
{ return a && b && IsEqualGUID(a, b); }

HRESULT npt_d3d10_state_qi(struct npt_com_base *primary, const GUID *riid, void **out)
{
   const GUID *family;
   const void *vtbl = NULL;
   uint32_t kind = 0;
   struct npt_d3d10_facet_base *candidate;
   if (!primary || !riid || !out)
      return E_NOINTERFACE;
   family = npt_com_primary_base_iid(primary);
   if (iid_equal(family, &IID_ID3D11ShaderResourceView) &&
       (iid_equal(riid, &IID_ID3D10ShaderResourceView1) || iid_equal(riid, &IID_ID3D10ShaderResourceView) || iid_equal(riid, &IID_ID3D10View) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &srv_vtbl; kind = NPT_D3D10_FACET_SRV1;
   }
   if (iid_equal(family, &IID_ID3D11RenderTargetView) &&
       (iid_equal(riid, &IID_ID3D10RenderTargetView) || iid_equal(riid, &IID_ID3D10View) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &rtv_vtbl; kind = NPT_D3D10_FACET_RTV;
   }
   if (iid_equal(family, &IID_ID3D11DepthStencilView) &&
       (iid_equal(riid, &IID_ID3D10DepthStencilView) || iid_equal(riid, &IID_ID3D10View) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &dsv_vtbl; kind = NPT_D3D10_FACET_DSV;
   }
   if (iid_equal(family, &IID_ID3D11InputLayout) &&
       (iid_equal(riid, &IID_ID3D10InputLayout) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &layout_vtbl; kind = NPT_D3D10_FACET_INPUT_LAYOUT;
   }
   if (iid_equal(family, &IID_ID3D11VertexShader) &&
       (iid_equal(riid, &IID_ID3D10VertexShader) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &vs_vtbl; kind = NPT_D3D10_FACET_VERTEX_SHADER;
   }
   if (iid_equal(family, &IID_ID3D11GeometryShader) &&
       (iid_equal(riid, &IID_ID3D10GeometryShader) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &gs_vtbl; kind = NPT_D3D10_FACET_GEOMETRY_SHADER;
   }
   if (iid_equal(family, &IID_ID3D11PixelShader) &&
       (iid_equal(riid, &IID_ID3D10PixelShader) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &ps_vtbl; kind = NPT_D3D10_FACET_PIXEL_SHADER;
   }
   if (iid_equal(family, &IID_ID3D11BlendState) &&
       (iid_equal(riid, &IID_ID3D10BlendState1) || iid_equal(riid, &IID_ID3D10BlendState) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &blend_vtbl; kind = NPT_D3D10_FACET_BLEND_STATE1;
   }
   if (iid_equal(family, &IID_ID3D11DepthStencilState) &&
       (iid_equal(riid, &IID_ID3D10DepthStencilState) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &depth_vtbl; kind = NPT_D3D10_FACET_DEPTH_STENCIL_STATE;
   }
   if (iid_equal(family, &IID_ID3D11RasterizerState) &&
       (iid_equal(riid, &IID_ID3D10RasterizerState) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &raster_vtbl; kind = NPT_D3D10_FACET_RASTERIZER_STATE;
   }
   if (iid_equal(family, &IID_ID3D11SamplerState) &&
       (iid_equal(riid, &IID_ID3D10SamplerState) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &sampler_vtbl; kind = NPT_D3D10_FACET_SAMPLER_STATE;
   }
   if (iid_equal(family, &IID_ID3D11Query) &&
       (iid_equal(riid, &IID_ID3D10Query) || iid_equal(riid, &IID_ID3D10Asynchronous) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &query_vtbl; kind = NPT_D3D10_FACET_QUERY;
   }
   if (iid_equal(family, &IID_ID3D11Predicate) &&
       (iid_equal(riid, &IID_ID3D10Predicate) || iid_equal(riid, &IID_ID3D10Query) || iid_equal(riid, &IID_ID3D10Asynchronous) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &predicate_vtbl; kind = NPT_D3D10_FACET_PREDICATE;
   }
   if (iid_equal(family, &IID_ID3D11Counter) &&
       (iid_equal(riid, &IID_ID3D10Counter) || iid_equal(riid, &IID_ID3D10Asynchronous) || iid_equal(riid, &IID_ID3D10DeviceChild))) {
      vtbl = &counter_vtbl; kind = NPT_D3D10_FACET_COUNTER;
   }
   if (!vtbl) return E_NOINTERFACE;
   void *facet = npt_com_load_local_d3d10(primary);
   if (!facet) {
      candidate = calloc(1, sizeof(*candidate));
      if (!candidate) return E_OUTOFMEMORY;
      candidate->lpVtbl = vtbl;
      candidate->host_id = npt_com_primary_host_id(primary);
      candidate->owner = primary;
      candidate->kind = kind;
      facet = npt_d3d10_publish_facet(primary, candidate);
      if (!facet) return E_OUTOFMEMORY;
   }
   ((IUnknown *)primary)->lpVtbl->AddRef((IUnknown *)primary);
   *out = facet;
   return S_OK;
}
#else
HRESULT npt_d3d10_state_qi(struct npt_com_base *primary, const GUID *riid, void **out)
{ (void)primary; (void)riid; (void)out; return E_NOINTERFACE; }
#endif
