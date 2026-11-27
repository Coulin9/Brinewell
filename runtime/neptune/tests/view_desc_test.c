/* View GetDesc caching + immutable GetType overrides: first GetDesc
 * pays one RPC, repeats answer from the aux; a reply that leaves
 * ViewDimension UNKNOWN is not cached.  Includes the production TUs and
 * stubs the COM/dispatch boundaries; override functions are called
 * directly so no family wiring runs. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../npt_overrides_d3d11_view.c"
#include "../npt_overrides_d3d11_buffer.c"
#include "../npt_overrides_d3d11_texture.c"
#include "../npt_overrides_d3d11_context.c"
#include "../npt_env.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#pragma GCC diagnostic ignored "-Wunused-parameter"

struct npt_id3d11rendertargetview1_client_vtbl
   npt_id3d11rendertargetview1_default_vtbl_storage;
struct npt_id3d11depthstencilview_client_vtbl
   npt_id3d11depthstencilview_default_vtbl_storage;
struct npt_id3d11shaderresourceview1_client_vtbl
   npt_id3d11shaderresourceview1_default_vtbl_storage;
struct npt_id3d11unorderedaccessview1_client_vtbl
   npt_id3d11unorderedaccessview1_default_vtbl_storage;
struct npt_id3d11buffer_client_vtbl
   npt_id3d11buffer_default_vtbl_storage;
struct npt_id3d11texture1d_client_vtbl
   npt_id3d11texture1d_default_vtbl_storage;
struct npt_id3d11texture2d1_client_vtbl
   npt_id3d11texture2d1_default_vtbl_storage;
struct npt_id3d11texture3d1_client_vtbl
   npt_id3d11texture3d1_default_vtbl_storage;
struct npt_id3d11devicecontext4_client_vtbl
   npt_id3d11devicecontext4_default_vtbl_storage;

struct npt_env npt_env;
void npt_env_init(void) {}
void npt_log_impl(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
}

static struct npt_device device;
static size_t view_aux_size;
static npt_com_aux_init_fn view_aux_init_fn;
static unsigned rpc_count;
static enum { REPLY_FILL, REPLY_EMPTY } reply_mode;

void *npt_com_family_aux(void *self, void (*destroy)(void *))
{
   struct npt_com_base *com = self;
   return com && com->aux_destroy == destroy ? com->aux : NULL;
}
void npt_com_register_family(const GUID *const *iids, size_t size,
                            npt_com_aux_init_fn init)
{
   CHECK(size == sizeof(struct npt_d3d11_view_aux));
   view_aux_size = size;
   view_aux_init_fn = init;
}

/* Fake wire GetDescs: count the round trip; REPLY_EMPTY leaves the
 * pre-zeroed desc (a failed/missing reply). */
void NPT_STDMETHODCALLTYPE
npt_id3d11rendertargetview_default_GetDesc(void *self,
   D3D11_RENDER_TARGET_VIEW_DESC *pDesc)
{
   ++rpc_count;
   if (reply_mode == REPLY_FILL)
      pDesc->ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
}
void NPT_STDMETHODCALLTYPE
npt_id3d11depthstencilview_default_GetDesc(void *self,
   D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc)
{
   ++rpc_count;
   if (reply_mode == REPLY_FILL)
      pDesc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
}
void NPT_STDMETHODCALLTYPE
npt_id3d11shaderresourceview_default_GetDesc(void *self,
   D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc)
{
   ++rpc_count;
   if (reply_mode == REPLY_FILL)
      pDesc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
}
void NPT_STDMETHODCALLTYPE
npt_id3d11unorderedaccessview_default_GetDesc(void *self,
   D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc)
{
   ++rpc_count;
   if (reply_mode == REPLY_FILL)
      pDesc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
}

D3D11_DEVICE_CONTEXT_TYPE NPT_STDMETHODCALLTYPE
npt_id3d11devicecontext_default_GetType(void *self)
{
   ++rpc_count;
   return D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

/* Kept globals of the included TUs reference these; the overrides
 * under test never reach them. */
#define STUB(...) { CHECK(false); }
struct npt_d3d11_buffer *npt_d3d11_buffer_cast(void *resource) STUB(return NULL;)
bool npt_d3d11_buffer_fill_desc(struct npt_d3d11_buffer *b,
                                D3D11_BUFFER_DESC *out) STUB(return false;)
bool npt_d3d11_buffer_ensure_map_shmem(struct npt_d3d11_buffer *b)
   STUB(return false;)
bool npt_d3d11_buffer_external_eligible(const struct npt_d3d11_buffer *b)
   STUB(return false;)
struct npt_d3d11_texture *npt_d3d11_texture_cast(void *resource)
   STUB(return NULL;)
bool npt_d3d11_texture_has_desc(const struct npt_d3d11_texture *t)
   STUB(return false;)
bool npt_d3d11_texture_is_mappable(const struct npt_d3d11_texture *t)
   STUB(return false;)
bool npt_d3d11_texture_ensure_map_shmem(struct npt_d3d11_texture *t)
   STUB(return false;)
void npt_d3d11_texture_fill_desc1d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE1D_DESC *out) STUB()
void npt_d3d11_texture_fill_desc2d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE2D_DESC *out) STUB()
void npt_d3d11_texture_fill_desc2d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE2D_DESC1 *out) STUB()
void npt_d3d11_texture_fill_desc3d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE3D_DESC *out) STUB()
void npt_d3d11_texture_fill_desc3d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE3D_DESC1 *out) STUB()
uint32_t npt_d3d11_texture_get_shmem_size(const struct npt_d3d11_texture *t)
   STUB(return 0;)
uint32_t npt_d3d11_texture_get_slot_size(const struct npt_d3d11_texture *t)
   STUB(return 0;)
uint32_t npt_d3d11_texture_get_cached_row_pitch(const struct npt_d3d11_texture *t)
   STUB(return 0;)
uint32_t npt_d3d11_texture_get_cached_depth_pitch(const struct npt_d3d11_texture *t)
   STUB(return 0;)
uint32_t npt_d3d11_texture_get_subresource_byte_size(
   const struct npt_d3d11_texture *t, uint32_t subresource,
   uint32_t row_pitch, uint32_t depth_pitch) STUB(return 0;)
void npt_d3d11_texture_get_map_extent(const struct npt_d3d11_texture *t,
                                      uint32_t subresource,
                                      uint32_t *out_rows,
                                      uint32_t *out_depth) STUB()
void npt_d3d11_texture_slot_visible(const struct npt_d3d11_texture *t,
                                    uint32_t slot, struct npt_ring *ring)
   STUB()
void *npt_d3d11_texture_slot_ptr(const struct npt_d3d11_texture *t,
                                 uint32_t slot) STUB(return NULL;)
bool npt_d3d11_texture_map_open(struct npt_d3d11_texture *t,
                                const void *context, uint32_t subresource,
                                enum npt_d3d_map_slot_choice choice,
                                uint32_t access_flags, uint32_t byte_size,
                                uint32_t *out_slot) STUB(return false;)
void NPT_STDMETHODCALLTYPE
npt_id3d11buffer_default_GetDesc(void *self, D3D11_BUFFER_DESC *pDesc)
   STUB()
void NPT_STDMETHODCALLTYPE
npt_id3d11texture1d_default_GetDesc(void *self, D3D11_TEXTURE1D_DESC *pDesc)
   STUB()
void NPT_STDMETHODCALLTYPE
npt_id3d11texture2d_default_GetDesc(void *self, D3D11_TEXTURE2D_DESC *pDesc)
   STUB()
void NPT_STDMETHODCALLTYPE
npt_id3d11texture2d1_default_GetDesc1(void *self,
                                      D3D11_TEXTURE2D_DESC1 *pDesc) STUB()
void NPT_STDMETHODCALLTYPE
npt_id3d11texture3d_default_GetDesc(void *self, D3D11_TEXTURE3D_DESC *pDesc)
   STUB()
void NPT_STDMETHODCALLTYPE
npt_id3d11texture3d1_default_GetDesc1(void *self,
                                      D3D11_TEXTURE3D_DESC1 *pDesc) STUB()
struct npt_d3d11_query_aux *npt_d3d11_query_aux_cast(void *async)
   STUB(return NULL;)
void NPT_STDMETHODCALLTYPE
npt_id3d11devicecontext_default_End(void *self, ID3D11Asynchronous *pAsync)
   STUB()
HRESULT NPT_STDMETHODCALLTYPE
npt_id3d11devicecontext_default_GetData(void *self,
   ID3D11Asynchronous *pAsync, void *pData, UINT DataSize,
   UINT GetDataFlags) STUB(return E_FAIL;)

struct npt_ring *npt_tls_get_ring(struct npt_device *dev)
   STUB(return NULL;)
struct npt_ring *npt_device_get_pipeline_library_ring(struct npt_device *dev)
   STUB(return NULL;)
uint32_t npt_ring_stage_self_index(struct npt_ring *ring)
   STUB(return 0;)
void npt_ring_stage_flush_index(struct npt_ring *ring, uint32_t index)
   STUB()
bool npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t id,
                             uint32_t *tail) STUB(return false;)
void npt_ring_order_after(struct npt_ring *ring, uint64_t id,
                          uint32_t seqno) STUB()
struct npt_cs_encoder *
npt_ring_submit_command_init(struct npt_ring *ring,
   struct npt_ring_submit_command *submit, void *data, size_t size,
   size_t reply_size)
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
                             struct npt_ring_submit_command *submit) STUB()

static struct npt_com_base *
view_wrapper(void)
{
   struct npt_com_base *com = calloc(1, sizeof(*com));
   com->aux = calloc(1, view_aux_size);
   com->base.device = &device;
   view_aux_init_fn(com, &device, 1);
   return com;
}

static void
test_rtv_cache(void)
{
   struct npt_com_base *rtv = view_wrapper();
   reply_mode = REPLY_FILL;
   rpc_count = 0;

   D3D11_RENDER_TARGET_VIEW_DESC d1 = {0}, d2 = {0};
   rtv_GetDesc_override(rtv, &d1);
   CHECK(rpc_count == 1);
   CHECK(d1.ViewDimension == D3D11_RTV_DIMENSION_BUFFER);
   rtv_GetDesc_override(rtv, &d2);
   CHECK(rpc_count == 1);
   CHECK(memcmp(&d1, &d2, sizeof(d1)) == 0);
   free(rtv->aux); free(rtv);
   fprintf(stderr, "ok rtv_cache\n");
}

static void
test_dsv_cache_and_empty_reply(void)
{
   struct npt_com_base *dsv = view_wrapper();
   reply_mode = REPLY_FILL;
   rpc_count = 0;

   D3D11_DEPTH_STENCIL_VIEW_DESC d = {0};
   dsv_GetDesc_override(dsv, &d);
   CHECK(rpc_count == 1);
   CHECK(d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D);
   memset(&d, 0, sizeof(d));
   dsv_GetDesc_override(dsv, &d);
   CHECK(rpc_count == 1);
   CHECK(d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D);
   free(dsv->aux); free(dsv);

   /* A failed reply (ViewDimension stays UNKNOWN) is not cached. */
   struct npt_com_base *dsv2 = view_wrapper();
   reply_mode = REPLY_EMPTY;
   rpc_count = 0;
   dsv_GetDesc_override(dsv2, &d);
   dsv_GetDesc_override(dsv2, &d);
   CHECK(rpc_count == 2);
   free(dsv2->aux); free(dsv2);
   fprintf(stderr, "ok dsv_cache_and_empty_reply\n");
}

static void
test_gettype_overrides(void)
{
   rpc_count = 0;
   D3D11_RESOURCE_DIMENSION dim = 0;
   buf_GetType_override(NULL, &dim);
   CHECK(dim == D3D11_RESOURCE_DIMENSION_BUFFER);
   tex1d_GetType_override(NULL, &dim);
   CHECK(dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D);
   tex2d_GetType_override(NULL, &dim);
   CHECK(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D);
   tex3d_GetType_override(NULL, &dim);
   CHECK(dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D);
   buf_GetType_override(NULL, NULL);   /* NULL out-param: no crash */

   /* Deferred-tagged context answers DEFERRED; untagged falls back to
    * the wire. */
   struct npt_com_base *ctx = calloc(1, sizeof(*ctx));
   ctx->aux = calloc(1, sizeof(struct npt_d3d11_context_aux));
   ctx->aux_destroy = npt_d3d11_context_aux_destroy;
   CHECK(ctx_GetType_override(ctx) == D3D11_DEVICE_CONTEXT_IMMEDIATE);
   npt_d3d11_context_set_deferred(ctx);
   CHECK(ctx_GetType_override(ctx) == D3D11_DEVICE_CONTEXT_DEFERRED);
   free(ctx->aux); free(ctx);

   struct npt_com_base *plain = calloc(1, sizeof(*plain));
   CHECK(ctx_GetType_override(plain) == D3D11_DEVICE_CONTEXT_IMMEDIATE);
   free(plain);
   CHECK(rpc_count == 1);   /* only the untagged fallback went out */
   fprintf(stderr, "ok gettype_overrides\n");
}

int
main(void)
{
   npt_overrides_d3d11_view_init();
   CHECK(view_aux_init_fn != NULL);

   test_rtv_cache();
   test_dsv_cache_and_empty_reply();
   test_gettype_overrides();
   fprintf(stderr, "PASS view_desc\n");
   return 0;
}
