/* Back-buffer public-format table + the overrides that consume it:
 * add/lookup by host id, NULL / UNKNOWN view-desc materialization, UAV
 * explicit-format rejection, and the GetDesc Format patches on a
 * wrapper minted after the original died.  Includes the production TUs
 * and stubs the wire/default-thunk boundary; override functions are
 * called directly so no family wiring runs. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../npt_device.c"
#include "../npt_overrides_d3d11_device.c"
#include "../npt_overrides_d3d11_texture.c"
#include "../npt_overrides_dxgi_surface.c"
#include "../npt_env.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#pragma GCC diagnostic ignored "-Wunused-parameter"

struct npt_env npt_env;
void npt_env_init(void) {}
void npt_log_impl(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
}

/* The override init functions patch these storages in place; the test
 * calls the override functions directly instead. */
struct npt_id3d11device5_client_vtbl
   npt_id3d11device5_default_vtbl_storage;
struct npt_id3d11texture1d_client_vtbl
   npt_id3d11texture1d_default_vtbl_storage;
struct npt_idxgisurface2_client_vtbl
   npt_idxgisurface2_default_vtbl_storage;

/* Kept functions of the included TUs reference these; the paths under
 * test never reach them (the real ring/renderer/unixlib calls all sit
 * behind the stubbed default thunks and the fabricated device). */
#define STUB(...) { CHECK(false); }
int npt_unixlib_ensure_init(void) { return 0; }
NTSTATUS npt_wine_unix_call(unsigned int code, void *args)
   STUB(return 0;)
void npt_event_init(struct npt_device *dev) STUB()
void npt_event_fini(struct npt_device *dev) STUB()
struct npt_renderer *npt_renderer_create_vtest(void)
   STUB(return NULL;)
struct npt_renderer *npt_renderer_create_virtgpu(void)
   STUB(return NULL;)
void npt_renderer_shmem_reaper_drain(struct npt_renderer *renderer) STUB()
void npt_ring_get_layout(size_t buf_size, size_t extra_size,
                         struct npt_ring_layout *layout) STUB()
struct npt_ring *npt_ring_create(struct npt_device *device,
                                 const struct npt_ring_layout *layout,
                                 uint64_t ring_id, bool is_tls_ring)
   STUB(return NULL;)
void npt_ring_destroy(struct npt_ring *ring) STUB()
bool npt_ring_seqno_passed(const struct npt_ring *ring, uint32_t seqno)
   STUB(return false;)
void npt_ring_order_after_ring(struct npt_ring *ring,
                               const struct npt_ring *target,
                               uint32_t target_seqno) STUB()
void npt_ring_stage_flush_all(struct npt_ring *ring) STUB()
struct npt_cs_decoder *
npt_ring_get_command_reply(struct npt_ring *ring,
                           struct npt_ring_submit_command *submit)
   STUB(return NULL;)
void npt_ring_free_command_reply(struct npt_ring *ring,
                                 struct npt_ring_submit_command *submit)
   STUB()
void npt_shmem_pool_init(struct npt_shmem_pool *pool, size_t min) STUB()
void npt_shmem_pool_fini(struct npt_renderer *renderer,
                         struct npt_shmem_pool *pool) STUB()
struct npt_renderer_shmem *
npt_shmem_pool_alloc(struct npt_renderer *renderer,
                     struct npt_shmem_pool *pool,
                     size_t size, size_t *out_offset)
   STUB(return NULL;)
void npt_tls_once_init(void) STUB()
void npt_tls_destroy_ring(struct npt_tls_ring *tr) STUB()
uint32_t NPT_STDMETHODCALLTYPE npt_com_default_release(void *self)
   STUB(return 0;)
void *npt_com_get_or_wrap_or_release(struct npt_device *dev,
                                     const GUID *iid, uint64_t host_id,
                                     struct npt_com_base *parent)
   STUB(return NULL;)
void npt_com_register_family(const GUID *const *tier_iids, size_t aux_size,
                             npt_com_aux_init_fn aux_init) STUB()
void npt_d3d11_buffer_set_desc(struct npt_d3d11_buffer *b,
                               const D3D11_BUFFER_DESC *desc) STUB()
void npt_d3d11_buffer_set_external_eligible(struct npt_d3d11_buffer *b)
   STUB()
void npt_d3d11_context_set_deferred(void *context) STUB()
void npt_d3d11_texture_aux_init(struct npt_com_base *com,
                                struct npt_device *dev, uint64_t host_id)
   STUB()
uint32_t npt_d3d11_texture_desc_mip_levels(
   const struct npt_d3d11_texture_desc *d) STUB(return 0;)
uint32_t npt_dxgi_format_block_rows(DXGI_FORMAT fmt, uint32_t rows)
   STUB(return 0;)
uint32_t npt_dxgi_format_row_bytes(DXGI_FORMAT fmt, uint32_t texels)
   STUB(return 0;)
uint32_t npt_dxgi_format_row_texels(DXGI_FORMAT fmt)
   STUB(return 0;)
uint32_t npt_dxgi_format_subresource_rows(DXGI_FORMAT fmt, uint32_t h)
   STUB(return 0;)
bool npt_resource_update(struct npt_ring *ring, uint64_t context_id,
                         uint64_t resource_id, uint32_t subresource,
                         const D3D11_BOX *box, uint32_t row_pitch,
                         uint32_t depth_pitch, const void *data,
                         uint64_t full, uint64_t copy,
                         const struct npt_update_split *split)
   STUB(return false;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device_default_CreateDeferredContext(
   void *self, UINT flags, ID3D11DeviceContext **out)
   STUB(return NPT_E_FAIL;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device1_default_CreateDeferredContext1(
   void *self, UINT flags, ID3D11DeviceContext1 **out)
   STUB(return NPT_E_FAIL;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device2_default_CreateDeferredContext2(
   void *self, UINT flags, ID3D11DeviceContext2 **out)
   STUB(return NPT_E_FAIL;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device3_default_CreateDeferredContext3(
   void *self, UINT flags, ID3D11DeviceContext3 **out)
   STUB(return NPT_E_FAIL;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device_default_GetDeviceRemovedReason(void *self)
   STUB(return NPT_E_FAIL;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device3_default_CreateRenderTargetView1(
   void *self, ID3D11Resource *res,
   const D3D11_RENDER_TARGET_VIEW_DESC1 *d,
   ID3D11RenderTargetView1 **out) STUB(return NPT_E_FAIL;)
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device3_default_CreateShaderResourceView1(
   void *self, ID3D11Resource *res,
   const D3D11_SHADER_RESOURCE_VIEW_DESC1 *d,
   ID3D11ShaderResourceView1 **out) STUB(return NPT_E_FAIL;)
void NPT_STDMETHODCALLTYPE
npt_id3d11texture1d_default_GetDesc(void *self,
                                    D3D11_TEXTURE1D_DESC *d) STUB()
struct npt_cs_encoder *
npt_ring_submit_command_init(struct npt_ring *ring,
                             struct npt_ring_submit_command *submit,
                             void *data, size_t size, size_t reply_size)
{
   memset(submit, 0, sizeof(*submit));
   submit->cmd_data = data;
   submit->cmd_size = size;
   submit->reply_size = reply_size;
   submit->enc.cur = data;
   submit->enc.end = (uint8_t *)data + size;
   return &submit->enc;
}
void npt_ring_submit_command(struct npt_ring *ring,
                             struct npt_ring_submit_command *submit)
   STUB()
uint64_t npt_com_allocate_next_id(void)
{
   static uint64_t next = 0x40000;
   return next++;
}

/* The view overrides plant a memo hold so the resource wrapper -- and
 * the recorded host id -- outlives the app's texture pointer. */
static void *memo_store_self, *memo_store_wrapper;
static unsigned memo_store_calls;
void npt_com_memo_store(void *self, uint32_t key, const GUID *riid,
                        void *wrapper)
{
   memo_store_calls++;
   memo_store_self = self;
   memo_store_wrapper = wrapper;
}

#define DEV_ID 1ull
#define BB_ID  0x1000ull
#define BB_FMT DXGI_FORMAT_B8G8R8A8_UNORM
#define BB_TYPELESS_FMT DXGI_FORMAT_B8G8R8A8_TYPELESS

static struct npt_device device;
static struct npt_com_base dev_wrapper;
static struct npt_com_base bb_wrapper;
static struct npt_com_base plain_wrapper;
static struct npt_com_base surface_wrapper;
static struct npt_com_base late_wrapper;

static void
init_wrapper(struct npt_com_base *com, uint64_t id)
{
   memset(com, 0, sizeof(*com));
   com->base.id = id;
   com->base.device = &device;
}

/* Fake wire answers: capture the view desc the override forwarded. */
static D3D11_RENDER_TARGET_VIEW_DESC last_rtv;
static D3D11_SHADER_RESOURCE_VIEW_DESC last_srv;
static D3D11_UNORDERED_ACCESS_VIEW_DESC last_uav;
static D3D11_UNORDERED_ACCESS_VIEW_DESC1 last_uav1;
static bool rtv_called, srv_called, uav_called, uav1_called;

HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device_default_CreateRenderTargetView(
   void *self, ID3D11Resource *pResource,
   const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
   ID3D11RenderTargetView **ppRTView)
{
   rtv_called = true;
   if (pDesc) last_rtv = *pDesc;
   if (ppRTView) *ppRTView = (ID3D11RenderTargetView *)0x1;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device_default_CreateShaderResourceView(
   void *self, ID3D11Resource *pResource,
   const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
   ID3D11ShaderResourceView **ppSRView)
{
   srv_called = true;
   if (pDesc) last_srv = *pDesc;
   if (ppSRView) *ppSRView = (ID3D11ShaderResourceView *)0x1;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device_default_CreateUnorderedAccessView(
   void *self, ID3D11Resource *pResource,
   const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
   ID3D11UnorderedAccessView **ppUAView)
{
   uav_called = true;
   if (pDesc) last_uav = *pDesc;
   if (ppUAView) *ppUAView = (ID3D11UnorderedAccessView *)0x1;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11device3_default_CreateUnorderedAccessView1(
   void *self, ID3D11Resource *pResource,
   const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc1,
   ID3D11UnorderedAccessView1 **ppUAView1)
{
   uav1_called = true;
   if (pDesc1) last_uav1 = *pDesc1;
   if (ppUAView1) *ppUAView1 = (ID3D11UnorderedAccessView1 *)0x1;
   return NPT_S_OK;
}

/* Host-side GetDesc: reports the typeless twin's format. */
void NPT_STDMETHODCALLTYPE
npt_id3d11texture2d_default_GetDesc(void *self, D3D11_TEXTURE2D_DESC *pDesc)
{
   memset(pDesc, 0, sizeof(*pDesc));
   pDesc->Format = BB_TYPELESS_FMT;
}

void NPT_STDMETHODCALLTYPE
npt_id3d11texture2d1_default_GetDesc1(void *self,
                                      D3D11_TEXTURE2D_DESC1 *pDesc)
{
   memset(pDesc, 0, sizeof(*pDesc));
   pDesc->Format = BB_TYPELESS_FMT;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_idxgisurface_default_GetDesc(void *self, DXGI_SURFACE_DESC *pDesc)
{
   memset(pDesc, 0, sizeof(*pDesc));
   pDesc->Format = BB_TYPELESS_FMT;
   return NPT_S_OK;
}

/* Cached-desc texture aux: not populated in this fixture. */
struct npt_d3d11_texture *npt_d3d11_texture_cast(void *resource)
{
   return NULL;
}
bool npt_d3d11_texture_has_desc(const struct npt_d3d11_texture *t)
{
   return false;
}
void npt_d3d11_texture_fill_desc1d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE1D_DESC *out) { CHECK(false); }
void npt_d3d11_texture_fill_desc2d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE2D_DESC *out) { CHECK(false); }
void npt_d3d11_texture_fill_desc2d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE2D_DESC1 *out) { CHECK(false); }
void npt_d3d11_texture_fill_desc3d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE3D_DESC *out) { CHECK(false); }
void npt_d3d11_texture_fill_desc3d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE3D_DESC1 *out) { CHECK(false); }

int
main(void)
{
   memset(&device, 0, sizeof(device));
   mtx_init(&device.bb_format_mutex, mtx_plain);
   list_inithead(&device.bb_formats);
   init_wrapper(&dev_wrapper, DEV_ID);
   init_wrapper(&bb_wrapper, BB_ID);
   init_wrapper(&plain_wrapper, 0x2000);
   init_wrapper(&surface_wrapper, 0x3000);
   surface_wrapper.identity = &bb_wrapper;
   init_wrapper(&late_wrapper, 0x5000);
   late_wrapper.identity = &bb_wrapper;
   ID3D11Resource *bb_res = (ID3D11Resource *)&bb_wrapper;
   ID3D11Resource *plain_res = (ID3D11Resource *)&plain_wrapper;

   /* add / lookup */
   CHECK(npt_device_backbuffer_format(&device, BB_ID) ==
         DXGI_FORMAT_UNKNOWN);
   npt_device_backbuffer_format_add(&device, BB_ID, BB_FMT);
   CHECK(npt_device_backbuffer_format(&device, BB_ID) == BB_FMT);
   CHECK(npt_device_backbuffer_format(&device, 0xdead) ==
         DXGI_FORMAT_UNKNOWN);
   fprintf(stderr, "ok table_add_lookup\n");

   /* NULL RTV desc materializes the public format. */
   ID3D11RenderTargetView *rtv = NULL;
   CHECK(dev_CreateRenderTargetView_override(&dev_wrapper, bb_res, NULL,
                                             &rtv) == NPT_S_OK);
   CHECK(rtv_called && rtv != NULL);
   CHECK(last_rtv.Format == BB_FMT);
   CHECK(last_rtv.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D);
   /* The view must hold the resource wrapper so the recorded host id
    * stays registered while any view of the buffer lives. */
   CHECK(memo_store_calls == 1 && memo_store_self == (void *)rtv &&
         memo_store_wrapper == (void *)bb_res);

   /* UNKNOWN RTV desc format materializes; explicit formats pass
    * through unchanged (the sRGB cast is the point). */
   rtv_called = false;
   D3D11_RENDER_TARGET_VIEW_DESC rd = { 0 };
   rd.Format = DXGI_FORMAT_UNKNOWN;
   rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
   CHECK(dev_CreateRenderTargetView_override(&dev_wrapper, bb_res, &rd,
                                             &rtv) == NPT_S_OK);
   CHECK(last_rtv.Format == BB_FMT);
   rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
   CHECK(dev_CreateRenderTargetView_override(&dev_wrapper, bb_res, &rd,
                                             &rtv) == NPT_S_OK);
   CHECK(last_rtv.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

   /* Non-back-buffer resources pass through untouched. */
   rtv_called = false;
   memset(&last_rtv, 0, sizeof(last_rtv));
   CHECK(dev_CreateRenderTargetView_override(&dev_wrapper, plain_res, NULL,
                                             &rtv) == NPT_S_OK);
   CHECK(rtv_called && last_rtv.Format == DXGI_FORMAT_UNKNOWN);
   /* A view on an ordinary resource plants no hold. */
   CHECK(memo_store_calls == 3);

   /* NULL SRV desc materializes. */
   ID3D11ShaderResourceView *srv = NULL;
   CHECK(dev_CreateShaderResourceView_override(&dev_wrapper, bb_res, NULL,
                                               &srv) == NPT_S_OK);
   CHECK(srv_called && srv != NULL);
   CHECK(last_srv.Format == BB_FMT);
   CHECK(last_srv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
   fprintf(stderr, "ok view_materialize\n");

   /* UAV: UNKNOWN materializes; public format passes; any other
    * explicit format is rejected locally without submitting. */
   ID3D11UnorderedAccessView *uav = (ID3D11UnorderedAccessView *)0xdead;
   D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { 0 };
   ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
   uav_called = false;
   CHECK(dev_CreateUnorderedAccessView_override(&dev_wrapper, bb_res, &ud,
                                                &uav) == NPT_E_INVALIDARG);
   CHECK(!uav_called && uav == NULL);

   ud.Format = DXGI_FORMAT_UNKNOWN;
   uav = NULL;
   CHECK(dev_CreateUnorderedAccessView_override(&dev_wrapper, bb_res, &ud,
                                                &uav) == NPT_S_OK);
   CHECK(uav_called && last_uav.Format == BB_FMT);

   ud.Format = BB_FMT;
   uav = NULL;
   CHECK(dev_CreateUnorderedAccessView_override(&dev_wrapper, bb_res, &ud,
                                                &uav) == NPT_S_OK);
   CHECK(uav_called && last_uav.Format == BB_FMT);

   /* Same rules on CreateUnorderedAccessView1. */
   ID3D11UnorderedAccessView1 *uav1 = (ID3D11UnorderedAccessView1 *)0xdead;
   D3D11_UNORDERED_ACCESS_VIEW_DESC1 ud1 = { 0 };
   ud1.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
   ud1.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
   uav1_called = false;
   CHECK(dev3_CreateUnorderedAccessView1_override(
            &dev_wrapper, bb_res, &ud1, &uav1) == NPT_E_INVALIDARG);
   CHECK(!uav1_called && uav1 == NULL);
   ud1.Format = DXGI_FORMAT_UNKNOWN;
   uav1 = NULL;
   CHECK(dev3_CreateUnorderedAccessView1_override(
            &dev_wrapper, bb_res, &ud1, &uav1) == NPT_S_OK);
   CHECK(uav1_called && last_uav1.Format == BB_FMT);
   fprintf(stderr, "ok uav_reject\n");

   /* GetDesc on a fresh back-buffer wrapper (no cached desc) falls
    * back to the host's typeless answer and is patched to public. */
   D3D11_TEXTURE2D_DESC td;
   tex2d_GetDesc_override(&bb_wrapper, &td);
   CHECK(td.Format == BB_FMT);
   D3D11_TEXTURE2D_DESC1 td1;
   tex2d1_GetDesc1_override(&bb_wrapper, &td1);
   CHECK(td1.Format == BB_FMT);
   tex2d_GetDesc_override(&plain_wrapper, &td);
   CHECK(td.Format == BB_TYPELESS_FMT);

   /* The late path: view->GetResource hands back the object the view
    * kept alive, and a wrapper whose own id is a secondary
    * registration (cross-family QI) resolves the public format
    * through com->identity -- the primary still keys the table. */
   tex2d_GetDesc_override(&late_wrapper, &td);
   CHECK(td.Format == BB_FMT);
   tex2d1_GetDesc1_override(&late_wrapper, &td1);
   CHECK(td1.Format == BB_FMT);

   /* IDXGISurface::GetDesc on the surface-family view of the object. */
   DXGI_SURFACE_DESC sd;
   CHECK(surface_GetDesc_override(&surface_wrapper, &sd) == NPT_S_OK);
   CHECK(sd.Format == BB_FMT);
   fprintf(stderr, "ok getdesc_patch\n");

   fprintf(stderr, "PASS backbuffer_format\n");
   return 0;
}
