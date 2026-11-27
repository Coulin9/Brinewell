/* Host-buildable test for the opt-in adapter identity overrides in
 * npt_overrides_dxgi_adapter.c and the minimal nvapi64 surface.  The
 * production .c files are compiled into this test executable together
 * with npt_ngx_visibility.c under NPT_NGX_TEST_HOOKS; this file
 * supplies the host-call boundary (default GetDesc* thunks), a fake
 * bridge tear-off with controllable Caps, a fake System32 payload
 * fixture, and stubs for the COM runtime externs.  Covers the
 * candidate/hash/module gating, identity field preservation across all
 * four desc tiers, the nvapi id table, and consumer call-order
 * independence. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx/npt_ngx_visibility.h"
#include "ngx/npt_sha256.h"

#include "npt_com.h"
#include "npt_display.h"
#include "npt_env.h"
#include "npt_ngx_bridge_v1.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
   exit(1); } } while (0)

/* ---------- stubs for the production TU's externals ---------- */

struct npt_env npt_env;
struct npt_idxgiadapter4_client_vtbl npt_idxgiadapter4_default_vtbl_storage;

void
npt_com_register_family(const GUID *const *tier_iids, size_t aux_size,
                        npt_com_aux_init_fn aux_init)
{
   (void)tier_iids; (void)aux_size; (void)aux_init;
}

void *
npt_com_get_or_wrap(struct npt_device *dev, const GUID *iid,
                    uint64_t host_id,
                    struct npt_com_base *parent_wrapper)
{
   (void)dev; (void)iid; (void)host_id; (void)parent_wrapper;
   return NULL;
}

void
npt_output_aux_populate(struct npt_com_base *out_com,
                        uint32_t connector_index,
                        const struct npt_output_info *info)
{
   (void)out_com; (void)connector_index; (void)info;
}

/* Default GetDesc* thunks: fill host-side sentinel values. */

static void
fill_desc_sentinel(WCHAR description[128], UINT *vendor, UINT *device,
                   UINT *subsys, UINT *rev, LUID *luid)
{
   static const char name[] = "Brinewell Virtual Adapter";
   size_t i;
   for (i = 0; i < sizeof(name) - 1; i++)
      description[i] = (WCHAR)(uint8_t)name[i];
   description[i] = 0;
   *vendor = 0x1234;
   *device = 0x5678;
   *subsys = 0xaaaa;
   *rev = 0xbb;
   luid->LowPart = 0x0badc0de;
   luid->HighPart = 0x12345678;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_idxgiadapter_default_GetDesc(void *self, DXGI_ADAPTER_DESC *pDesc)
{
   (void)self;
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   fill_desc_sentinel(pDesc->Description, &pDesc->VendorId,
                      &pDesc->DeviceId, &pDesc->SubSysId,
                      &pDesc->Revision, &pDesc->AdapterLuid);
   pDesc->DedicatedVideoMemory = 0x1000;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_idxgiadapter1_default_GetDesc1(void *self, DXGI_ADAPTER_DESC1 *pDesc)
{
   (void)self;
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   fill_desc_sentinel(pDesc->Description, &pDesc->VendorId,
                      &pDesc->DeviceId, &pDesc->SubSysId,
                      &pDesc->Revision, &pDesc->AdapterLuid);
   pDesc->Flags = 7;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_idxgiadapter2_default_GetDesc2(void *self, DXGI_ADAPTER_DESC2 *pDesc)
{
   (void)self;
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   fill_desc_sentinel(pDesc->Description, &pDesc->VendorId,
                      &pDesc->DeviceId, &pDesc->SubSysId,
                      &pDesc->Revision, &pDesc->AdapterLuid);
   pDesc->Flags = 9;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_idxgiadapter4_default_GetDesc3(void *self, DXGI_ADAPTER_DESC3 *pDesc)
{
   (void)self;
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   fill_desc_sentinel(pDesc->Description, &pDesc->VendorId,
                      &pDesc->DeviceId, &pDesc->SubSysId,
                      &pDesc->Revision, &pDesc->AdapterLuid);
   return NPT_S_OK;
}

/* Fake bridge tear-off with a controllable Caps answer. */

static uint32_t g_caps_sr = 1;
static uint32_t g_caps_result = NPT_SR_RESULT_SUCCESS;
static unsigned g_caps_calls;

static NPT_SR_HRESULT NPT_SR_METHODCALLTYPE
tb_qi(NptNgxBridgeV1 *self, const NptSrGuid *riid, void **out)
{
   (void)self; (void)riid;
   if (out) *out = NULL;
   return NPT_SR_E_NOINTERFACE;
}
static uint32_t NPT_SR_METHODCALLTYPE
tb_addref(NptNgxBridgeV1 *self) { (void)self; return 2; }
static uint32_t NPT_SR_METHODCALLTYPE
tb_release(NptNgxBridgeV1 *self) { (void)self; return 1; }
static NPT_SR_HRESULT NPT_SR_METHODCALLTYPE
tb_get_ref(NptNgxBridgeV1 *self, uint32_t *kind,
           uintptr_t *cookie, uint64_t *object_id)
{
   (void)self;
   *kind = NPT_SR_KIND_ADAPTER;
   *cookie = 0;
   *object_id = 0xad;
   return NPT_SR_S_OK;
}
static uint32_t NPT_SR_METHODCALLTYPE
tb_caps(NptNgxBridgeV1 *self, uint32_t *sr)
{
   (void)self;
   ++g_caps_calls;
   *sr = g_caps_sr;
   return g_caps_result;
}
static uint32_t NPT_SR_METHODCALLTYPE
tb_nop(NptNgxBridgeV1 *self)
{
   (void)self;
   return NPT_SR_RESULT_NOT_SUPPORTED;
}

static const NptNgxBridgeV1Vtbl tb_vtbl = {
   tb_qi, tb_addref, tb_release, tb_get_ref, tb_caps,
   (uint32_t (NPT_SR_METHODCALLTYPE *)(NptNgxBridgeV1 *, uint64_t,
                                       uint32_t))tb_nop,
   (uint32_t (NPT_SR_METHODCALLTYPE *)(NptNgxBridgeV1 *, uint64_t,
                                       const NptSrCreateV1 *,
                                       uint64_t *))tb_nop,
   (uint32_t (NPT_SR_METHODCALLTYPE *)(NptNgxBridgeV1 *, uint64_t,
                                       const NptSrEvalIdsV1 *))tb_nop,
   (uint32_t (NPT_SR_METHODCALLTYPE *)(NptNgxBridgeV1 *, uint64_t))tb_nop,
   tb_nop,
};

static NptNgxBridgeV1 tb_bridge = {&tb_vtbl};

HRESULT
npt_ngx_bridge_query_interface(void *self, const GUID *riid, void **out)
{
   (void)self; (void)riid;
   if (!out)
      return NPT_E_POINTER;
   *out = &tb_bridge;
   return NPT_S_OK;
}

/* nvapi64.c symbols */
void *nvapi_QueryInterface(uint32_t id);

static unsigned luid_attempts;

int
npt_nvapi_test_get_luid(uint32_t out[2])
{
   if (++luid_attempts == 1)
      return 0;
   out[0] = 0x0badc0de;
   out[1] = 0x12345678;
   return 1;
}

struct vis_registry_ops {
   int (*read)(void *, unsigned, const char *, uint32_t *, uint8_t **, uint32_t *);
   int (*write)(void *, unsigned, const char *, uint32_t, const void *, uint32_t);
   int (*remove)(void *, unsigned, const char *);
};

void npt_ngx_vis_test_registry(const struct vis_registry_ops *, void *, unsigned,
                               bool, uint32_t, const void *, uint32_t);

struct mock_value {
   bool present;
   uint32_t type, size;
   uint8_t data[128];
};

struct mock_registry {
   struct mock_value values[3][5];
};

static struct mock_value *
mock_slot(void *ctx, unsigned i, const char *name)
{
   struct mock_registry *r = ctx;
   unsigned slot = !name ? 0 : !strcmp(name, "Managed") ? 1 :
                   !strcmp(name, "HadOrig") ? 2 : !strcmp(name, "OrigType") ? 3 : 4;
   CHECK(i < 3 && (!name || slot != 4 || !strcmp(name, "OrigData")));
   return &r->values[i][slot];
}

static int
mock_read(void *ctx, unsigned i, const char *name, uint32_t *type,
          uint8_t **data, uint32_t *size)
{
   struct mock_value *v = mock_slot(ctx, i, name);
   if (!v->present)
      return 0;
   *data = malloc(v->size ? v->size : 1);
   CHECK(*data);
   memcpy(*data, v->data, v->size);
   *type = v->type;
   *size = v->size;
   return 1;
}

static int
mock_write(void *ctx, unsigned i, const char *name, uint32_t type,
           const void *data, uint32_t size)
{
   struct mock_value *v = mock_slot(ctx, i, name);
   CHECK(size <= sizeof(v->data));
   v->present = true;
   v->type = type;
   v->size = size;
   memcpy(v->data, data, size);
   return 1;
}

static int
mock_remove(void *ctx, unsigned i, const char *name)
{
   struct mock_value *v = mock_slot(ctx, i, name);
   int present = v->present;
   v->present = false;
   return present;
}

static void
test_registry_lifecycle(void)
{
   static const struct vis_registry_ops ops = {mock_read, mock_write, mock_remove};
   struct mock_registry r = {0};
   const uint32_t one = 1, original = 27, third_party = 43;
   const uint8_t path[] = { 'C', 0, ':', 0, 0, 0 };
   const uint8_t old_path[] = { 'D', 0, ':', 0, 0, 0 };
   CHECK(mock_write(&r, 1, NULL, 4, &original, sizeof(original)) == 1);
   CHECK(mock_write(&r, 2, NULL, 2, old_path, sizeof(old_path)) == 1);
   for (unsigned i = 0; i < 3; i++)
      npt_ngx_vis_test_registry(&ops, &r, i, true, i == 2 ? 1 : 4,
                                i == 2 ? (const void *)path : &one,
                                i == 2 ? sizeof(path) : sizeof(one));
   CHECK(r.values[0][0].present && r.values[0][1].present);
   CHECK(r.values[1][4].present && r.values[2][4].present);
   CHECK(mock_write(&r, 1, NULL, 4, &third_party, sizeof(third_party)) == 1);
   for (unsigned i = 0; i < 3; i++)
      npt_ngx_vis_test_registry(&ops, &r, i, true, i == 2 ? 1 : 4,
                                i == 2 ? (const void *)path : &one,
                                i == 2 ? sizeof(path) : sizeof(one));
   CHECK(!memcmp(r.values[1][4].data, &original, sizeof(original)));
   for (unsigned i = 0; i < 3; i++)
      npt_ngx_vis_test_registry(&ops, &r, i, false, i == 2 ? 1 : 4,
                                i == 2 ? (const void *)path : &one,
                                i == 2 ? sizeof(path) : sizeof(one));
   CHECK(!r.values[0][0].present);
   CHECK(r.values[1][0].type == 4 && r.values[1][0].size == sizeof(original) &&
         !memcmp(r.values[1][0].data, &original, sizeof(original)));
   CHECK(r.values[2][0].type == 2 && r.values[2][0].size == sizeof(old_path) &&
         !memcmp(r.values[2][0].data, old_path, sizeof(old_path)));
   for (unsigned i = 0; i < 3; i++)
      for (unsigned j = 1; j < 5; j++)
         CHECK(!r.values[i][j].present);
   npt_ngx_vis_test_registry(&ops, &r, 0, true, 4, &one, sizeof(one));
   CHECK(mock_write(&r, 0, NULL, 4, &third_party, sizeof(third_party)) == 1);
   npt_ngx_vis_test_registry(&ops, &r, 0, false, 4, &one, sizeof(one));
   CHECK(r.values[0][0].present &&
         !memcmp(r.values[0][0].data, &third_party, sizeof(third_party)));
   CHECK(!r.values[0][1].present);
}

/* ---------- helpers ---------- */

/* Fake System32 directory holding the two payload files; the matching
 * env hashes are computed from the real bytes. */
static char g_sysdir[512];
static char g_core_hex[65];
static char g_nvapi_hex[65];
static char g_mod_core[600];
static char g_mod_nvapi[600];

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
   strcpy(g_sysdir, "/tmp/nptngxvis-idXXXXXX");
   CHECK(mkdtemp(g_sysdir) != NULL);
   write_fixture("nvngx.dll", "npt-core-payload-v2");
   write_fixture("nvapi64.dll", "npt-nvapi-payload-v2");
   hex_of_file("nvngx.dll", g_core_hex);
   hex_of_file("nvapi64.dll", g_nvapi_hex);
   snprintf(g_mod_core, sizeof(g_mod_core), "%s/nvngx.dll", g_sysdir);
   snprintf(g_mod_nvapi, sizeof(g_mod_nvapi), "%s/nvapi64.dll", g_sysdir);
   npt_ngx_vis_test_setup(g_sysdir, NULL, NULL, NULL);
}

static void
set_active()
{
   setenv("BRINEWELL_NGX_SR_CANDIDATE", "1", 1);
   setenv("BRINEWELL_NEPTUNE_BACKEND", "d3dmetal", 1);
   setenv("BRINEWELL_NGX_CORE_SHA256", g_core_hex, 1);
   setenv("BRINEWELL_NGX_NVAPI_SHA256", g_nvapi_hex, 1);
   npt_ngx_vis_test_reset();
}

static void
clear_env()
{
   unsetenv("BRINEWELL_NGX_SR_ACTIVE");
   unsetenv("BRINEWELL_NGX_SR_CANDIDATE");
   unsetenv("BRINEWELL_NEPTUNE_BACKEND");
   unsetenv("BRINEWELL_NGX_CORE_SHA256");
   unsetenv("BRINEWELL_NGX_NVAPI_SHA256");
   npt_ngx_vis_test_reset();
}

/* struct npt_adapter_aux layout is private to the production TU; its
 * first member is the mutex and the caps cache starts zeroed, so a
 * zeroed buffer plus an inited first-member lock is a valid aux. */
static struct npt_com_base *
make_adapter()
{
   struct npt_com_base *com = (struct npt_com_base *)calloc(1, 512);
   com->aux = calloc(1, 4096);
   mtx_init((mtx_t *)com->aux, mtx_plain);
   return com;
}

static bool
desc_has_spoofed(const WCHAR description[128], UINT vendor, UINT device)
{
   static const char want[] = "NVIDIA GeForce RTX 3070";
   for (size_t i = 0; i < sizeof(want) - 1; i++) {
      if (description[i] != (WCHAR)(uint8_t)want[i])
         return false;
   }
   return description[sizeof(want) - 1] == 0 && vendor == 0x10de &&
          device == 0x2484;
}

/* ---------- tests ---------- */

static void
test_desc_spoof()
{
   npt_overrides_dxgi_adapter_init();
   struct npt_idxgiadapter4_client_vtbl *vt =
      &npt_idxgiadapter4_default_vtbl_storage;
   CHECK(vt->GetDesc != NULL && vt->GetDesc1 != NULL &&
         vt->GetDesc2 != NULL && vt->GetDesc3 != NULL);

   /* Inactive: original identity, env gate before any wire call. */
   clear_env();
   struct npt_com_base *com = make_adapter();
   DXGI_ADAPTER_DESC d;
   CHECK(vt->GetDesc(com, &d) == NPT_S_OK);
   CHECK(d.VendorId == 0x1234 && d.DeviceId == 0x5678);
   CHECK(d.Description[0] == 'B');
   CHECK(g_caps_calls == 0);

   /* Candidate set but backend not d3dmetal: unchanged, still no wire
    * call. */
   setenv("BRINEWELL_NGX_SR_CANDIDATE", "1", 1);
   setenv("BRINEWELL_NGX_CORE_SHA256", g_core_hex, 1);
   setenv("BRINEWELL_NGX_NVAPI_SHA256", g_nvapi_hex, 1);
   setenv("BRINEWELL_NEPTUNE_BACKEND", "virgl", 1);
   npt_ngx_vis_test_reset();
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(com, &d) == NPT_S_OK);
   CHECK(d.VendorId == 0x1234);
   CHECK(g_caps_calls == 0);

   /* Fully active: spoofed identity, everything else preserved. */
   set_active();
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(com, &d) == NPT_S_OK);
   CHECK(desc_has_spoofed(d.Description, d.VendorId, d.DeviceId));
   CHECK(d.AdapterLuid.LowPart == 0x0badc0de &&
         d.AdapterLuid.HighPart == 0x12345678);
   CHECK(d.DedicatedVideoMemory == 0x1000);
   CHECK(d.SubSysId == 0xaaaa && d.Revision == 0xbb);
   CHECK(g_caps_calls == 1);

   /* Caps cached per adapter: second call does not re-query. */
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(com, &d) == NPT_S_OK);
   CHECK(g_caps_calls == 1);

   /* All four desc tiers: same spoof, LUID byte-identical, other
    * fields untouched. */
   DXGI_ADAPTER_DESC1 d1;
   DXGI_ADAPTER_DESC2 d2;
   DXGI_ADAPTER_DESC3 d3;
   memset(&d1, 0, sizeof(d1));
   memset(&d2, 0, sizeof(d2));
   memset(&d3, 0, sizeof(d3));
   CHECK(vt->GetDesc1(com, &d1) == NPT_S_OK);
   CHECK(vt->GetDesc2(com, &d2) == NPT_S_OK);
   CHECK(vt->GetDesc3(com, &d3) == NPT_S_OK);
   CHECK(desc_has_spoofed(d1.Description, d1.VendorId, d1.DeviceId));
   CHECK(desc_has_spoofed(d2.Description, d2.VendorId, d2.DeviceId));
   CHECK(desc_has_spoofed(d3.Description, d3.VendorId, d3.DeviceId));
   CHECK(d1.Flags == 7 && d2.Flags == 9);
   CHECK(!memcmp(&d.AdapterLuid, &d1.AdapterLuid, sizeof(LUID)));
   CHECK(!memcmp(&d.AdapterLuid, &d2.AdapterLuid, sizeof(LUID)));
   CHECK(!memcmp(&d.AdapterLuid, &d3.AdapterLuid, sizeof(LUID)));

   /* Negative caps on a fresh adapter: unchanged. */
   g_caps_sr = 0;
   struct npt_com_base *com2 = make_adapter();
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(com2, &d) == NPT_S_OK);
   CHECK(d.VendorId == 0x1234);
   g_caps_sr = 2;
   struct npt_com_base *com_d3d11 = make_adapter();
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(com_d3d11, &d) == NPT_S_OK);
   CHECK(desc_has_spoofed(d.Description, d.VendorId, d.DeviceId));
   g_caps_sr = 1;

   /* Caps transport failure: unchanged. */
   g_caps_result = NPT_SR_RESULT_FAIL;
   struct npt_com_base *com3 = make_adapter();
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(com3, &d) == NPT_S_OK);
   CHECK(d.VendorId == 0x1234);
   g_caps_result = NPT_SR_RESULT_SUCCESS;
}

/* Process-side visibility gates seen through the two consumers in this
 * binary (adapter spoof + nvapi Initialize), in both call orders. */
static void
test_visibility_gates()
{
   typedef int32_t (*status_fn)(void);
   status_fn init = (status_fn)nvapi_QueryInterface(0x0150e828);
   struct npt_idxgiadapter4_client_vtbl *vt =
      &npt_idxgiadapter4_default_vtbl_storage;
   DXGI_ADAPTER_DESC d;

   /* Stale inherited ACTIVE=1 without a candidate: neither consumer
    * enables the NVIDIA-facing path. */
   clear_env();
   setenv("BRINEWELL_NGX_SR_ACTIVE", "1", 1);
   npt_ngx_vis_test_reset();
   struct npt_com_base *a = make_adapter();
   CHECK(vt->GetDesc(a, &d) == NPT_S_OK && d.VendorId == 0x1234);
   CHECK(init() != 0);
   free(a->aux);
   free(a);

   /* One payload DLL replaced (nvapi64.dll content no longer matches
    * the published hash): GetDesc first, then Initialize. */
   set_active();
   write_fixture("nvapi64.dll", "stock-nvapi");
   npt_ngx_vis_test_reset();
   a = make_adapter();
   CHECK(vt->GetDesc(a, &d) == NPT_S_OK && d.VendorId == 0x1234);
   CHECK(init() != 0);
   free(a->aux);
   free(a);

   /* Other file replaced, reversed order: Initialize first, then
    * GetDesc. */
   write_fixture("nvapi64.dll", "npt-nvapi-payload-v2");
   write_fixture("nvngx.dll", "foreign-core");
   npt_ngx_vis_test_reset();
   CHECK(init() != 0);
   a = make_adapter();
   CHECK(vt->GetDesc(a, &d) == NPT_S_OK && d.VendorId == 0x1234);
   free(a->aux);
   free(a);
   write_fixture("nvngx.dll", "npt-core-payload-v2");

   /* Missing payload file: the read fails outright. */
   set_active();
   {
      char missing[600];
      snprintf(missing, sizeof(missing), "%s/nvngx.dll", g_sysdir);
      CHECK(remove(missing) == 0);
   }
   npt_ngx_vis_test_reset();
   CHECK(init() != 0);
   write_fixture("nvngx.dll", "npt-core-payload-v2");

   /* Foreign same-name module outside System32. */
   npt_ngx_vis_test_setup(g_sysdir, "D:\\game\\nvngx.dll", NULL, NULL);
   npt_ngx_vis_test_reset();
   a = make_adapter();
   CHECK(vt->GetDesc(a, &d) == NPT_S_OK && d.VendorId == 0x1234);
   CHECK(init() != 0);
   free(a->aux);
   free(a);
   npt_ngx_vis_test_setup(g_sysdir, NULL, NULL, NULL);

   /* Positive: whichever consumer runs first publishes ACTIVE for the
    * process and both enable. */
   npt_ngx_vis_test_reset();
   CHECK(init() == 0);
   a = make_adapter();
   memset(&d, 0, sizeof(d));
   CHECK(vt->GetDesc(a, &d) == NPT_S_OK);
   CHECK(desc_has_spoofed(d.Description, d.VendorId, d.DeviceId));
   const char *act = getenv("BRINEWELL_NGX_SR_ACTIVE");
   CHECK(act && !strcmp(act, "1"));
   free(a->aux);
   free(a);
   {
      typedef int32_t (*status2_fn)(void);
      status2_fn unload = (status2_fn)nvapi_QueryInterface(0xd22bdd7e);
      CHECK(unload() == 0);
   }
}

typedef int32_t (*nvapi_status_fn)(void);

typedef struct {
   uint32_t version, architecture, implementation, revision;
} test_arch_info;

typedef struct {
   uint32_t version;
   uint8_t low_latency, fs_vrr, cpl_vsync;
   uint32_t interval;
   uint8_t game_sleep, reserved[121];
} test_sleep_status;

_Static_assert(sizeof(test_arch_info) == 16, "architecture info size");
_Static_assert(sizeof(test_sleep_status) == 136, "sleep status size");
_Static_assert(offsetof(test_sleep_status, version) == 0, "version offset");
_Static_assert(offsetof(test_sleep_status, low_latency) == 4, "low latency offset");
_Static_assert(offsetof(test_sleep_status, fs_vrr) == 5, "VRR offset");
_Static_assert(offsetof(test_sleep_status, cpl_vsync) == 6, "vsync offset");
_Static_assert(offsetof(test_sleep_status, interval) == 8, "interval offset");
_Static_assert(offsetof(test_sleep_status, game_sleep) == 12, "game sleep offset");
_Static_assert(offsetof(test_sleep_status, reserved) == 13, "reserved offset");

static void
test_nvapi_extra(void)
{
   typedef int32_t (*arch_fn)(void *, test_arch_info *);
   typedef int32_t (*logical_fn)(void *, void **);
   typedef int32_t (*adapter_fn)(void *, void *);
   typedef int32_t (*name_fn)(void *, char *);
   typedef int32_t (*pci_fn)(void *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
   typedef int32_t (*sleep_fn)(void *, test_sleep_status *);
   typedef int32_t (*enum_fn)(void **, uint32_t *);
   arch_fn arch = (arch_fn)nvapi_QueryInterface(0xd8265d24);
   logical_fn logical = (logical_fn)nvapi_QueryInterface(0xadd604d1);
   adapter_fn adapter = (adapter_fn)nvapi_QueryInterface(0x0ff07fde);
   name_fn name = (name_fn)nvapi_QueryInterface(0xceee8e9f);
   pci_fn pci = (pci_fn)nvapi_QueryInterface(0x2ddfb66e);
   sleep_fn sleep_status = (sleep_fn)nvapi_QueryInterface(0xaef96ca1);
   nvapi_status_fn init = (nvapi_status_fn)nvapi_QueryInterface(0x0150e828);
   nvapi_status_fn unload = (nvapi_status_fn)nvapi_QueryInterface(0xd22bdd7e);
   enum_fn enumerate = (enum_fn)nvapi_QueryInterface(0xe5ac921f);
   CHECK(arch && logical && adapter && name && pci && sleep_status);
   void *gpu[64] = { NULL }, *lgpu = NULL, *lgpu2 = NULL;
   uint32_t count = 0, luid[2], luid2[2], device, subsystem, revision, ext;
   char full_name[64];
   test_arch_info info = { .version = 0x10010 };
   test_sleep_status sleep = { .version = 0x10088 };
   CHECK(arch(&count, &info) == -4);
   CHECK(logical(&count, &lgpu) == -4);
   CHECK(adapter(&count, luid) == -4);
   CHECK(name(&count, full_name) == -4);
   CHECK(pci(&count, &device, &subsystem, &revision, &ext) == -4);
   CHECK(sleep_status(&count, &sleep) == -4);
   set_active();
   CHECK(init() == 0 && enumerate(gpu, &count) == 0 && count == 1);
   CHECK(arch(NULL, &info) == -5 && arch(&count, &info) == -5);
   CHECK(arch(gpu[0], NULL) == -5);
   CHECK(arch(gpu[0], &info) == 0);
   CHECK(info.architecture == 0x170 && info.implementation == 0x4 && info.revision == 0x11);
   info.version = 0x20010;
   CHECK(arch(gpu[0], &info) == 0);
   CHECK(info.architecture == 0x170 && info.implementation == 0x4 && info.revision == 0x11);
   info.version = 0x20014;
   CHECK(arch(gpu[0], &info) == -9);
   CHECK(logical(NULL, &lgpu) == -5 && logical(&count, &lgpu) == -5);
   CHECK(logical(gpu[0], NULL) == -5 && logical(gpu[0], &lgpu) == 0);
   CHECK(logical(gpu[0], &lgpu2) == 0 && lgpu && lgpu == lgpu2);
   CHECK(adapter(NULL, luid) == -5 && adapter(&count, luid) == -5);
   CHECK(adapter(gpu[0], NULL) == -5);
   luid[0] = 0xaaaaaaaa;
   luid[1] = 0xbbbbbbbb;
   CHECK(adapter(gpu[0], luid) == -1);
   CHECK(luid[0] == 0xaaaaaaaa && luid[1] == 0xbbbbbbbb && luid_attempts == 1);
   CHECK(adapter(gpu[0], luid) == 0);
   CHECK(luid[0] == 0x0badc0de && luid[1] == 0x12345678 && luid_attempts == 2);
   CHECK(adapter(gpu[0], luid2) == 0 && !memcmp(luid, luid2, sizeof(luid)));
   CHECK(luid_attempts == 2);
   CHECK(name(NULL, full_name) == -5 && name(&count, full_name) == -5);
   CHECK(name(gpu[0], NULL) == -5 && name(gpu[0], full_name) == 0);
   CHECK(!strcmp(full_name, "NVIDIA GeForce RTX 3070"));
   CHECK(pci(NULL, &device, &subsystem, &revision, &ext) == -5);
   CHECK(pci(&count, &device, &subsystem, &revision, &ext) == -5);
   CHECK(pci(gpu[0], NULL, &subsystem, &revision, &ext) == -5);
   CHECK(pci(gpu[0], &device, NULL, &revision, &ext) == -5);
   CHECK(pci(gpu[0], &device, &subsystem, NULL, &ext) == -5);
   CHECK(pci(gpu[0], &device, &subsystem, &revision, NULL) == -5);
   CHECK(pci(gpu[0], &device, &subsystem, &revision, &ext) == 0);
   CHECK(device == 0x248410de && subsystem == 0 && revision == 0xa1 && ext == 0x2484);
   CHECK(sleep_status(NULL, &sleep) == -5 && sleep_status(&count, NULL) == -5);
   sleep.version = 0x10010;
   CHECK(sleep_status(&count, &sleep) == -9);
   memset(&sleep, 0xff, sizeof(sleep));
   sleep.version = 0x10088;
   CHECK(sleep_status(&count, &sleep) == 0);
   CHECK(!sleep.low_latency && !sleep.fs_vrr && !sleep.cpl_vsync &&
         !sleep.interval && !sleep.game_sleep && sleep.reserved[0] == 0xff);
   CHECK(unload() == 0);
}

static void
test_nvapi()
{
   typedef void *(*qi_fn)(uint32_t);
   qi_fn qi = nvapi_QueryInterface;

   CHECK(qi(0x0150e828) != NULL);
   CHECK(qi(0xd22bdd7e) != NULL);
   CHECK(qi(0x2926aaad) != NULL);
   CHECK(qi(0x01053fa5) != NULL);
   CHECK(qi(0x6c2d048c) != NULL);
   CHECK(qi(0xe5ac921f) != NULL);
   CHECK(qi(0x0) == NULL);
   CHECK(qi(0xffffffff) == NULL);
   CHECK(qi(0x33c7358c) == NULL);

   /* Inactive: Initialize fails and EnumPhysicalGPUs refuses. */
   clear_env();
   nvapi_status_fn init = (nvapi_status_fn)qi(0x0150e828);
   nvapi_status_fn unload = (nvapi_status_fn)qi(0xd22bdd7e);
   CHECK(init() != 0);
   /* Unload before any successful Initialize is an error. */
   CHECK(unload() != 0);

   typedef int32_t (*enum_fn)(void **, uint32_t *);
   enum_fn enum_gpus = (enum_fn)qi(0xe5ac921f);
   void *gpus[64] = {NULL};
   uint32_t count = 0;
   CHECK(enum_gpus(gpus, &count) != 0);

   /* Active: one stable process-local handle. */
   set_active();
   CHECK(init() == 0);
   CHECK(enum_gpus(gpus, &count) == 0);
   CHECK(count == 1 && gpus[0] != NULL);
   void *gpus2[64] = {NULL};
   uint32_t count2 = 0;
   CHECK(enum_gpus(gpus2, &count2) == 0);
   CHECK(gpus2[0] == gpus[0]);

   typedef int32_t (*drv_fn)(uint32_t *, char *);
   drv_fn drv = (drv_fn)qi(0x2926aaad);
   uint32_t ver = 0;
   char branch[64];
   CHECK(drv(&ver, branch) == 0);
   CHECK(ver == 57000 && !strcmp(branch, "r570_00"));

   typedef int32_t (*str_fn)(char *);
   str_fn verstr = (str_fn)qi(0x01053fa5);
   char desc[64];
   CHECK(verstr(desc) == 0 && strlen(desc) > 0);

   typedef int32_t (*err_fn)(int32_t, char *);
   err_fn err = (err_fn)qi(0x6c2d048c);
   CHECK(err(0, desc) == 0 && !strcmp(desc, "NVAPI_OK"));

   /* Initialize/Unload are counted pairs: a second Initialize needs a
    * second Unload before the library deinitializes, and Unload past
    * zero is NVAPI_API_NOT_INITIALIZED. */
   nvapi_status_fn unload2 = unload;
   CHECK(init() == 0);
   CHECK(unload2() == 0);
   CHECK(enum_gpus(gpus, &count) == 0);
   CHECK(unload2() == 0);
   CHECK(unload2() == -4);
   CHECK(enum_gpus(gpus, &count) != 0);
   CHECK(init() == 0);
   memset(gpus, 0, sizeof(gpus));
   count = 0;
   CHECK(enum_gpus(gpus, &count) == 0);
   CHECK(count == 1 && gpus[1] == NULL);
   CHECK(unload2() == 0);
}

int
main()
{
   make_sysdir();
   test_desc_spoof();
   test_visibility_gates();
   test_nvapi();
   test_nvapi_extra();
   test_registry_lifecycle();
   clear_env();
   printf("ngx_identity_test: PASS\n");
   return 0;
}
