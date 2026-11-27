#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "../npt_overrides_d3d12_list.c"
#include "../npt_env.h"
#include "neptune-protocol/npt_protocol_client_id3d12commandlist.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#pragma GCC diagnostic ignored "-Wunused-parameter"

struct npt_id3d12graphicscommandlist10_client_vtbl
   npt_id3d12graphicscommandlist10_default_vtbl_storage;
struct npt_id3d12commandallocator_client_vtbl
   npt_id3d12commandallocator_default_vtbl_storage;
struct npt_id3d12device14_client_vtbl
   npt_id3d12device14_default_vtbl_storage;

static const GUID *const ancestors[] = { &NPT_IID_ID3D12CommandList, NULL };
static const struct npt_com_family family = {
   .index = NPT_IFACE_FAMILY_ID3D12GRAPHICSCOMMANDLIST,
   .tier_iids = list12_tiers,
   .tier_count = 11,
   .ancestor_iids = ancestors,
};
static struct npt_device device;
static struct npt_ring rings[3];
static struct npt_ring *current_ring;
static uint32_t stage_index = 1;
static unsigned requests, frees, outstanding, family_registrations;
static unsigned close_requests, reset_requests;
static enum { REPLY_VALID, REPLY_NONE, REPLY_WRONG, REPLY_SHORT, REPLY_ZERO } reply_mode;
static struct npt_reply_header reply;
static D3D12_COMMAND_LIST_TYPE host_type;
static struct { uint64_t ring, predecessor; uint32_t seqno; } edges[16];
static unsigned edge_count, flush_count;
static uint32_t flushed_index;
struct npt_env npt_env;

void npt_env_init(void) {}
void npt_log_impl(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
}

void *npt_com_family_aux(void *self, void (*destroy)(void *))
{
   struct npt_com_base *com = self;
   return com && com->aux_destroy == destroy ? com->aux : NULL;
}
uint32_t NPT_STDMETHODCALLTYPE npt_com_default_addref(void *self)
{ CHECK(false); return 0; }
uint32_t NPT_STDMETHODCALLTYPE npt_com_default_release(void *self)
{ CHECK(false); return 0; }
void npt_com_register_family(const GUID *const *iids, size_t size,
                            npt_com_aux_init_fn init)
{
   if (iids == list12_tiers) {
      CHECK(size == sizeof(struct npt_d3d12_list_aux));
      CHECK(init == npt_d3d12_list_aux_init);
   } else {
      CHECK(iids == alloc12_tiers);
   }
   ++family_registrations;
}

struct npt_ring *npt_tls_get_ring(struct npt_device *dev) { return current_ring; }
struct npt_ring *npt_device_get_pipeline_library_ring(struct npt_device *dev)
{ CHECK(false); return NULL; }
uint32_t npt_ring_stage_self_index(struct npt_ring *ring) { return stage_index; }
void npt_ring_stage_flush_index(struct npt_ring *ring, uint32_t index)
{ ++flush_count; flushed_index = index; }
bool npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t id, uint32_t *tail)
{ *tail = 42; return true; }
void npt_ring_order_after(struct npt_ring *ring, uint64_t id, uint32_t seqno)
{
   CHECK(edge_count < 16);
   edges[edge_count].ring = ring->id;
   edges[edge_count].predecessor = id;
   edges[edge_count++].seqno = seqno;
}
void npt_ring_order_after_all(struct npt_ring *ring) { CHECK(false); }
void npt_ring_order_after_edges(struct npt_ring *ring,
   const struct npt_ring_edge *order, uint32_t count)
{
   for (uint32_t i = 0; i < count; ++i)
      npt_ring_order_after(ring, order[i].ring_id, order[i].seqno);
}

struct npt_cs_encoder *
npt_ring_submit_command_init(struct npt_ring *ring,
   struct npt_ring_submit_command *submit, void *data, size_t size, size_t reply_size)
{
   memset(submit, 0, sizeof(*submit));
   submit->cmd_data = data;
   submit->cmd_size = size;
   submit->reply_size = reply_size;
   submit->enc.cur = data;
   submit->enc.end = (uint8_t *)data + size;
   return &submit->enc;
}
void npt_ring_submit_command(struct npt_ring *ring, struct npt_ring_submit_command *submit)
{
   struct npt_command_header header;
   memcpy(&header, submit->cmd_data, sizeof(header));
   if (header.cmd_type == NPT_CMD_TYPE(255, NPT_IFACE_ID_ID3D12CommandList,
                                      NPT_METHOD_ID3D12CommandList_GetType)) {
      CHECK(header.cmd_flags == NPT_CMD_FLAG_REPLY);
      CHECK(submit->reply_size == sizeof(reply));
      ++requests;
      ++outstanding;
   } else {
      CHECK(header.cmd_type == NPT_CMD_TYPE(255, NPT_IFACE_ID_ID3D12GraphicsCommandList,
                                            NPT_METHOD_ID3D12GraphicsCommandList_Close));
      CHECK(header.cmd_flags == 0);
      ++close_requests;
   }
   submit->seqno = ++ring->cur;
}
struct npt_cs_decoder *
npt_ring_get_command_reply(struct npt_ring *ring, struct npt_ring_submit_command *submit)
{
   if (reply_mode == REPLY_NONE)
      return NULL;
   reply.cmd_type = NPT_CMD_TYPE(255, NPT_IFACE_ID_ID3D12CommandList,
                                 NPT_METHOD_ID3D12CommandList_GetType);
   reply.cmd_return = (uint32_t)host_type;
   if (reply_mode == REPLY_WRONG) ++reply.cmd_type;
   if (reply_mode == REPLY_ZERO) memset(&reply, 0, sizeof(reply));
   submit->dec.cur = (const uint8_t *)&reply;
   submit->dec.end = submit->dec.cur + sizeof(reply) - (reply_mode == REPLY_SHORT);
   return &submit->dec;
}
void npt_ring_free_command_reply(struct npt_ring *ring, struct npt_ring_submit_command *submit)
{ CHECK(outstanding == 1); --outstanding; ++frees; }

HRESULT NPT_STDMETHODCALLTYPE npt_id3d12commandallocator_default_Reset(void *self)
{ CHECK(false); return NPT_E_FAIL; }
HRESULT NPT_STDMETHODCALLTYPE npt_id3d12device_default_CreateCommandList(
   void *self, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator,
   ID3D12PipelineState *state, REFIID iid, void **out)
{ CHECK(false); return NPT_E_FAIL; }
HRESULT NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_Reset(
   void *self, ID3D12CommandAllocator *allocator, ID3D12PipelineState *state)
{ CHECK(npt_com_self_ring(self) == current_ring); ++reset_requests; return NPT_S_OK; }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_ExecuteBundle(
   void *self, ID3D12GraphicsCommandList *bundle) { CHECK(false); }
void npt_d3d12_desc_order_read(struct npt_ring *ring, uint64_t handle, uint32_t count)
{ CHECK(false); }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_OMSetRenderTargets(
   void *self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *handles, BOOL single,
   const D3D12_CPU_DESCRIPTOR_HANDLE *depth) { CHECK(false); }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_ClearRenderTargetView(
   void *self, D3D12_CPU_DESCRIPTOR_HANDLE handle, const FLOAT *color,
   UINT count, const D3D12_RECT *rects) { CHECK(false); }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_ClearDepthStencilView(
   void *self, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
   FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT *rects) { CHECK(false); }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_ClearUnorderedAccessViewUint(
   void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
   ID3D12Resource *resource, const UINT *values, UINT count, const D3D12_RECT *rects)
{ CHECK(false); }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist_default_ClearUnorderedAccessViewFloat(
   void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
   ID3D12Resource *resource, const FLOAT *values, UINT count, const D3D12_RECT *rects)
{ CHECK(false); }
void NPT_STDMETHODCALLTYPE npt_id3d12graphicscommandlist4_default_BeginRenderPass(
   void *self, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets,
   const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth, D3D12_RENDER_PASS_FLAGS flags)
{ CHECK(false); }

static struct npt_com_base *new_list(void)
{
   struct npt_com_base *com = calloc(1, sizeof(*com));
   CHECK(com);
   npt_com_base_init(com, (const void **)&npt_id3d12graphicscommandlist_default_vtbl_storage,
                      &device, 123);
   com->family = &family;
   atomic_store(&com->tier, 10);
   com->aux = calloc(1, sizeof(struct npt_d3d12_list_aux));
   CHECK(com->aux);
   npt_d3d12_list_aux_init(com, &device, 123);
   return com;
}
static void destroy_list(struct npt_com_base *com)
{ com->aux_destroy(com->aux); free(com); }
static D3D12_COMMAND_LIST_TYPE get_type(void *self)
{
   struct npt_com_base *com = self;
   const struct npt_id3d12commandlist_client_vtbl *vtbl = (const void *)com->lpVtbl;
   return vtbl->GetType(self);
}

static void test_types_and_family(void)
{
   puts("CASE types-reset-family-vtable");
   for (int type = -1; type <= 6; ++type) {
      struct npt_com_base *com = new_list();
      unsigned before = requests;
      host_type = (D3D12_COMMAND_LIST_TYPE)type;
      CHECK(get_type(com) == host_type);
      CHECK(requests == before + 1);
      CHECK(get_type(com) == host_type);
      CHECK(npt_id3d12graphicscommandlist_default_vtbl_storage.Reset(com, NULL, NULL) == NPT_S_OK);
      CHECK(get_type(com) == host_type);
      CHECK(requests == before + 1);
      destroy_list(com);
   }
   CHECK(reset_requests == 8);
}

static void test_failed_reply(void)
{
   puts("CASE failed-reply-retry");
   for (int mode = REPLY_NONE; mode <= REPLY_ZERO; ++mode) {
      struct npt_com_base *com = new_list();
      unsigned before = requests;
      reply_mode = mode;
      host_type = D3D12_COMMAND_LIST_TYPE_COPY;
      CHECK(get_type(com) == D3D12_COMMAND_LIST_TYPE_DIRECT);
      CHECK(get_type(com) == D3D12_COMMAND_LIST_TYPE_DIRECT);
      CHECK(requests == before + 2 && frees == requests && outstanding == 0);
      reply_mode = REPLY_VALID;
      CHECK(get_type(com) == D3D12_COMMAND_LIST_TYPE_COPY);
      CHECK(get_type(com) == D3D12_COMMAND_LIST_TYPE_COPY);
      CHECK(requests == before + 3);
      destroy_list(com);
   }
}

static void test_missing_aux(void)
{
   puts("CASE missing-aux");
   struct npt_com_base *com = new_list();
   void *aux = com->aux;
   com->aux = NULL;
   unsigned before = requests;
   host_type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
   CHECK(get_type(com) == host_type && get_type(com) == host_type);
   CHECK(requests == before + 2);
   com->aux = aux;
   CHECK(get_type(com) == host_type && requests == before + 3);
   destroy_list(com);
}

static void test_wire_ordering(void)
{
   puts("CASE cached-query-preserves-wire-order");
   for (unsigned staging = 0; staging < 2; ++staging) {
      edge_count = flush_count = 0;
      for (unsigned i = 0; i < 3; ++i) rings[i].staging = staging;
      current_ring = &rings[0]; stage_index = 1;
      struct npt_com_base *com = new_list();
      host_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      CHECK(get_type(com) == host_type);
      unsigned before = requests;
      current_ring = &rings[1]; stage_index = 2;
      CHECK(get_type(com) == host_type);
      CHECK(requests == before && edge_count == 0 && flush_count == 0);
      current_ring = &rings[2]; stage_index = 3;
      CHECK(npt_id3d12graphicscommandlist_default_vtbl_storage.Close(com) == NPT_S_OK);
      if (staging) {
         CHECK(flush_count == 1 && flushed_index == 1);
         CHECK(atomic_load(&com->base.stage_index) == 3);
      } else {
         CHECK(edge_count == 1 && edges[0].predecessor == rings[0].id);
         CHECK(edges[0].ring == rings[2].id && edges[0].seqno == 42);
         CHECK(atomic_load(&com->base.order_ring_id) == rings[2].id);
      }
      uint64_t close_ring; uint32_t close_seqno;
      CHECK(npt_d3d12_list_close_barrier(com, &close_ring, &close_seqno));
      CHECK(close_ring == rings[2].id && close_seqno == rings[2].cur);
      exec_stamps_add(&list12_aux(com)->exec, 99, 100);
      current_ring = &rings[0]; stage_index = 1;
      CHECK(npt_id3d12graphicscommandlist_default_vtbl_storage.Reset(com, NULL, NULL) == NPT_S_OK);
      CHECK(edges[edge_count - 1].predecessor == 99 && edges[edge_count - 1].seqno == 100);
      CHECK(get_type(com) == host_type && requests == before);
      destroy_list(com);
   }
   CHECK(close_requests == 2);
}

int main(void)
{
   npt_overrides_d3d12_list_init();
   CHECK(family_registrations == 2);
   const struct npt_id3d12graphicscommandlist10_client_vtbl *vtables[] = {
      &npt_id3d12graphicscommandlist_default_vtbl_storage,
      &npt_id3d12graphicscommandlist1_default_vtbl_storage,
      &npt_id3d12graphicscommandlist2_default_vtbl_storage,
      &npt_id3d12graphicscommandlist3_default_vtbl_storage,
      &npt_id3d12graphicscommandlist4_default_vtbl_storage,
      &npt_id3d12graphicscommandlist5_default_vtbl_storage,
      &npt_id3d12graphicscommandlist6_default_vtbl_storage,
      &npt_id3d12graphicscommandlist7_default_vtbl_storage,
      &npt_id3d12graphicscommandlist8_default_vtbl_storage,
      &npt_id3d12graphicscommandlist9_default_vtbl_storage,
      &npt_id3d12graphicscommandlist10_default_vtbl_storage,
   };
   for (unsigned i = 0; i < 11; ++i)
      CHECK(vtables[i]->GetType == list12_GetType_override);
   for (unsigned i = 0; i < 3; ++i) rings[i].id = i + 1;
   current_ring = &rings[0];
   test_types_and_family();
   test_failed_reply();
   test_missing_aux();
   test_wire_ordering();
   CHECK(frees == requests && outstanding == 0);
   puts("PASS command-list GetType cache");
   return 0;
}
