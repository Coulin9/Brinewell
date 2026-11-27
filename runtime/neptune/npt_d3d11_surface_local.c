#ifdef _WIN32
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#define NPT_PROTOCOL_DIRECTX_H
#endif
#include "npt_d3d11_surface_local.h"

#ifdef _WIN32
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum surface_state { IDLE, MAPPING, MAPPED, ACQUIRING_DC, SURFACE_DC_ACTIVE, RELEASING_DC };
struct npt_local_surface {
   const IDXGISurface2Vtbl *lpVtbl;
   uint64_t host_id;
   struct npt_com_base *primary;
   SRWLOCK lock;
   enum surface_state state;
   HDC dc;
   HBITMAP bitmap;
   HGDIOBJ old_bitmap;
   void *bits;
   UINT width, height, pitch;
};
_Static_assert(offsetof(struct npt_local_surface, host_id) == 8,
               "npt_local_surface: host_id must be at offset 8");

static ID3D11Texture2D *texture(struct npt_local_surface *s)
{
   return (ID3D11Texture2D *)s->primary;
}
static IUnknown *unknown(struct npt_local_surface *s)
{
   return (IUnknown *)s->primary;
}
static bool transition(struct npt_local_surface *s, enum surface_state from,
                       enum surface_state to)
{
   AcquireSRWLockExclusive(&s->lock);
   bool ok = s->state == from;
   if (ok) s->state = to;
   ReleaseSRWLockExclusive(&s->lock);
   return ok;
}
static void set_state(struct npt_local_surface *s, enum surface_state state)
{
   AcquireSRWLockExclusive(&s->lock);
   s->state = state;
   ReleaseSRWLockExclusive(&s->lock);
}
static HRESULT get_context(struct npt_local_surface *s, ID3D11Device **dev,
                           ID3D11DeviceContext **ctx)
{
   *dev = NULL;
   *ctx = NULL;
   ID3D11Texture2D *t = texture(s);
   t->lpVtbl->GetDevice(t, dev);
   if (!*dev) return E_FAIL;
   (*dev)->lpVtbl->GetImmediateContext(*dev, ctx);
   if (!*ctx) { (*dev)->lpVtbl->Release(*dev); *dev = NULL; return E_FAIL; }
   return S_OK;
}
static HRESULT STDMETHODCALLTYPE surface_qi(IDXGISurface2 *self, REFIID iid, void **out)
{
   IUnknown *p = unknown((struct npt_local_surface *)self);
   return p->lpVtbl->QueryInterface(p, iid, out);
}
static ULONG STDMETHODCALLTYPE surface_addref(IDXGISurface2 *self)
{
   IUnknown *p = unknown((struct npt_local_surface *)self);
   return p->lpVtbl->AddRef(p);
}
static ULONG STDMETHODCALLTYPE surface_release(IDXGISurface2 *self)
{
   IUnknown *p = unknown((struct npt_local_surface *)self);
   return p->lpVtbl->Release(p);
}
static HRESULT STDMETHODCALLTYPE surface_set_data(IDXGISurface2 *self, REFGUID name, UINT size, const void *data)
{
   ID3D11Texture2D *t = texture((struct npt_local_surface *)self);
   return t->lpVtbl->SetPrivateData(t, name, size, data);
}
static HRESULT STDMETHODCALLTYPE surface_set_interface(IDXGISurface2 *self, REFGUID name, const IUnknown *value)
{
   ID3D11Texture2D *t = texture((struct npt_local_surface *)self);
   return t->lpVtbl->SetPrivateDataInterface(t, name, value);
}
static HRESULT STDMETHODCALLTYPE surface_get_data(IDXGISurface2 *self, REFGUID name, UINT *size, void *data)
{
   ID3D11Texture2D *t = texture((struct npt_local_surface *)self);
   return t->lpVtbl->GetPrivateData(t, name, size, data);
}
static HRESULT STDMETHODCALLTYPE surface_get_device(IDXGISurface2 *self, REFIID iid, void **out)
{
   if (!out) return E_INVALIDARG;
   *out = NULL;
   ID3D11Texture2D *t = texture((struct npt_local_surface *)self);
   ID3D11Device *dev = NULL;
   t->lpVtbl->GetDevice(t, &dev);
   if (!dev) return E_FAIL;
   HRESULT hr = dev->lpVtbl->QueryInterface(dev, iid, out);
   dev->lpVtbl->Release(dev);
   return hr;
}
static HRESULT STDMETHODCALLTYPE surface_get_desc(IDXGISurface2 *self, DXGI_SURFACE_DESC *out)
{
   if (!out) return E_INVALIDARG;
   ID3D11Texture2D *t = texture((struct npt_local_surface *)self);
   D3D11_TEXTURE2D_DESC d = { 0 };
   t->lpVtbl->GetDesc(t, &d);
   out->Width = d.Width; out->Height = d.Height;
   out->Format = d.Format; out->SampleDesc = d.SampleDesc;
   return S_OK;
}
static HRESULT STDMETHODCALLTYPE surface_map(IDXGISurface2 *self, DXGI_MAPPED_RECT *out, UINT flags)
{
   struct npt_local_surface *s = (struct npt_local_surface *)self;
   if (!out) return E_INVALIDARG;
   memset(out, 0, sizeof(*out));
   if (!flags || (flags & ~(DXGI_MAP_READ | DXGI_MAP_WRITE | DXGI_MAP_DISCARD)) ||
       ((flags & DXGI_MAP_DISCARD) && (!(flags & DXGI_MAP_WRITE) || (flags & DXGI_MAP_READ))))
      return E_INVALIDARG;
   if (!transition(s, IDLE, MAPPING)) return DXGI_ERROR_INVALID_CALL;
   D3D11_TEXTURE2D_DESC d = { 0 };
   ID3D11Texture2D *t = texture(s);
   t->lpVtbl->GetDesc(t, &d);
   D3D11_MAP mode;
   if (flags & DXGI_MAP_DISCARD) mode = D3D11_MAP_WRITE_DISCARD;
   else if (flags == (DXGI_MAP_READ | DXGI_MAP_WRITE)) mode = D3D11_MAP_READ_WRITE;
   else if (flags & DXGI_MAP_READ) mode = D3D11_MAP_READ;
   else mode = D3D11_MAP_WRITE;
   HRESULT hr = E_INVALIDARG;
   if (((flags & DXGI_MAP_READ) && !(d.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) ||
       ((flags & DXGI_MAP_WRITE) && !(d.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) ||
       (mode == D3D11_MAP_WRITE_DISCARD && d.Usage != D3D11_USAGE_DYNAMIC) ||
       (mode != D3D11_MAP_WRITE_DISCARD && d.Usage != D3D11_USAGE_STAGING))
      goto done;
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   hr = get_context(s, &dev, &ctx);
   if (FAILED(hr)) goto done;
   D3D11_MAPPED_SUBRESOURCE mapped = {0};
   hr = ctx->lpVtbl->Map(ctx, (ID3D11Resource *)t, 0, mode, 0, &mapped);
   if (SUCCEEDED(hr)) {
      if (mapped.RowPitch > INT_MAX) {
         ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)t, 0);
         hr = E_FAIL;
      } else {
         out->Pitch = (INT)mapped.RowPitch;
         out->pBits = mapped.pData;
      }
   }
   ctx->lpVtbl->Release(ctx);
   dev->lpVtbl->Release(dev);
done:
   set_state(s, SUCCEEDED(hr) ? MAPPED : IDLE);
   return hr;
}
static HRESULT STDMETHODCALLTYPE surface_unmap(IDXGISurface2 *self)
{
   struct npt_local_surface *s = (struct npt_local_surface *)self;
   if (!transition(s, MAPPED, MAPPING)) return DXGI_ERROR_INVALID_CALL;
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   HRESULT hr = get_context(s, &dev, &ctx);
   if (SUCCEEDED(hr)) {
      ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)texture(s), 0);
      ctx->lpVtbl->Release(ctx);
      dev->lpVtbl->Release(dev);
   }
   set_state(s, IDLE);
   return hr;
}
static HRESULT unbind_target(struct npt_local_surface *s, ID3D11DeviceContext *ctx)
{
   ID3D11RenderTargetView *views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
   ID3D11DepthStencilView *dsv = NULL;
   IUnknown *identity = NULL;
   IUnknown *p = unknown(s);
   HRESULT hr = p->lpVtbl->QueryInterface(p, &NPT_IID_IUnknown, (void **)&identity);
   if (FAILED(hr)) return hr;
   ctx->lpVtbl->OMGetRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, &dsv);
   bool changed = false;
   for (unsigned i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
      if (!views[i]) continue;
      ID3D11Resource *res = NULL;
      IUnknown *other = NULL;
      views[i]->lpVtbl->GetResource(views[i], &res);
      if (res) {
         hr = res->lpVtbl->QueryInterface(res, &NPT_IID_IUnknown, (void **)&other);
         res->lpVtbl->Release(res);
         if (FAILED(hr)) break;
      }
      if (other == identity) {
         views[i]->lpVtbl->Release(views[i]);
         views[i] = NULL;
         changed = true;
      }
      if (other) other->lpVtbl->Release(other);
   }
   if (SUCCEEDED(hr) && changed)
      ctx->lpVtbl->OMSetRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, dsv);
   for (unsigned i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
      if (views[i]) views[i]->lpVtbl->Release(views[i]);
   if (dsv) dsv->lpVtbl->Release(dsv);
   identity->lpVtbl->Release(identity);
   return hr;
}
static void cleanup_dc(HDC dc, HBITMAP bitmap, HGDIOBJ old_bitmap)
{
   if (dc && old_bitmap && old_bitmap != HGDI_ERROR) SelectObject(dc, old_bitmap);
   if (bitmap) DeleteObject(bitmap);
   if (dc) DeleteDC(dc);
}
static HRESULT STDMETHODCALLTYPE surface_get_dc(IDXGISurface2 *self, BOOL discard, HDC *out)
{
   struct npt_local_surface *s = (struct npt_local_surface *)self;
   if (!out) return E_INVALIDARG;
   *out = NULL;
   if (!transition(s, IDLE, ACQUIRING_DC)) return DXGI_ERROR_INVALID_CALL;
   ID3D11Texture2D *t = texture(s), *staging = NULL;
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   HDC dc = NULL;
   HBITMAP bitmap = NULL;
   HGDIOBJ old = NULL;
   void *bits = NULL;
   D3D11_TEXTURE2D_DESC d = { 0 };
   t->lpVtbl->GetDesc(t, &d);
   HRESULT hr = DXGI_ERROR_INVALID_CALL;
   if (d.Usage != D3D11_USAGE_DEFAULT || !(d.BindFlags & D3D11_BIND_RENDER_TARGET) ||
       !(d.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) || d.SampleDesc.Count != 1 ||
       (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
       !d.Width || !d.Height || d.Width > INT_MAX / 4 || d.Height > INT_MAX ||
       (uint64_t)d.Width * 4 * d.Height > UINT32_MAX)
      goto done;
   hr = get_context(s, &dev, &ctx);
   if (FAILED(hr)) goto done;
   hr = unbind_target(s, ctx);
   if (FAILED(hr)) goto done;
   hr = E_OUTOFMEMORY;
   dc = CreateCompatibleDC(NULL);
   if (!dc) goto done;
   BITMAPINFO bmi = {0};
   bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
   bmi.bmiHeader.biWidth = (LONG)d.Width;
   bmi.bmiHeader.biHeight = -(LONG)d.Height;
   bmi.bmiHeader.biPlanes = 1;
   bmi.bmiHeader.biBitCount = 32;
   bmi.bmiHeader.biCompression = BI_RGB;
   bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
   if (!bitmap || !bits) goto done;
   old = SelectObject(dc, bitmap);
   if (!old || old == HGDI_ERROR) goto done;
   if (!discard) {
      D3D11_TEXTURE2D_DESC sd = d;
      sd.MipLevels = sd.ArraySize = 1;
      sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
      sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
      hr = dev->lpVtbl->CreateTexture2D(dev, &sd, NULL, &staging);
      if (FAILED(hr)) goto done;
      ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)t);
      D3D11_MAPPED_SUBRESOURCE mapped = {0};
      hr = ctx->lpVtbl->Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(hr)) goto done;
      if (!mapped.pData || mapped.RowPitch < d.Width * 4) hr = E_FAIL;
      else for (UINT y = 0; y < d.Height; ++y)
         memcpy((uint8_t *)bits + (size_t)y * d.Width * 4,
                (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, d.Width * 4);
      ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)staging, 0);
      if (FAILED(hr)) goto done;
   }
   AcquireSRWLockExclusive(&s->lock);
   s->dc = dc; s->bitmap = bitmap; s->old_bitmap = old; s->bits = bits;
   s->width = d.Width; s->height = d.Height; s->pitch = d.Width * 4;
   s->state = SURFACE_DC_ACTIVE;
   ReleaseSRWLockExclusive(&s->lock);
   *out = dc;
   dc = NULL; bitmap = NULL; old = NULL;
   hr = S_OK;
done:
   if (staging) staging->lpVtbl->Release(staging);
   if (ctx) ctx->lpVtbl->Release(ctx);
   if (dev) dev->lpVtbl->Release(dev);
   cleanup_dc(dc, bitmap, old);
   if (FAILED(hr)) set_state(s, IDLE);
   return hr;
}
static HRESULT STDMETHODCALLTYPE surface_release_dc(IDXGISurface2 *self, RECT *dirty)
{
   struct npt_local_surface *s = (struct npt_local_surface *)self;
   if (!transition(s, SURFACE_DC_ACTIVE, RELEASING_DC)) return DXGI_ERROR_INVALID_CALL;
   GdiFlush();
   LONG l = dirty ? dirty->left : 0, t = dirty ? dirty->top : 0;
   LONG r = dirty ? dirty->right : (LONG)s->width, b = dirty ? dirty->bottom : (LONG)s->height;
   if (l < 0) l = 0;
   if (t < 0) t = 0;
   if (r > (LONG)s->width) r = (LONG)s->width;
   if (b > (LONG)s->height) b = (LONG)s->height;
   HRESULT hr = S_OK;
   if (l < r && t < b) {
      ID3D11Device *dev;
      ID3D11DeviceContext *ctx;
      hr = get_context(s, &dev, &ctx);
      if (SUCCEEDED(hr)) {
         D3D11_BOX box = {(UINT)l, (UINT)t, 0, (UINT)r, (UINT)b, 1};
         const uint8_t *src = (const uint8_t *)s->bits + (size_t)t * s->pitch + (size_t)l * 4;
         ctx->lpVtbl->UpdateSubresource(ctx, (ID3D11Resource *)texture(s), 0, &box, src, s->pitch, 0);
         ctx->lpVtbl->Release(ctx);
         dev->lpVtbl->Release(dev);
      }
   }
   cleanup_dc(s->dc, s->bitmap, s->old_bitmap);
   AcquireSRWLockExclusive(&s->lock);
   s->dc = NULL; s->bitmap = NULL; s->old_bitmap = NULL; s->bits = NULL;
   s->state = IDLE;
   ReleaseSRWLockExclusive(&s->lock);
   return hr;
}
static HRESULT STDMETHODCALLTYPE surface_get_resource(IDXGISurface2 *self, REFIID iid, void **out, UINT *index)
{
   if (out) *out = NULL;
   if (index) *index = 0;
   if (!out || !index) return E_INVALIDARG;
   IUnknown *p = unknown((struct npt_local_surface *)self);
   return p->lpVtbl->QueryInterface(p, iid, out);
}
static const IDXGISurface2Vtbl surface_vtbl = {
   surface_qi, surface_addref, surface_release, surface_set_data,
   surface_set_interface, surface_get_data, surface_get_device, surface_get_device,
   surface_get_desc, surface_map, surface_unmap, surface_get_dc,
   surface_release_dc, surface_get_resource
};
HRESULT npt_d3d11_surface_local_qi(struct npt_com_base *primary, const GUID *iid, void **out)
{
   if (!iid || (memcmp(iid, &NPT_IID_IDXGISurface, sizeof(*iid)) &&
                memcmp(iid, &NPT_IID_IDXGISurface1, sizeof(*iid)) &&
                memcmp(iid, &NPT_IID_IDXGISurface2, sizeof(*iid)))) return E_NOINTERFACE;
   if (!out) return E_POINTER;
   *out = NULL;
   if (!primary || !primary->family || !primary->family->tier_iids ||
       !primary->family->tier_iids[0] ||
       memcmp(primary->family->tier_iids[0], &NPT_IID_ID3D11Texture2D, sizeof(*iid)))
      return E_NOINTERFACE;
   ID3D11Texture2D *t = (ID3D11Texture2D *)primary;
   D3D11_TEXTURE2D_DESC d = { 0 };
   t->lpVtbl->GetDesc(t, &d);
   UINT levels = d.MipLevels;
   if (!levels) {
      UINT size = d.Width > d.Height ? d.Width : d.Height;
      levels = 1;
      while (size > 1) { size >>= 1; ++levels; }
   }
   if (d.ArraySize != 1 || levels != 1) return E_NOINTERFACE;
   struct npt_local_surface *s = atomic_load_explicit(&primary->local_surface, memory_order_acquire);
   if (!s) {
      struct npt_local_surface *candidate = calloc(1, sizeof(*candidate));
      if (!candidate) return E_OUTOFMEMORY;
      candidate->lpVtbl = &surface_vtbl;
      candidate->host_id = primary->base.id;
      candidate->primary = primary;
      InitializeSRWLock(&candidate->lock);
      void *expected = NULL;
      if (!atomic_compare_exchange_strong_explicit(&primary->local_surface, &expected, candidate,
                                                    memory_order_release, memory_order_acquire)) {
         free(candidate);
         s = expected;
      } else s = candidate;
   }
   ((IUnknown *)primary)->lpVtbl->AddRef((IUnknown *)primary);
   *out = s;
   return S_OK;
}
void npt_d3d11_surface_local_destroy(struct npt_com_base *primary)
{
   struct npt_local_surface *s = atomic_exchange_explicit(&primary->local_surface, NULL, memory_order_acq_rel);
   if (!s) return;
   cleanup_dc(s->dc, s->bitmap, s->old_bitmap);
   free(s);
}
bool npt_d3d11_surface_local_has_active_dc(struct npt_com_base *primary)
{
   struct npt_local_surface *s = atomic_load_explicit(&primary->local_surface, memory_order_acquire);
   if (!s) return false;
   AcquireSRWLockShared(&s->lock);
   bool active = s->state == ACQUIRING_DC || s->state == SURFACE_DC_ACTIVE || s->state == RELEASING_DC;
   ReleaseSRWLockShared(&s->lock);
   return active;
}
#else
HRESULT npt_d3d11_surface_local_qi(struct npt_com_base *primary, const GUID *iid, void **out)
{
   (void)primary; (void)iid; (void)out;
   return NPT_E_NOINTERFACE;
}
void npt_d3d11_surface_local_destroy(struct npt_com_base *primary) { (void)primary; }
bool npt_d3d11_surface_local_has_active_dc(struct npt_com_base *primary) { (void)primary; return false; }
#endif
