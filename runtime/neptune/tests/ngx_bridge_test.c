/* npt_ngx_bridge + npt_dispatch_ngx unit test: private tear-off QI,
 * kind gating, the list-recording gate, wire bytes, ring choice after a
 * recording-thread migration, and reply validation.  Includes the
 * production translation units and stubs the ring submit boundary, the
 * owner vtbl, and the list-aux accessor. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../npt_ngx_bridge.c"
#include "../npt_dispatch.c"

#include "../ngx/npt_sha256.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#pragma GCC diagnostic ignored "-Wunused-parameter"

/* ---------- environment ---------- */

struct npt_profile npt_profile;
NPT_TLS struct npt_profile_thread *npt_tl_thread;

uint64_t npt_profile_now_ns(void) { return 0; }
struct npt_profile_thread *npt_profile_thread_register(void) { return NULL; }
void npt_profile_dump(const char *reason) {}

#ifdef __WINE__
void npt_log_impl(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
}
#endif

/* ---------- candidate fixture: a fake System32 with two payload files -- */

static char g_sysdir[512];
static char g_core_hex[65];
static char g_nvapi_hex[65];
static char g_mod_core[600];
static char g_mod_nvapi[600];

static void
set_env(const char *name, const char *value)
{
#if defined(_WIN32)
   SetEnvironmentVariableA(name, value);
#else
   if (value)
      setenv(name, value, 1);
   else
      unsetenv(name);
#endif
}

static void
write_fixture(const char *name, const char *content)
{
   char path[600];
   snprintf(path, sizeof(path), "%s/%s", g_sysdir, name);
   FILE *f = fopen(path, "wb");
   CHECK(f);
   CHECK(fwrite(content, 1, strlen(content), f) == strlen(content));
   fclose(f);
}

static void
hex_of_file(const char *name, char out[65])
{
   char path[600];
   snprintf(path, sizeof(path), "%s/%s", g_sysdir, name);
   FILE *f = fopen(path, "rb");
   CHECK(f);
   npt_sha256_ctx c;
   npt_sha256_init(&c);
   uint8_t buf[8192];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      npt_sha256_update(&c, buf, n);
   fclose(f);
   uint8_t dig[32];
   npt_sha256_final(&c, dig);
   for (unsigned i = 0; i < 32; i++)
      snprintf(out + i * 2, 3, "%02x", dig[i]);
}

static void
make_sysdir(void)
{
#if defined(_WIN32)
   char tmp[512];
   DWORD n = GetTempPathA(sizeof(tmp), tmp);
   CHECK(n && n < sizeof(tmp));
   snprintf(g_sysdir, sizeof(g_sysdir), "%snptngxvis%lu", tmp,
            (unsigned long)GetCurrentProcessId());
   CHECK(CreateDirectoryA(g_sysdir, NULL));
#else
   strcpy(g_sysdir, "/tmp/nptngxvisXXXXXX");
   CHECK(mkdtemp(g_sysdir) != NULL);
#endif
   write_fixture("nvngx.dll", "npt-core-payload-v2");
   write_fixture("nvapi64.dll", "npt-nvapi-payload-v2");
   hex_of_file("nvngx.dll", g_core_hex);
   hex_of_file("nvapi64.dll", g_nvapi_hex);
   snprintf(g_mod_core, sizeof(g_mod_core), "%s/nvngx.dll", g_sysdir);
   snprintf(g_mod_nvapi, sizeof(g_mod_nvapi), "%s/nvapi64.dll", g_sysdir);
   npt_ngx_vis_test_setup(g_sysdir, NULL, NULL, NULL);
}

static void
set_opt_in(bool on)
{
   if (on) {
      set_env("BRINEWELL_NGX_SR_CANDIDATE", "1");
      set_env("BRINEWELL_NEPTUNE_BACKEND", "d3dmetal");
      set_env("BRINEWELL_NGX_CORE_SHA256", g_core_hex);
      set_env("BRINEWELL_NGX_NVAPI_SHA256", g_nvapi_hex);
   } else {
      set_env("BRINEWELL_NGX_SR_CANDIDATE", NULL);
      set_env("BRINEWELL_NEPTUNE_BACKEND", NULL);
      set_env("BRINEWELL_NGX_CORE_SHA256", NULL);
      set_env("BRINEWELL_NGX_NVAPI_SHA256", NULL);
      set_env("BRINEWELL_NGX_SR_ACTIVE", NULL);
   }
   npt_ngx_vis_test_reset();
}

/* ---------- fake owner wrapper ---------- */

static struct npt_device device;
static struct npt_ring rings[4];
static struct npt_ring *current_ring;
static bool list_recording;
static bool context_immediate;
static bool resource_is_texture;
static unsigned owner_addrefs, owner_releases, owner_qi_calls;
static void *owner_identity_answer = (void *)0x1d3d12;

static HRESULT NPT_STDMETHODCALLTYPE
fake_owner_qi(void *self, const GUID *riid, void **out)
{
   ++owner_qi_calls;
   if (riid && resource_is_texture &&
       memcmp(riid, &NPT_IID_ID3D11Texture2D, sizeof(GUID)) == 0) {
      *out = self;
      ++owner_addrefs;
      return NPT_S_OK;
   }
   if (!riid || memcmp(riid, &NPT_IID_IUnknown, sizeof(GUID)) != 0)
      return NPT_E_NOINTERFACE;
   *out = owner_identity_answer;
   return NPT_S_OK;
}
static uint32_t NPT_STDMETHODCALLTYPE
fake_owner_addref(void *self) { return ++owner_addrefs; }
static uint32_t NPT_STDMETHODCALLTYPE
fake_owner_release(void *self) { return ++owner_releases; }

static const void *fake_owner_vtbl[] = {
   fake_owner_qi, fake_owner_addref, fake_owner_release,
};

static const struct npt_com_family family_adapter = {
   .index = NPT_IFACE_FAMILY_IDXGIADAPTER,
};
static const struct npt_com_family family_device = {
   .index = NPT_IFACE_FAMILY_ID3D12DEVICE,
};
static const struct npt_com_family family_list = {
   .index = NPT_IFACE_FAMILY_ID3D12GRAPHICSCOMMANDLIST,
};
static const struct npt_com_family family_resource = {
   .index = NPT_IFACE_FAMILY_ID3D12RESOURCE,
};
static const struct npt_com_family family_other = {
   .index = NPT_IFACE_FAMILY_IDXGIFACTORY,
};

static const GUID k_ngx_iid = {
   0x7ec368f5, 0x2e66, 0x4b9f,
   {0x9e, 0x97, 0xcf, 0xf4, 0x0b, 0xb9, 0x2c, 0x81}};
static const GUID k_other_iid = {
   0x11111111, 0x2222, 0x3333,
   {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}};

static struct npt_com_base *
new_wrapper(const struct npt_com_family *fam, uint64_t id)
{
   struct npt_com_base *com = calloc(1, sizeof(*com));
   CHECK(com);
   npt_com_base_init(com, fake_owner_vtbl, &device, id);
   com->family = fam;
   return com;
}

/* ---------- stubs the included TUs need ---------- */

bool npt_d3d12_list_ngx_recording(void *self) { return list_recording; }
bool npt_d3d11_context_is_immediate(void *self) { return context_immediate; }

struct npt_ring *npt_tls_get_ring(struct npt_device *dev)
{ return current_ring; }
struct npt_ring *npt_device_get_pipeline_library_ring(struct npt_device *dev)
{ CHECK(false); return NULL; }
uint32_t npt_ring_stage_self_index(struct npt_ring *ring) { return 0; }
void npt_ring_stage_flush_index(struct npt_ring *ring, uint32_t index)
{ CHECK(false); }
void npt_ring_order_after_all(struct npt_ring *ring) { CHECK(false); }

static struct { uint64_t ring, predecessor; uint32_t seqno; } edges[16];
static unsigned edge_count;
static bool tls_tail_found;
void npt_ring_order_after(struct npt_ring *ring, uint64_t id, uint32_t seqno)
{
   CHECK(edge_count < 16);
   edges[edge_count].ring = ring->id;
   edges[edge_count].predecessor = id;
   edges[edge_count++].seqno = seqno;
}
bool npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t id,
                             uint32_t *tail)
{ *tail = 77; tls_tail_found = true; return true; }

/* Recorded submissions: bytes captured per call, one at a time. */
static uint8_t last_cmd[256];
static uint32_t last_cmd_size;
static struct npt_ring *last_cmd_ring;
static unsigned submits, replies_issued, frees, outstanding, drops;
static uint32_t reply_result, reply_mode;
static uint64_t reply_value;
enum { REPLY_VALID, REPLY_NONE, REPLY_WRONG_ECHO, REPLY_SHORT };

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
{
   CHECK(submit->cmd_size <= sizeof(last_cmd));
   memcpy(last_cmd, submit->cmd_data, submit->cmd_size);
   last_cmd_size = (uint32_t)submit->cmd_size;
   last_cmd_ring = ring;
   submit->seqno = ++ring->cur;
   ++submits;
   ++outstanding;
}
struct npt_cs_decoder *
npt_ring_get_command_reply(struct npt_ring *ring,
                           struct npt_ring_submit_command *submit)
{
   static struct npt_cmd_ngx_reply reply;
   if (reply_mode == REPLY_NONE) {
      /* The reply window was allocated at submit but the wait dropped
       * (ring fatal / renderer lost): production must still call
       * free_command_reply to release the window reference. */
      ++drops;
      return NULL;
   }
   memset(&reply, 0, sizeof(reply));
   const struct npt_command_header *hdr = (const void *)last_cmd;
   reply.header.cmd_type =
      reply_mode == REPLY_WRONG_ECHO ? hdr->cmd_type + 1 : hdr->cmd_type;
   reply.header.cmd_return = reply_result;
   reply.value = reply_result == NPT_NGX_RESULT_SUCCESS ? reply_value : 0;
   submit->dec.cur = (const uint8_t *)&reply;
   submit->dec.end = submit->dec.cur + sizeof(reply) -
                     (reply_mode == REPLY_SHORT);
   ++replies_issued;
   return &submit->dec;
}
void npt_ring_free_command_reply(struct npt_ring *ring,
                                 struct npt_ring_submit_command *submit)
{ CHECK(outstanding == 1); --outstanding; ++frees; }

bool npt_ring_submit_raw(struct npt_ring *ring, const void *data,
                         uint32_t size)
{ CHECK(false); return false; }
bool npt_ring_submit_raw_seqno(struct npt_ring *ring, const void *data,
                               uint32_t size, uint32_t *out_seqno)
{ CHECK(false); return false; }
bool npt_ring_submit_raw_with_payload(struct npt_ring *ring,
                                      const void *header,
                                      uint32_t header_size,
                                      const void *payload,
                                      uint32_t payload_size,
                                      uint32_t payload_padded_size)
{ CHECK(false); return false; }

/* ---------- cases ---------- */

static void
test_tearoff_identity(void)
{
   puts("CASE tearoff-qi-identity-ref");
   set_opt_in(true);
   struct npt_com_base *owner = new_wrapper(&family_list, 1001);
   list_recording = true;

   void *out = NULL;
   CHECK(npt_ngx_bridge_query_interface(owner, &k_ngx_iid, &out) ==
         NPT_S_OK);
   CHECK(out && out != owner);
   CHECK(owner_addrefs == 1 && owner_qi_calls == 0);
   NptNgxBridgeV1 *b = out;

   /* QI on the wrong IID / a foreign riid is refused. */
   void *junk = (void *)1;
   CHECK(npt_ngx_bridge_query_interface(owner, &k_other_iid, &junk) ==
         NPT_E_NOINTERFACE && junk == NULL);

   /* GetRef: kind, process cookie, object id. */
   uint32_t kind = 0;
   uintptr_t cookie = 0;
   uint64_t oid = 0;
   CHECK(b->lpVtbl->GetRef(b, &kind, &cookie, &oid) == NPT_SR_S_OK);
   CHECK(kind == NPT_SR_KIND_LIST && oid == 1001);
   CHECK(cookie == (uintptr_t)&device);
   CHECK(b->lpVtbl->GetRef(b, NULL, &cookie, &oid) == NPT_SR_E_POINTER);

   /* Tear-off QI(IUnknown) delegates to the owner's identity; QI of the
    * private IID returns the tear-off itself. */
   void *unk = NULL;
   CHECK(b->lpVtbl->QueryInterface(b, &NPT_SR_IID_IUNKNOWN, &unk) ==
         NPT_SR_S_OK);
   CHECK(unk == owner_identity_answer && owner_qi_calls == 1);
   void *self2 = NULL;
   CHECK(b->lpVtbl->QueryInterface(b, &NPT_SR_IID_NGX_BRIDGE_V1, &self2) ==
         NPT_SR_S_OK);
   CHECK(self2 == b);
   CHECK(b->lpVtbl->QueryInterface(b, &NPT_SR_IID_IUNKNOWN, NULL) ==
         NPT_SR_E_POINTER);

   CHECK(b->lpVtbl->Release(b) == 1);
   CHECK(b->lpVtbl->Release(b) == 0);
   CHECK(owner_releases == 1);
   free(owner);
}

static void
test_foreign_rejected(void)
{
   puts("CASE foreign-and-optout-rejected");
   void *out = NULL;

   /* Opted-in but wrong shapes. */
   set_opt_in(true);
   struct npt_com_base *no_fam = new_wrapper(NULL, 7);
   CHECK(npt_ngx_bridge_query_interface(no_fam, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   free(no_fam);

   struct npt_com_base *fab = new_wrapper(&family_list,
                                          npt_com_make_guest_id(
                                             NPT_GUEST_KIND_OUTPUT, 5));
   CHECK(npt_ngx_bridge_query_interface(fab, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   free(fab);

   struct npt_com_base *other = new_wrapper(&family_other, 9);
   CHECK(npt_ngx_bridge_query_interface(other, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   free(other);

   /* Real wrapper, opt-in cleared: refused like any unknown IID. */
   struct npt_com_base *dev = new_wrapper(&family_device, 10);
   set_opt_in(false);
   CHECK(npt_ngx_bridge_query_interface(dev, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   set_opt_in(true);
   CHECK(npt_ngx_bridge_query_interface(dev, &k_ngx_iid, &out) ==
         NPT_S_OK);
   ((NptNgxBridgeV1 *)out)->lpVtbl->Release((NptNgxBridgeV1 *)out);
   free(dev);
}

static void
test_kind_gating(void)
{
   puts("CASE kind-gating");
   set_opt_in(true);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 3;
   list_recording = true;
   void *out;
   NptSrCreateV1 c = {640, 360, 1280, 720, 0, 0x02};
   NptSrEvalIdsV1 e = {1, 2, 3, 4, 0, 640, 360, 0, 0, 1, 1, 1, 0};

   /* Adapter: Caps only. */
   struct npt_com_base *ad = new_wrapper(&family_adapter, 40);
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) == NPT_S_OK);
   NptNgxBridgeV1 *ab = out;
   uint32_t sr = 0;
   unsigned before = submits;
   CHECK(ab->lpVtbl->Caps(ab, &sr) == NPT_SR_RESULT_SUCCESS && sr == 3);
   CHECK(submits == before + 1 && last_cmd_ring == current_ring);
   struct npt_cmd_ngx_query_caps *qc = (void *)last_cmd;
   CHECK(qc->header.cmd_type ==
         NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX,
                                NPT_TRANSPORT_NGX_QUERY_CAPS));
   CHECK(qc->header.object_id == 40 &&
         qc->header.cmd_size == sizeof(*qc) &&
         qc->header.cmd_flags == NPT_CMD_FLAG_REPLY &&
         qc->header.reserved == 0);
   uint32_t unsupported = ab->lpVtbl->Eval(ab, 1, &e);
   CHECK(unsupported == 0xBAD00001u &&
         (unsupported & 0xFFF00000u) == 0xBAD00000u);
   CHECK(ab->lpVtbl->Create(ab, 1, &c, &(uint64_t){0}) ==
         0xBAD00001u);
   CHECK(ab->lpVtbl->Init(ab, 1, 1) == 0xBAD00001u);
   CHECK(submits == before + 1);
   ab->lpVtbl->Release(ab);
   free(ad);

   /* Resource: GetRef only. */
   struct npt_com_base *rs = new_wrapper(&family_resource, 41);
   CHECK(npt_ngx_bridge_query_interface(rs, &k_ngx_iid, &out) == NPT_S_OK);
   NptNgxBridgeV1 *rb = out;
   uint32_t kind;
   uintptr_t cookie;
   uint64_t oid;
   CHECK(rb->lpVtbl->GetRef(rb, &kind, &cookie, &oid) == NPT_SR_S_OK &&
         kind == NPT_SR_KIND_RESOURCE && oid == 41);
   CHECK(rb->lpVtbl->Caps(rb, &sr) == 0xBAD00001u);
   CHECK(rb->lpVtbl->Eval(rb, 1, &e) == 0xBAD00001u);
   CHECK(rb->lpVtbl->Shutdown(rb) == 0xBAD00001u);
   CHECK(submits == before + 1);
   rb->lpVtbl->Release(rb);
   free(rs);
}

static void
test_device_methods(void)
{
   puts("CASE device-init-release-shutdown");
   set_opt_in(true);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 0;
   struct npt_com_base *dev = new_wrapper(&family_device, 50);
   void *out;
   CHECK(npt_ngx_bridge_query_interface(dev, &k_ngx_iid, &out) == NPT_S_OK);
   NptNgxBridgeV1 *db = out;
   unsigned before = submits;

   CHECK(db->lpVtbl->Init(db, 0xfeed, 0x13) == NPT_SR_RESULT_SUCCESS);
   struct npt_cmd_ngx_init *ic = (void *)last_cmd;
   CHECK(ic->header.object_id == 50 && ic->header.cmd_size == sizeof(*ic));
   CHECK(ic->header.cmd_type ==
         NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX,
                                NPT_TRANSPORT_NGX_INIT));
   CHECK(ic->application_id == 0xfeed && ic->sdk_version == 0x13 &&
         ic->pad == 0);

   CHECK(db->lpVtbl->ReleaseFeature(db, 0xabc) == NPT_SR_RESULT_SUCCESS);
   struct npt_cmd_ngx_release *rc = (void *)last_cmd;
   CHECK(rc->header.object_id == 50 && rc->token == 0xabc &&
         rc->header.cmd_size == sizeof(*rc));
   CHECK(rc->header.cmd_type ==
         NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX,
                                NPT_TRANSPORT_NGX_RELEASE));

   CHECK(db->lpVtbl->Shutdown(db) == NPT_SR_RESULT_SUCCESS);
   struct npt_cmd_ngx_shutdown *sc = (void *)last_cmd;
   CHECK(sc->header.object_id == 50 &&
         sc->header.cmd_size == sizeof(*sc) &&
         sc->header.cmd_type ==
            NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX,
                                   NPT_TRANSPORT_NGX_SHUTDOWN));

   CHECK(db->lpVtbl->ReleaseFeature(db, 0) ==
         0xBAD00005u);
   uint32_t sr;
   CHECK(db->lpVtbl->Caps(db, &sr) == 0xBAD00001u);
   NptSrEvalIdsV1 e = {0};
   CHECK(db->lpVtbl->Eval(db, 1, &e) == 0xBAD00001u);
   CHECK(submits == before + 3);
   db->lpVtbl->Release(db);
   free(dev);
}

static void
test_list_create_eval(void)
{
   puts("CASE list-create-eval-wire-bytes");
   set_opt_in(true);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 0x5a;
   list_recording = true;
   struct npt_com_base *list = new_wrapper(&family_list, 60);
   void *out;
   CHECK(npt_ngx_bridge_query_interface(list, &k_ngx_iid, &out) ==
         NPT_S_OK);
   NptNgxBridgeV1 *lb = out;
   unsigned before = submits;

   uint64_t token = 0;
   NptSrCreateV1 c = {640, 360, 1280, 720, 1,
                      NPT_NGX_CREATE_FLAG_MV_LOW_RES};
   CHECK(lb->lpVtbl->Create(lb, 50, &c, &token) == NPT_SR_RESULT_SUCCESS);
   CHECK(token == 0x5a);
   struct npt_cmd_ngx_create *cc = (void *)last_cmd;
   CHECK(cc->header.cmd_type ==
         NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX,
                                NPT_TRANSPORT_NGX_CREATE));
   CHECK(cc->header.object_id == 60 && cc->header.cmd_size == sizeof(*cc));
   CHECK(cc->device_id == 50 && cc->feature_id == NPT_NGX_FEATURE_SR);
   CHECK(cc->in_w == 640 && cc->in_h == 360 &&
         cc->out_w == 1280 && cc->out_h == 720);
   CHECK(cc->quality == 1 &&
         cc->create_flags == NPT_NGX_CREATE_FLAG_MV_LOW_RES &&
         cc->pad == 0);

   NptSrCreateV1 low_flags = c;
   low_flags.flags = 0x01 | 0x04 | 0x80;
   CHECK(lb->lpVtbl->Create(lb, 50, &low_flags, &token) ==
         NPT_SR_RESULT_SUCCESS);
   CHECK(token == 0x5a && cc->create_flags == low_flags.flags);

   NptSrEvalIdsV1 e = {11, 12, 13, 14, 15, 640, 360,
                       0.5f, -0.25f, 1.0f, 1.0f, 0.0f, 1};
   CHECK(lb->lpVtbl->Eval(lb, token, &e) == NPT_SR_RESULT_SUCCESS);
   struct npt_cmd_ngx_eval *ec = (void *)last_cmd;
   CHECK(ec->header.cmd_type ==
         NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX,
                                NPT_TRANSPORT_NGX_EVAL));
   CHECK(ec->header.object_id == 60 &&
         ec->header.cmd_size == sizeof(*ec));
   CHECK(ec->token == 0x5a && ec->color == 11 && ec->output == 12 &&
         ec->depth == 13 && ec->motion == 14 && ec->exposure == 15);
   CHECK(ec->render_w == 640 && ec->render_h == 360);
   CHECK(ec->jitter_x == 0.5f && ec->jitter_y == -0.25f);
   CHECK(ec->mv_scale_x == 1.0f && ec->mv_scale_y == 1.0f);
   /* Zero pre-exposure is normalized to 1 on the guest side. */
   CHECK(ec->pre_exposure == 1.0f && ec->reset == 1);

   /* Local validation failures never reach the ring. */
   NptSrCreateV1 badc = c;
   badc.flags = 0x100;
   CHECK(lb->lpVtbl->Create(lb, 50, &badc, &token) ==
         0xBAD00001u);
   badc = c;
   badc.in_w = 2000;
   CHECK(lb->lpVtbl->Create(lb, 50, &badc, &token) ==
         0xBAD00005u);
   badc = c;
   badc.quality = 6;
   CHECK(lb->lpVtbl->Create(lb, 50, &badc, &token) ==
         0xBAD00001u);
   NptSrEvalIdsV1 bade = e;
   bade.color = 0;
   CHECK(lb->lpVtbl->Eval(lb, token, &bade) ==
         0xBAD00005u);
   bade = e;
   bade.jitter_x = 0.0f / 0.0f;
   CHECK(lb->lpVtbl->Eval(lb, token, &bade) ==
         0xBAD00005u);
   CHECK(lb->lpVtbl->Eval(lb, 0, &e) == 0xBAD00005u);
   CHECK(lb->lpVtbl->Create(lb, 0, &c, &token) ==
         0xBAD00005u);
   CHECK(submits == before + 3);

   /* A success reply carrying a zero token is a failure. */
   reply_value = 0;
   CHECK(lb->lpVtbl->Create(lb, 50, &c, &token) == 0xBAD00000u &&
         token == 0);
   lb->lpVtbl->Release(lb);
   free(list);
}

static void
test_recording_gate(void)
{
   puts("CASE closed-list-cannot-submit");
   set_opt_in(true);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 7;
   struct npt_com_base *list = new_wrapper(&family_list, 61);
   void *out;
   CHECK(npt_ngx_bridge_query_interface(list, &k_ngx_iid, &out) ==
         NPT_S_OK);
   NptNgxBridgeV1 *lb = out;
   NptSrCreateV1 c = {640, 360, 1280, 720, 0, 0};
   NptSrEvalIdsV1 e = {1, 2, 3, 4, 0, 640, 360, 0, 0, 1, 1, 1, 0};
   uint64_t token = 0;
   unsigned before = submits;

   list_recording = false;
   CHECK(lb->lpVtbl->Create(lb, 50, &c, &token) ==
         0xBAD00005u);
   CHECK(lb->lpVtbl->Eval(lb, 7, &e) == 0xBAD00005u);
   CHECK(submits == before);

   list_recording = true;
   CHECK(lb->lpVtbl->Create(lb, 50, &c, &token) == NPT_SR_RESULT_SUCCESS &&
         token == 7);
   CHECK(lb->lpVtbl->Eval(lb, token, &e) == NPT_SR_RESULT_SUCCESS);
   list_recording = false; /* Closed. */
   CHECK(lb->lpVtbl->Eval(lb, token, &e) == 0xBAD00005u);
   CHECK(submits == before + 2);
   lb->lpVtbl->Release(lb);
   free(list);
}

static void
test_ring_migration(void)
{
   puts("CASE recording-thread-migration-uses-new-ring");
   set_opt_in(true);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 9;
   list_recording = true;
   struct npt_com_base *list = new_wrapper(&family_list, 62);
   list->base.ring_ordered = true;
   void *out;
   CHECK(npt_ngx_bridge_query_interface(list, &k_ngx_iid, &out) ==
         NPT_S_OK);
   NptNgxBridgeV1 *lb = out;
   NptSrEvalIdsV1 e = {1, 2, 3, 4, 0, 640, 360, 0, 0, 1, 1, 1, 0};

   current_ring = &rings[0];
   CHECK(lb->lpVtbl->Eval(lb, 9, &e) == NPT_SR_RESULT_SUCCESS);
   CHECK(last_cmd_ring == &rings[0] && edge_count == 0);

   /* The next call on another thread's ring orders it after the old
    * one, then submits there. */
   current_ring = &rings[2];
   CHECK(lb->lpVtbl->Eval(lb, 9, &e) == NPT_SR_RESULT_SUCCESS);
   CHECK(last_cmd_ring == &rings[2]);
   CHECK(edge_count == 1 && edges[0].ring == rings[2].id &&
         edges[0].predecessor == rings[0].id && edges[0].seqno == 77);

   lb->lpVtbl->Release(lb);
   free(list);
}

static void
test_reply_failures(void)
{
   puts("CASE reply-validation");
   set_opt_in(true);
   list_recording = true;
   struct npt_com_base *ad = new_wrapper(&family_adapter, 70);
   void *out;
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) == NPT_S_OK);
   NptNgxBridgeV1 *ab = out;
   uint32_t sr = 99;

   /* No valid reply is distinguished from a backend rejection: the
    * caller sees the transport-lost sentinel, not a wire result. */
   reply_mode = REPLY_NONE;
   CHECK(ab->lpVtbl->Caps(ab, &sr) == NPT_SR_RESULT_TRANSPORT_LOST &&
         sr == 0);
   reply_mode = REPLY_SHORT;
   CHECK(ab->lpVtbl->Caps(ab, &sr) == NPT_SR_RESULT_TRANSPORT_LOST &&
         sr == 0);
   reply_mode = REPLY_WRONG_ECHO;
   CHECK(ab->lpVtbl->Caps(ab, &sr) == NPT_SR_RESULT_TRANSPORT_LOST &&
         sr == 0);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_NOT_SUPPORTED;
   CHECK(ab->lpVtbl->Caps(ab, &sr) == 0xBAD00001u &&
         sr == 0);
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 1;
   CHECK(ab->lpVtbl->Caps(ab, &sr) == NPT_SR_RESULT_SUCCESS && sr == 1);

   ab->lpVtbl->Release(ab);
   free(ad);
}

static bool
always_fail_hash(const char *path, uint8_t out[32])
{
   (void)path; (void)out;
   return false;
}

static void
test_visibility_gates(void)
{
   puts("CASE process-visibility-gates");
   struct npt_com_base *ad = new_wrapper(&family_adapter, 80);
   void *out = (void *)1;

   /* Stale inherited ACTIVE without a candidate is not trusted. */
   set_opt_in(false);
   set_env("BRINEWELL_NGX_SR_ACTIVE", "1");
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);

   /* One payload file replaced (hash mismatch on nvngx.dll only). */
   set_opt_in(true);
   write_fixture("nvngx.dll", "foreign-core");
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   write_fixture("nvngx.dll", "npt-core-payload-v2");

   /* Read failure: nvapi64.dll missing from the visible directory. */
   char gone[600];
   snprintf(gone, sizeof(gone), "%s/nvapi64.dll", g_sysdir);
   remove(gone);
   npt_ngx_vis_test_reset();
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   write_fixture("nvapi64.dll", "npt-nvapi-payload-v2");

   /* A same-name module loaded from outside System32 is foreign. */
   npt_ngx_vis_test_setup(g_sysdir, "C:\\elsewhere\\nvngx.dll", NULL,
                          NULL);
   npt_ngx_vis_test_reset();
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   npt_ngx_vis_test_setup(g_sysdir, g_mod_core, "D:\\game\\nvapi64.dll",
                          NULL);
   npt_ngx_vis_test_reset();
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);

   /* Modules loaded from System32 plus matching hashes: active, and
    * ACTIVE is published for the rest of the process. */
   npt_ngx_vis_test_setup(g_sysdir, g_mod_core, g_mod_nvapi, NULL);
   npt_ngx_vis_test_reset();
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_S_OK && out != NULL);
   ((NptNgxBridgeV1 *)out)->lpVtbl->Release((NptNgxBridgeV1 *)out);
#if defined(_WIN32)
   char abuf[8];
   DWORD an = GetEnvironmentVariableA("BRINEWELL_NGX_SR_ACTIVE", abuf,
                                      sizeof(abuf));
   CHECK(an == 1 && abuf[0] == '1');
#else
   const char *as = getenv("BRINEWELL_NGX_SR_ACTIVE");
   CHECK(as && !strcmp(as, "1"));
#endif

   /* Injected hash failure stands in for an unreadable file. */
   npt_ngx_vis_test_setup(g_sysdir, NULL, NULL, always_fail_hash);
   npt_ngx_vis_test_reset();
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_E_NOINTERFACE && out == NULL);
   npt_ngx_vis_test_setup(g_sysdir, NULL, NULL, NULL);
   npt_ngx_vis_test_reset();
   CHECK(npt_ngx_bridge_query_interface(ad, &k_ngx_iid, &out) ==
         NPT_S_OK);
   ((NptNgxBridgeV1 *)out)->lpVtbl->Release((NptNgxBridgeV1 *)out);

   set_opt_in(true);
   free(ad);
}

static void
test_d3d11_bridge(void)
{
   const struct npt_com_family fd = {.index = NPT_IFACE_FAMILY_ID3D11DEVICE};
   const struct npt_com_family fc = {.index = NPT_IFACE_FAMILY_ID3D11DEVICECONTEXT};
   const struct npt_com_family ft = {.index = NPT_IFACE_FAMILY_ID3D11TEXTURE2D};
   const struct npt_com_family fr = {.index = NPT_IFACE_FAMILY_ID3D11RESOURCE};
   struct npt_com_base *dev = new_wrapper(&fd, 80);
   struct npt_com_base *ctx = new_wrapper(&fc, 81);
   struct npt_com_base *tex = new_wrapper(&ft, 82);
   struct npt_com_base *res = new_wrapper(&fr, 83);
   void *p = NULL;
   set_opt_in(true);
   reply_mode = REPLY_VALID;
   reply_result = NPT_NGX_RESULT_SUCCESS;
   reply_value = 77;
   context_immediate = false;
   CHECK(npt_ngx_bridge_query_interface(ctx, &k_ngx_iid, &p) == NPT_E_NOINTERFACE);
   context_immediate = true;
   CHECK(npt_ngx_bridge_query_interface(ctx, &k_ngx_iid, &p) == NPT_S_OK);
   NptNgxBridgeV1 *cb = p;
   ctx->base.instance_ring = &rings[3];
   NptSrCreateV1 c = {1280, 720, 1280, 720, 5, 0};
   uint64_t token = 0;
   CHECK(cb->lpVtbl->Create(cb, 80, &c, &token) == NPT_SR_RESULT_SUCCESS);
   CHECK(((struct npt_command_header *)last_cmd)->cmd_type == NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX, NPT_TRANSPORT_NGX_D3D11_CREATE));
   CHECK(last_cmd_ring == &rings[3]);
   NptSrEvalIdsV1 e = {1, 2, 3, 4, 0, 1280, 720, 0, 0, 1, 1, 1, 0};
   CHECK(cb->lpVtbl->Eval(cb, token, &e) == NPT_SR_RESULT_SUCCESS);
   CHECK(((struct npt_command_header *)last_cmd)->cmd_type == NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX, NPT_TRANSPORT_NGX_D3D11_EVAL));
   CHECK(last_cmd_ring == &rings[3]);
   cb->lpVtbl->Release(cb);
   CHECK(npt_ngx_bridge_query_interface(dev, &k_ngx_iid, &p) == NPT_S_OK);
   NptNgxBridgeV1 *db = p;
   CHECK(db->lpVtbl->Init(db, 1, 2) == NPT_SR_RESULT_SUCCESS);
   CHECK(((struct npt_command_header *)last_cmd)->cmd_type == NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX, NPT_TRANSPORT_NGX_D3D11_INIT));
   CHECK(db->lpVtbl->ReleaseFeature(db, token) == NPT_SR_RESULT_SUCCESS);
   CHECK(((struct npt_command_header *)last_cmd)->cmd_type == NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX, NPT_TRANSPORT_NGX_D3D11_RELEASE));
   CHECK(db->lpVtbl->Shutdown(db) == NPT_SR_RESULT_SUCCESS);
   CHECK(((struct npt_command_header *)last_cmd)->cmd_type == NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_NGX, NPT_TRANSPORT_NGX_D3D11_SHUTDOWN));
   db->lpVtbl->Release(db);
   CHECK(npt_ngx_bridge_query_interface(tex, &k_ngx_iid, &p) == NPT_S_OK);
   ((NptNgxBridgeV1 *)p)->lpVtbl->Release(p);
   resource_is_texture = false;
   CHECK(npt_ngx_bridge_query_interface(res, &k_ngx_iid, &p) == NPT_E_NOINTERFACE);
   resource_is_texture = true;
   unsigned released = owner_releases;
   CHECK(npt_ngx_bridge_query_interface(res, &k_ngx_iid, &p) == NPT_S_OK);
   CHECK(owner_releases == released + 1);
   ((NptNgxBridgeV1 *)p)->lpVtbl->Release(p);
   resource_is_texture = false;
   free(dev); free(ctx); free(tex); free(res);
}

int
main(void)
{
   make_sysdir();
   memset(&device, 0, sizeof(device));
   memset(rings, 0, sizeof(rings));
   for (unsigned i = 0; i < 4; ++i) {
      rings[i].id = i + 1;
      rings[i].device = &device;
   }
   current_ring = &rings[0];

   test_tearoff_identity();
   test_foreign_rejected();
   test_kind_gating();
   test_device_methods();
   test_list_create_eval();
   test_recording_gate();
   test_ring_migration();
   test_reply_failures();
   test_visibility_gates();
   test_d3d11_bridge();
   /* Every reply window reference is recovered: decoded replies and
    * dropped waits alike. */
   CHECK(outstanding == 0 && frees == replies_issued + drops);
   puts("PASS ngx-bridge");
   return 0;
}
