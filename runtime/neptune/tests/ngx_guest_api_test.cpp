/* nvngx.dll guest API unit test: includes the production translation
 * unit and drives it with a caller-side parameter vtable (declared here
 * independently, like a game would) plus fake COM objects whose bridge
 * tear-offs record what the DLL submitted.  Covers the ABI slots, init
 * aliases, capability keys/callback, create/eval validation, handle
 * tombstoning and failure rollback. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <condition_variable>

#include "../ngx/nvngx.cpp"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
   exit(1); } } while (0)

/* ---------- caller-side parameter vtable (independent declaration) -- */

struct CallerParamVtbl {
   void (*Set_void)(void *, const char *, void *);
   void (*Set_res12)(void *, const char *, ID3D12Resource *);
   void (*Set_res11)(void *, const char *, ID3D11Resource *);
   void (*Set_int)(void *, const char *, int);
   void (*Set_uint)(void *, const char *, unsigned int);
   void (*Set_double)(void *, const char *, double);
   void (*Set_float)(void *, const char *, float);
   void (*Set_ull)(void *, const char *, unsigned long long);
   uint32_t (*Get_void)(void *, const char *, void **);
   uint32_t (*Get_res12)(void *, const char *, ID3D12Resource **);
   uint32_t (*Get_double)(void *, const char *, double *);
   uint32_t (*Get_uint)(void *, const char *, unsigned int *);
   uint32_t (*Get_int)(void *, const char *, int *);
   uint32_t (*Get_res11)(void *, const char *, ID3D11Resource **);
   uint32_t (*Get_float)(void *, const char *, float *);
   uint32_t (*Get_ull)(void *, const char *, unsigned long long *);
   void (*Reset)(void *);
};

struct CallerParams {
   const CallerParamVtbl *vt;
};

/* ---------- fake COM objects + recording bridge --------------------- */

struct FakeObj {
   void *const *vt;
   uint32_t kind;
   uint64_t id;
   uintptr_t cookie;
   void *identity;
   FakeObj *device; /* list -> owning device */
   bool bridge_allowed;
   int refs;
};

static FakeObj *g_dxgi_adapter;
static bool g_allow_d3d12_dxgi;
static int g_qi_calls;
static int g_bridge_addrefs, g_bridge_releases;

static uint32_t g_caps_result = NPT_SR_RESULT_SUCCESS;
static uint32_t g_caps_sr = 1;
static uint32_t g_init_result = NPT_SR_RESULT_SUCCESS;
static uint32_t g_create_result = NPT_SR_RESULT_SUCCESS;
static uint32_t g_eval_result = NPT_SR_RESULT_SUCCESS;
static uint32_t g_release_result = NPT_SR_RESULT_SUCCESS;
static uint32_t g_shutdown_result = NPT_SR_RESULT_SUCCESS;

static uint64_t g_seen_app_id;
static uint32_t g_seen_sdk;
static uint64_t g_seen_create_device_id;
static NptSrCreateV1 g_seen_create;
static uint64_t g_seen_eval_token;
static NptSrEvalIdsV1 g_seen_eval;
static uint64_t g_seen_release_token;
static int g_init_calls, g_create_calls, g_eval_calls, g_release_calls,
   g_shutdown_calls, g_caps_calls;
static uint64_t g_next_token = 0xbeef00;
static int g_releases_at_eval = -1;
static std::mutex g_release_mtx;
static std::condition_variable g_release_cv;
static bool g_pause_release, g_release_entered, g_release_continue;
static bool g_reenter_create;
static uint32_t g_reenter_result = NGX_RESULT_OK;
static void *g_reenter_params;
static void *g_reenter_list;

static NPT_SR_HRESULT NPT_SR_METHODCALLTYPE
fake_bridge_qi(NptNgxBridgeV1 *self, const NptSrGuid *riid, void **out)
{
   (void)self;
   if (!out)
      return NPT_SR_E_POINTER;
   *out = nullptr;
   if (!riid)
      return NPT_SR_E_POINTER;
   return NPT_SR_E_NOINTERFACE;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_addref(NptNgxBridgeV1 *self)
{
   (void)self;
   return ++g_bridge_addrefs;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_release(NptNgxBridgeV1 *self)
{
   (void)self;
   return ++g_bridge_releases;
}

struct FakeBridge {
   NptNgxBridgeV1 base;
   FakeObj *owner;
};

static NPT_SR_HRESULT NPT_SR_METHODCALLTYPE
fake_bridge_get_ref(NptNgxBridgeV1 *self, uint32_t *kind,
                    uintptr_t *cookie, uint64_t *object_id)
{
   FakeBridge *b = (FakeBridge *)self;
   if (!kind || !cookie || !object_id)
      return NPT_SR_E_POINTER;
   *kind = b->owner->kind;
   *cookie = b->owner->cookie;
   *object_id = b->owner->id;
   return NPT_SR_S_OK;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_caps(NptNgxBridgeV1 *self, uint32_t *sr)
{
   (void)self;
   ++g_caps_calls;
   if (!sr)
      return NPT_SR_RESULT_INVALID_PARAMETER;
   *sr = g_caps_sr;
   return g_caps_result;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_init(NptNgxBridgeV1 *self, uint64_t app, uint32_t sdk)
{
   (void)self;
   ++g_init_calls;
   g_seen_app_id = app;
   g_seen_sdk = sdk;
   return g_init_result;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_create(NptNgxBridgeV1 *self, uint64_t device_id,
                   const NptSrCreateV1 *p, uint64_t *token)
{
   (void)self;
   ++g_create_calls;
   g_seen_create_device_id = device_id;
   g_seen_create = *p;
   *token = ++g_next_token;
   return g_create_result;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_eval(NptNgxBridgeV1 *self, uint64_t token,
                 const NptSrEvalIdsV1 *p)
{
   (void)self;
   ++g_eval_calls;
   g_seen_eval_token = token;
   g_seen_eval = *p;
   g_releases_at_eval = g_bridge_releases;
   return g_eval_result;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_release_feature(NptNgxBridgeV1 *self, uint64_t token)
{
   (void)self;
   ++g_release_calls;
   g_seen_release_token = token;
   {
      std::unique_lock<std::mutex> lock(g_release_mtx);
      if (g_pause_release) {
         g_release_entered = true;
         g_release_cv.notify_one();
         g_release_cv.wait(lock, [] { return g_release_continue; });
      }
   }
   if (g_reenter_create) {
      /* Reentrancy probe: a CreateFeature attempted while a Shutdown
       * holds the session in SHUTTING_DOWN must not reach the wire. */
      unsigned int *reh = nullptr;
      g_reenter_result = NVSDK_NGX_D3D12_CreateFeature(
         (ID3D12GraphicsCommandList *)g_reenter_list, 1,
         g_reenter_params, &reh);
   }
   return g_release_result;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_bridge_shutdown(NptNgxBridgeV1 *self)
{
   (void)self;
   ++g_shutdown_calls;
   return g_shutdown_result;
}

static const NptNgxBridgeV1Vtbl fake_bridge_vtbl = {
   fake_bridge_qi,        fake_bridge_addref, fake_bridge_release,
   fake_bridge_get_ref,   fake_bridge_caps,   fake_bridge_init,
   fake_bridge_create,    fake_bridge_eval,
   fake_bridge_release_feature, fake_bridge_shutdown,
};

static int32_t NPT_SR_METHODCALLTYPE
fake_qi(FakeObj *self, const NptSrGuid *riid, void **out)
{
   ++g_qi_calls;
   if (!out)
      return NPT_SR_E_POINTER;
   *out = nullptr;
   if (!riid)
      return NPT_SR_E_POINTER;
   static const NptSrGuid dxgi_device_iid = {
      {0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
       0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
   if ((self->kind == NPT_NGX_BRIDGE_KIND_D3D11_DEVICE ||
        (g_allow_d3d12_dxgi && self->kind == NPT_SR_KIND_DEVICE)) &&
       memcmp(riid, &dxgi_device_iid, sizeof(*riid)) == 0) {
      ++self->refs;
      *out = self;
      return NPT_SR_S_OK;
   }
   if (memcmp(riid, &NPT_SR_IID_IUNKNOWN, sizeof(*riid)) == 0) {
      ++self->refs;
      *out = self->identity;
      return NPT_SR_S_OK;
   }
   if (memcmp(riid, &NPT_SR_IID_NGX_BRIDGE_V1, sizeof(*riid)) == 0 &&
       self->bridge_allowed) {
      FakeBridge *b = new FakeBridge();
      b->base.lpVtbl = &fake_bridge_vtbl;
      b->owner = self;
      *out = b;
      return NPT_SR_S_OK;
   }
   return NPT_SR_E_NOINTERFACE;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_addref(FakeObj *self)
{
   return ++self->refs;
}

static uint32_t NPT_SR_METHODCALLTYPE
fake_release(FakeObj *self)
{
   return --self->refs;
}

static int32_t NPT_SR_METHODCALLTYPE
fake_passthrough(FakeObj *self)
{
   (void)self;
   return 0;
}

static int32_t NPT_SR_METHODCALLTYPE
fake_get_device(FakeObj *self, const NptSrGuid *riid, void **out)
{
   (void)riid;
   if (!out)
      return NPT_SR_E_POINTER;
   *out = nullptr;
   if (!self->device)
      return NPT_SR_E_NOINTERFACE;
   ++self->device->refs;
   *out = self->device;
   return NPT_SR_S_OK;
}

static int32_t NPT_SR_METHODCALLTYPE
fake_get_adapter(FakeObj *self, void **out)
{
   (void)self;
   *out = g_dxgi_adapter;
   if (!g_dxgi_adapter)
      return NPT_SR_E_NOINTERFACE;
   ++g_dxgi_adapter->refs;
   return NPT_SR_S_OK;
}

static void NPT_SR_METHODCALLTYPE
fake_get_device11(FakeObj *self, void **out)
{
   *out = self->device;
   if (self->device)
      ++self->device->refs;
}

static void *const fake_device11_vtbl[] = {
   (void *)fake_qi, (void *)fake_addref, (void *)fake_release,
   (void *)fake_passthrough, (void *)fake_passthrough,
   (void *)fake_passthrough, (void *)fake_passthrough, (void *)fake_get_adapter,
};
static void *const fake_context11_vtbl[] = {
   (void *)fake_qi, (void *)fake_addref, (void *)fake_release,
   (void *)fake_get_device11,
};

static void *const fake_obj_vtbl[] = {
   (void *)fake_qi,
   (void *)fake_addref,
   (void *)fake_release,
   (void *)fake_passthrough, /* GetPrivateData */
   (void *)fake_passthrough, /* SetPrivateData */
   (void *)fake_passthrough, /* SetPrivateDataInterface */
   (void *)fake_passthrough, /* SetName */
   (void *)fake_get_device,  /* GetDevice, slot 7 */
};

/* Canonical identities are themselves releasable COM objects. */
static FakeObj g_id_a = {fake_obj_vtbl, 0, 0, 0, nullptr, nullptr,
                         false, 0};
static FakeObj g_id_b = {fake_obj_vtbl, 0, 0, 0, nullptr, nullptr,
                         false, 0};

static FakeObj g_device = {fake_obj_vtbl, NPT_SR_KIND_DEVICE, 0x100,
                           0xabc, &g_id_a, nullptr, true, 0};
static FakeObj g_device_b = {fake_obj_vtbl, NPT_SR_KIND_DEVICE, 0x200,
                             0xdef, &g_id_b, nullptr, true, 0};
static FakeObj g_list = {fake_obj_vtbl, NPT_SR_KIND_LIST, 0x300,
                         0xabc, &g_id_b, &g_device, true, 0};
static FakeObj g_color = {fake_obj_vtbl, NPT_SR_KIND_RESOURCE, 0x401,
                          0xabc, &g_id_a, nullptr, true, 0};
static FakeObj g_output = {fake_obj_vtbl, NPT_SR_KIND_RESOURCE, 0x402,
                           0xabc, &g_id_a, nullptr, true, 0};
static FakeObj g_depth = {fake_obj_vtbl, NPT_SR_KIND_RESOURCE, 0x403,
                          0xabc, &g_id_a, nullptr, true, 0};
static FakeObj g_motion = {fake_obj_vtbl, NPT_SR_KIND_RESOURCE, 0x404,
                           0xabc, &g_id_a, nullptr, true, 0};
static FakeObj g_foreign = {fake_obj_vtbl, NPT_SR_KIND_RESOURCE, 0x500,
                            0xdef, &g_id_b, nullptr, true, 0};
static FakeObj g_adapter = {fake_obj_vtbl, NPT_SR_KIND_ADAPTER, 0x700,
                            0xabc, &g_id_a, nullptr, true, 0};

static void
reset_counters()
{
   g_init_calls = g_create_calls = g_eval_calls = 0;
   g_release_calls = g_shutdown_calls = g_caps_calls = 0;
   g_bridge_addrefs = g_bridge_releases = 0;
   g_caps_result = g_init_result = g_create_result = NPT_SR_RESULT_SUCCESS;
   g_eval_result = g_release_result = g_shutdown_result =
      NPT_SR_RESULT_SUCCESS;
   g_caps_sr = 1;
   g_releases_at_eval = -1;
   g_reenter_create = false;
   g_reenter_result = NGX_RESULT_OK;
   memset(&g_seen_create, 0, sizeof(g_seen_create));
   memset(&g_seen_eval, 0, sizeof(g_seen_eval));
}

/* ------------------------------------------------------------------ */

static void
test_param_slots()
{
   void *raw = nullptr;
   CHECK(NVSDK_NGX_D3D12_AllocateParameters(&raw) == NGX_RESULT_OK);
   CallerParams *p = (CallerParams *)raw;
   CHECK(p->vt->Set_uint != nullptr && p->vt->Get_ull != nullptr);

   /* Set via each caller-declared slot; read back via the matching and
    * the interoperable slots. */
   p->vt->Set_uint(p, "u", 42);
   unsigned int u = 0;
   int i = 0;
   CHECK(p->vt->Get_uint(p, "u", &u) == NGX_RESULT_OK && u == 42);
   CHECK(p->vt->Get_int(p, "u", &i) == NGX_RESULT_OK && i == 42);

   p->vt->Set_int(p, "i", -7);
   CHECK(p->vt->Get_int(p, "i", &i) == NGX_RESULT_OK && i == -7);
   CHECK(p->vt->Get_uint(p, "i", &u) == NGX_RESULT_OK &&
         u == (unsigned)-7);

   p->vt->Set_float(p, "f", 1.5f);
   float f = 0;
   CHECK(p->vt->Get_float(p, "f", &f) == NGX_RESULT_OK && f == 1.5f);
   CHECK(p->vt->Get_int(p, "f", &i) == NGX_RESULT_OK && i == 1);

   p->vt->Set_double(p, "d", 2.25);
   double d = 0;
   CHECK(p->vt->Get_double(p, "d", &d) == NGX_RESULT_OK && d == 2.25);
   CHECK(p->vt->Get_float(p, "d", &f) == NGX_RESULT_OK && f == 2.25f);

   p->vt->Set_ull(p, "ull", 0x1122334455667788ull);
   unsigned long long ull = 0;
   CHECK(p->vt->Get_ull(p, "ull", &ull) == NGX_RESULT_OK &&
         ull == 0x1122334455667788ull);
   void *vp = nullptr;
   CHECK(p->vt->Get_void(p, "ull", &vp) == NGX_RESULT_OK &&
         (uintptr_t)vp == 0x1122334455667788ull);

   const double invalid_numbers[] = {NAN, INFINITY, -INFINITY, -1e30, 1e30};
   for (double value : invalid_numbers) {
      p->vt->Set_double(p, "invalid", value);
      CHECK(p->vt->Get_int(p, "invalid", &i) == NGX_RESULT_FAIL);
      CHECK(p->vt->Get_uint(p, "invalid", &u) == NGX_RESULT_FAIL);
      CHECK(p->vt->Get_ull(p, "invalid", &ull) == NGX_RESULT_FAIL);
      p->vt->Set_float(p, "invalid", (float)value);
      CHECK(p->vt->Get_int(p, "invalid", &i) == NGX_RESULT_FAIL);
      CHECK(p->vt->Get_uint(p, "invalid", &u) == NGX_RESULT_FAIL);
      CHECK(p->vt->Get_ull(p, "invalid", &ull) == NGX_RESULT_FAIL);
   }
   p->vt->Set_double(p, "boundary", (double)INT_MAX);
   CHECK(p->vt->Get_int(p, "boundary", &i) == NGX_RESULT_OK && i == INT_MAX);
   p->vt->Set_double(p, "boundary", (double)INT_MAX + 1.0);
   CHECK(p->vt->Get_int(p, "boundary", &i) == NGX_RESULT_FAIL);
   p->vt->Set_double(p, "boundary", (double)INT_MIN);
   CHECK(p->vt->Get_int(p, "boundary", &i) == NGX_RESULT_OK && i == INT_MIN);
   p->vt->Set_double(p, "boundary", (double)INT_MIN - 1.0);
   CHECK(p->vt->Get_int(p, "boundary", &i) == NGX_RESULT_FAIL);
   p->vt->Set_double(p, "boundary", (double)UINT_MAX);
   CHECK(p->vt->Get_uint(p, "boundary", &u) == NGX_RESULT_OK && u == UINT_MAX);
   p->vt->Set_double(p, "boundary", (double)UINT_MAX + 1.0);
   CHECK(p->vt->Get_uint(p, "boundary", &u) == NGX_RESULT_FAIL);
   p->vt->Set_double(p, "boundary", -0.5);
   CHECK(p->vt->Get_uint(p, "boundary", &u) == NGX_RESULT_FAIL);
   p->vt->Set_double(p, "boundary", 18446744073709551616.0);
   CHECK(p->vt->Get_ull(p, "boundary", &ull) == NGX_RESULT_FAIL);

   /* resource-as-void both ways */
   ID3D12Resource *res = (ID3D12Resource *)&g_color;
   p->vt->Set_res12(p, "res", res);
   ID3D12Resource *res2 = nullptr;
   CHECK(p->vt->Get_res12(p, "res", &res2) == NGX_RESULT_OK &&
         res2 == res);
   CHECK(p->vt->Get_void(p, "res", &vp) == NGX_RESULT_OK && vp == res);
   p->vt->Set_void(p, "resv", (void *)res);
   res2 = nullptr;
   CHECK(p->vt->Get_res12(p, "resv", &res2) == NGX_RESULT_OK &&
         res2 == res);

   /* missing key and wrong-type reads fail */
   CHECK(p->vt->Get_uint(p, "absent", &u) == NGX_RESULT_FAIL);
   CHECK(p->vt->Get_res12(p, "f", &res2) == NGX_RESULT_FAIL);

   p->vt->Reset(p);
   CHECK(p->vt->Get_uint(p, "u", &u) == NGX_RESULT_FAIL);

   CHECK(NVSDK_NGX_D3D12_DestroyParameters(raw) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_AllocateParameters(nullptr) ==
         NGX_RESULT_INVALID_PARAMETER);
}

static void
test_init_and_requirements()
{
   reset_counters();

   /* No session yet: eval-style calls fail rather than crash. */
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, nullptr, nullptr,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);

   /* Device without a bridge -> not supported. */
   FakeObj plain = {fake_obj_vtbl, NPT_SR_KIND_DEVICE, 9, 9,
                    &g_id_b, nullptr, false, 0};
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&plain,
                              nullptr, 1) == NGX_RESULT_NOT_SUPPORTED);
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, nullptr, nullptr, 1) ==
         NGX_RESULT_INVALID_PARAMETER);

   /* Requirements on the adapter reflect Caps. */
   NGXFeatureDiscoveryInfo info = {};
   info.FeatureID = 1;
   NGXFeatureRequirement req = {};
   CHECK(NVSDK_NGX_D3D12_GetFeatureRequirements(
             (IDXGIAdapter *)&g_adapter, &info, &req) == NGX_RESULT_OK);
   CHECK(req.FeatureSupported == 0 && g_caps_calls == 1);
   CHECK(!strcmp(req.MinOSVersion, "10.0.16299.0"));

   g_caps_sr = 0;
   FakeObj adapter_b = {fake_obj_vtbl, NPT_SR_KIND_ADAPTER, 0x701, 0xabc,
                        &g_id_b, nullptr, true, 0};
   CHECK(NVSDK_NGX_D3D12_GetFeatureRequirements(
             (IDXGIAdapter *)&adapter_b, &info, &req) == NGX_RESULT_OK);
   CHECK(req.FeatureSupported == 1);
   g_caps_sr = 1;

   g_caps_result = NPT_SR_RESULT_FAIL;
   FakeObj adapter_c = {fake_obj_vtbl, NPT_SR_KIND_ADAPTER, 0x702, 0xabc,
                        &g_id_b, nullptr, true, 0};
   CHECK(NVSDK_NGX_D3D12_GetFeatureRequirements(
             (IDXGIAdapter *)&adapter_c, &info, &req) ==
         NGX_RESULT_FAIL);
   CHECK(req.FeatureSupported == 1);
   g_caps_result = NPT_SR_RESULT_SUCCESS;

   info.FeatureID = 13;
   CHECK(NVSDK_NGX_D3D12_GetFeatureRequirements(
             (IDXGIAdapter *)&g_adapter, &info, &req) ==
         NGX_RESULT_NOT_SUPPORTED);
   CHECK(req.FeatureSupported == 1);
   info.FeatureID = 1;

   /* Init, Init_Ext and the two ProjectID orderings all reach the same
    * local init; project variants send application_id 0. */
   CHECK(NVSDK_NGX_D3D12_Init(0xabcdef, nullptr,
                              (ID3D12Device *)&g_device, nullptr, 77) ==
         NGX_RESULT_OK);
   CHECK(g_init_calls == 1 && g_seen_app_id == 0xabcdef &&
         g_seen_sdk == 77);

   /* Same device re-init is idempotent: no new wire Init. */
   CHECK(NVSDK_NGX_D3D12_Init_Ext(9, nullptr,
                                  (ID3D12Device *)&g_device, 2,
                                  nullptr) == NGX_RESULT_OK);
   CHECK(g_init_calls == 1);

   /* Different device while a session lives -> fail. */
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device_b,
                              nullptr, 1) == NGX_RESULT_FAIL);

   /* Capability params after init report availability. */
   void *cap = nullptr;
   CHECK(NVSDK_NGX_D3D12_GetCapabilityParameters(&cap) == NGX_RESULT_OK);
   CallerParams *cp = (CallerParams *)cap;
   unsigned int u = 0;
   CHECK(cp->vt->Get_uint(cp, "SuperSampling.Available", &u) ==
             NGX_RESULT_OK &&
         u == 1);
   char key2[] = {'#', 1, 0};
   u = 0;
   CHECK(cp->vt->Get_uint(cp, key2, &u) == NGX_RESULT_OK && u == 1);
   void *cb = nullptr;
   CHECK(cp->vt->Get_void(cp, "DLSSOptimalSettingsCallback", &cb) ==
             NGX_RESULT_OK &&
         cb != nullptr);
   CHECK(NVSDK_NGX_D3D12_DestroyParameters(cap) == NGX_RESULT_OK);

   /* GetParameters returns the same capability surface. */
   cap = nullptr;
   CHECK(NVSDK_NGX_D3D12_GetParameters(&cap) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_DestroyParameters(cap) == NGX_RESULT_OK);

   /* Shutdown1 on a foreign device fails; on the session device works. */
   CHECK(NVSDK_NGX_D3D12_Shutdown1((ID3D12Device *)&g_device_b) ==
         NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D12_Shutdown1((ID3D12Device *)&g_device) ==
         NGX_RESULT_OK);
   CHECK(g_shutdown_calls == 1);

   /* ProjectID variants establish a session with app_id 0. */
   CHECK(NVSDK_NGX_D3D12_Init_ProjectID("proj", 0, "1.0", nullptr,
                                        (ID3D12Device *)&g_device, 55,
                                        nullptr) == NGX_RESULT_OK);
   CHECK(g_seen_app_id == 0 && g_seen_sdk == 55);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_Init_with_ProjectID("proj", 0, "1.0", nullptr,
                                            (ID3D12Device *)&g_device,
                                            nullptr, 56) ==
         NGX_RESULT_OK);
   CHECK(g_seen_app_id == 0 && g_seen_sdk == 56);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
}

static void
test_optimal_settings_callback()
{
   void *cap = nullptr;
   CHECK(NVSDK_NGX_D3D12_GetCapabilityParameters(&cap) == NGX_RESULT_OK);
   CallerParams *cp = (CallerParams *)cap;
   void *cbv = nullptr;
   CHECK(cp->vt->Get_void(cp, "DLSSOptimalSettingsCallback", &cbv) ==
         NGX_RESULT_OK);
   typedef uint32_t (*cb_t)(void *);
   cb_t cb = (cb_t)cbv;
   /* Callback only ever points into this module's code. */
   CHECK(cb == (cb_t)&ngx_dlss_optimal_settings);

   void *raw = nullptr;
   CHECK(NVSDK_NGX_D3D12_AllocateParameters(&raw) == NGX_RESULT_OK);
   CallerParams *p = (CallerParams *)raw;
   p->vt->Set_uint(p, "Width", 1280);
   p->vt->Set_uint(p, "Height", 720);

   g_sessions[0].extended = 1;
   struct { int q; unsigned w, h; } cases[] = {
      {0, 640, 360}, {1, 743, 418}, {2, 854, 480},
      {3, 427, 240}, {4, 985, 554}, {5, 1280, 720},
   };
   for (auto &c : cases) {
      p->vt->Set_int(p, "PerfQualityValue", c.q);
      CHECK(cb(p) == NGX_RESULT_OK);
      unsigned int w = 0, h = 0, mnw = 0, mxw = 0;
      CHECK(p->vt->Get_uint(p, "OutWidth", &w) == NGX_RESULT_OK &&
            w == c.w);
      CHECK(p->vt->Get_uint(p, "OutHeight", &h) == NGX_RESULT_OK &&
            h == c.h);
      CHECK(p->vt->Get_uint(p, "DLSS.Get.Dynamic.Min.Render.Width",
                            &mnw) == NGX_RESULT_OK &&
            mnw == (c.q == 5 ? 1280u : (c.w < 640u ? c.w : 640u)));
      CHECK(p->vt->Get_uint(p, "DLSS.Get.Dynamic.Max.Render.Width",
                            &mxw) == NGX_RESULT_OK &&
            mxw == 1280);
   }
   g_sessions[0].extended = 0;
   p->vt->Set_int(p, "PerfQualityValue", 3);
   uint32_t unsupported = cb(p);
   CHECK(unsupported == 0xBAD00001u &&
         (unsupported & 0xFFF00000u) == 0xBAD00000u);
   g_sessions[0].extended = -1;
   CHECK(cb(p) == 0xBAD00001u);
   p->vt->Set_int(p, "PerfQualityValue", 0);
   const char *keys[] = {"Width", "Height", "PerfQualityValue"};
   for (const char *key : keys) {
      p->vt->Set_ull(p, key, 0x100000001ULL);
      CHECK(cb(p) == NGX_RESULT_FAIL);
      const double invalid[] = {NAN, INFINITY, -1.0, 1e30, 1.5};
      for (double value : invalid) {
         p->vt->Set_double(p, key, value);
         CHECK(cb(p) == NGX_RESULT_FAIL);
      }
      p->vt->Set_void(p, key, (void *)1);
      CHECK(cb(p) == NGX_RESULT_FAIL);
      p->vt->Set_uint(p, key, 1);
   }
   p->vt->Reset(p);
   CHECK(cb(p) == NGX_RESULT_FAIL);
   CHECK(NVSDK_NGX_D3D12_DestroyParameters(raw) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_DestroyParameters(cap) == NGX_RESULT_OK);
}

static CallerParams *
alloc_params()
{
   void *raw = nullptr;
   CHECK(NVSDK_NGX_D3D12_AllocateParameters(&raw) == NGX_RESULT_OK);
   return (CallerParams *)raw;
}

static void
test_reused_eval_subrect()
{
   reset_counters();
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == NGX_RESULT_OK);
   CallerParams *p = alloc_params();
   p->vt->Set_uint(p, "Width", 1280);
   p->vt->Set_uint(p, "Height", 800);
   p->vt->Set_uint(p, "OutWidth", 1280);
   p->vt->Set_uint(p, "OutHeight", 800);
   p->vt->Set_int(p, "PerfQualityValue", 2);
   p->vt->Set_uint(p, "DLSS.Feature.Create.Flags", 0x2b);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 1280);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", 800);
   p->vt->Set_res12(p, "Color", (ID3D12Resource *)&g_color);
   p->vt->Set_res12(p, "Output", (ID3D12Resource *)&g_output);
   p->vt->Set_res12(p, "Depth", (ID3D12Resource *)&g_depth);
   p->vt->Set_res12(p, "MotionVectors", (ID3D12Resource *)&g_motion);
   unsigned int *handle = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p, nullptr) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(handle) == NGX_RESULT_OK);

   p->vt->Set_uint(p, "Width", 854);
   p->vt->Set_uint(p, "Height", 534);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Base.X", 4);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) == NGX_RESULT_OK);
   CHECK(g_seen_create.in_w == 854 && g_seen_create.in_h == 534);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Base.X", 0);
   int evals_before = g_eval_calls;
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p, nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 854);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", 534);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p, nullptr) == NGX_RESULT_OK);
   CHECK(g_seen_eval.render_w == 854 && g_seen_eval.render_h == 534);
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(handle) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_DestroyParameters(p) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
}

static void
test_create_eval_release()
{
   reset_counters();
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == NGX_RESULT_OK);

   CallerParams *p = alloc_params();
   p->vt->Set_uint(p, "Width", 640);
   p->vt->Set_uint(p, "Height", 360);
   p->vt->Set_uint(p, "OutWidth", 1280);
   p->vt->Set_uint(p, "OutHeight", 720);
   p->vt->Set_int(p, "PerfQualityValue", 0);
   p->vt->Set_uint(p, "DLSS.Feature.Create.Flags", 0x02);

   /* Unsupported feature id. */
   unsigned int *handle = (unsigned int *)0xdead;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 13, p, &handle) ==
         NGX_RESULT_NOT_SUPPORTED);
   CHECK(handle == nullptr);

   /* Unsupported create flag bit. */
   p->vt->Set_uint(p, "DLSS.Feature.Create.Flags", 0x02 | 0x100);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_NOT_SUPPORTED);
   p->vt->Set_uint(p, "DLSS.Feature.Create.Flags", 0x02);

   /* Unsupported quality. */
   p->vt->Set_int(p, "PerfQualityValue", 3);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_NOT_SUPPORTED);
   p->vt->Set_int(p, "PerfQualityValue", 0);

   p->vt->Set_ull(p, "DLSS.Feature.Create.Flags", 0x100000001ULL);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_void(p, "DLSS.Feature.Create.Flags", (void *)1);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_uint(p, "DLSS.Feature.Create.Flags", 2);
   p->vt->Set_ull(p, "Width", 0x100000280ULL);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_int(p, "Width", -1);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_double(p, "Width", NAN);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_double(p, "Width", INFINITY);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_double(p, "Width", 640.5);
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) != NGX_RESULT_OK);
   p->vt->Set_uint(p, "Width", 640);
   p->vt->Set_void(p, "DLSS.ScratchBuffer", (void *)1);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_INVALID_PARAMETER);
   p->vt->Set_void(p, "DLSS.ScratchBuffer", nullptr);
   p->vt->Set_uint(p, "CreationNodeMask", 2);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_NOT_SUPPORTED);
   p->vt->Set_uint(p, "CreationNodeMask", 1);

   /* Foreign params object (not ours) rejected without deref. */
   CallerParams foreign = {nullptr};
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, &foreign,
             &handle) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, nullptr,
             &handle) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, nullptr) ==
         NGX_RESULT_INVALID_PARAMETER);

   /* Real create: wire fields + token. */
   p->vt->Set_uint(p, "DLSS.Feature.Create.Flags", 0xff);
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_OK);
   CHECK(handle != nullptr && g_create_calls == 1);
   CHECK(g_seen_create_device_id == 0x100);
   CHECK(g_seen_create.in_w == 640 && g_seen_create.in_h == 360);
   CHECK(g_seen_create.out_w == 1280 && g_seen_create.out_h == 720);
   CHECK(g_seen_create.quality == 0 && g_seen_create.flags == 0xff);
   const uint64_t token = g_next_token;

   /* Eval without required resources fails before any wire call. */
   int evals_before = g_eval_calls;
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before);

   /* Non-empty progress callback unsupported. */
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             (void *)1) == NGX_RESULT_NOT_SUPPORTED);

   p->vt->Set_res12(p, "Color", (ID3D12Resource *)&g_color);
   p->vt->Set_res12(p, "Output", (ID3D12Resource *)&g_output);
   p->vt->Set_res12(p, "Depth", (ID3D12Resource *)&g_depth);
   p->vt->Set_void(p, "MotionVectors", (void *)&g_motion);
   p->vt->Set_float(p, "Jitter.Offset.X", 0.25f);
   p->vt->Set_float(p, "Jitter.Offset.Y", -0.5f);
   p->vt->Set_float(p, "MV.Scale.X", 0.5f);
   p->vt->Set_float(p, "DLSS.Pre.Exposure", 0.0f);
   p->vt->Set_int(p, "Reset", 1);

   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_OK);
   CHECK(g_eval_calls == 1 && g_seen_eval_token == token);
   CHECK(g_seen_eval.color == 0x401 && g_seen_eval.output == 0x402);
   CHECK(g_seen_eval.depth == 0x403 && g_seen_eval.motion == 0x404);
   CHECK(g_seen_eval.exposure == 0);
   CHECK(g_seen_eval.render_w == 640 && g_seen_eval.render_h == 360);
   CHECK(g_seen_eval.jitter_x == 0.25f &&
         g_seen_eval.jitter_y == -0.5f);
   CHECK(g_seen_eval.mv_scale_x == 0.5f && g_seen_eval.mv_scale_y == 1.0f);
   CHECK(g_seen_eval.pre_exposure == 1.0f && g_seen_eval.reset == 1);

   /* Resource tear-offs stay referenced until the synchronous submit
    * ends: the list bridge plus the four resolved resource bridges are
    * released only after fake_eval ran. */
   CHECK(g_releases_at_eval >= 0 &&
         g_bridge_releases == g_releases_at_eval + 5);

   int evals_before2 = g_eval_calls;
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Base.X", 3);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before2);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Base.X", 0);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 320);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", 180);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_OK);
   CHECK(g_eval_calls == evals_before2 + 1);
   CHECK(g_seen_eval.render_w == 320 && g_seen_eval.render_h == 180);
   evals_before2 = g_eval_calls;

   unsigned int *reuse = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &reuse) == NGX_RESULT_OK);
   CHECK(g_seen_create.in_w == 640 && g_seen_create.in_h == 360);
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(reuse) == NGX_RESULT_OK);
   const char *base_keys[] = {
      "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
      "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y",
      "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
      "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y",
      "DLSS.Input.Exposure.Subrect.Base.X", "DLSS.Input.Exposure.Subrect.Base.Y",
   };
   for (const char *key : base_keys) {
      p->vt->Set_uint(p, key, 1);
      CHECK(NVSDK_NGX_D3D12_EvaluateFeature((ID3D12GraphicsCommandList *)&g_list, handle, p, nullptr) == NGX_RESULT_INVALID_PARAMETER);
      p->vt->Set_void(p, key, (void *)1);
      CHECK(NVSDK_NGX_D3D12_EvaluateFeature((ID3D12GraphicsCommandList *)&g_list, handle, p, nullptr) == NGX_RESULT_INVALID_PARAMETER);
      p->vt->Set_uint(p, key, 0);
   }
   CHECK(g_eval_calls == evals_before2);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature((ID3D12GraphicsCommandList *)&g_list, handle, p, nullptr) == NGX_RESULT_OK);
   evals_before2 = g_eval_calls;

   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 0);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before2);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 641);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before2);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 320);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", 0);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before2);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", 361);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_eval_calls == evals_before2);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", 640);
   p->vt->Set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", 360);
   p->vt->Set_int(p, "DLSS.Enable.Output.Subrects", 1);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_NOT_SUPPORTED);
   p->vt->Set_int(p, "DLSS.Enable.Output.Subrects", 0);

   /* A present key that does not convert is a type error, not unset. */
   p->vt->Set_void(p, "MV.Scale.X", (void *)1);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   p->vt->Set_float(p, "MV.Scale.X", 0.5f);
   CHECK(g_eval_calls == evals_before2);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_OK);

   /* A handle pointer outside the feature table is rejected without
    * dereference. */
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list,
             (unsigned int *)0xdeadcafe, p, nullptr) ==
         NGX_RESULT_INVALID_PARAMETER);

   /* Foreign resource: cookie mismatch -> invalid parameter. */
   p->vt->Set_res12(p, "Color", (ID3D12Resource *)&g_foreign);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature_C(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   p->vt->Set_res12(p, "Color", (ID3D12Resource *)&g_color);

   /* Release, then the stale handle must not resolve. */
   const unsigned int stale_word = *handle;
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(handle) == NGX_RESULT_OK);
   CHECK(g_release_calls == 2 && g_seen_release_token == token);
   unsigned int *stale = handle;
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(stale) ==
         NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, stale, p, nullptr) ==
         NGX_RESULT_INVALID_PARAMETER);

   /* A second create may reuse the released slot, but the generation
    * encoded in the handle word must differ so the stale handle never
    * resolves to the new feature. */
   unsigned int *handle2 = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle2) ==
         NGX_RESULT_OK);
   CHECK(*handle2 != stale_word);

   /* Wire create failure -> no handle, nothing registered. */
   g_create_result = NPT_SR_RESULT_FAIL;
   handle = (unsigned int *)0xdead;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_FAIL);
   CHECK(handle == nullptr);
   g_create_result = NPT_SR_RESULT_SUCCESS;

   /* Shutdown releases the live feature, then the session is gone. */
   int rel_before = g_release_calls;
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
   CHECK(g_release_calls == rel_before + 1);
   CHECK(g_shutdown_calls == 1);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle2, p,
             nullptr) == NGX_RESULT_FAIL);

   CHECK(NVSDK_NGX_D3D12_DestroyParameters(p) == NGX_RESULT_OK);
}

static CallerParams *
alloc_create_params()
{
   CallerParams *p = alloc_params();
   p->vt->Set_uint(p, "Width", 640);
   p->vt->Set_uint(p, "Height", 360);
   p->vt->Set_uint(p, "OutWidth", 1280);
   p->vt->Set_uint(p, "OutHeight", 720);
   p->vt->Set_res12(p, "Color", (ID3D12Resource *)&g_color);
   p->vt->Set_res12(p, "Output", (ID3D12Resource *)&g_output);
   p->vt->Set_res12(p, "Depth", (ID3D12Resource *)&g_depth);
   p->vt->Set_res12(p, "MotionVectors", (ID3D12Resource *)&g_motion);
   return p;
}

/* I2: guest/host state must not diverge.  Release commits invalidation
 * only on confirmed success; Shutdown enters SHUTTING_DOWN, blocks new
 * work, and rolls back on explicit rejection while treating transport
 * loss as session death. */
static void
test_release_and_shutdown_states()
{
   reset_counters();
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == NGX_RESULT_OK);
   CallerParams *p = alloc_create_params();

   unsigned int *handle = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle) ==
         NGX_RESULT_OK);

   /* Explicit backend rejection: the handle stays live and retryable. */
   g_release_result = NPT_SR_RESULT_FAIL;
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(handle) == NGX_RESULT_FAIL);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle, p,
             nullptr) == NGX_RESULT_OK);

   /* Transport loss: result unknown, handle still retryable. */
   g_release_result = NPT_SR_RESULT_TRANSPORT_LOST;
   uint32_t lost = NVSDK_NGX_D3D12_ReleaseFeature(handle);
   CHECK(lost == 0xBAD00000u && (lost & 0xFFF00000u) == 0xBAD00000u);
   g_release_result = NPT_SR_RESULT_SUCCESS;
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(handle) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(handle) ==
         NGX_RESULT_INVALID_PARAMETER);

   /* Shutdown explicitly rejected: successfully released features stay
    * dead, the session stays live, new work is accepted. */
   unsigned int *handle2 = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle2) ==
         NGX_RESULT_OK);
   g_shutdown_result = 0xBAD00005u;
   const int rel_before = g_release_calls;
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_INVALID_PARAMETER);
   CHECK(g_release_calls == rel_before + 1 && g_shutdown_calls == 1);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle2, p,
             nullptr) == NGX_RESULT_INVALID_PARAMETER);
   unsigned int *handle3 = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle3) ==
         NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle3, p,
             nullptr) == NGX_RESULT_OK);
   g_shutdown_result = NPT_SR_RESULT_SUCCESS;
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);

   /* Transport loss during shutdown: the session is unusable but the
    * DLL recovers cleanly and a fresh session can start. */
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == NGX_RESULT_OK);
   unsigned int *handle4 = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle4) ==
         NGX_RESULT_OK);
   g_shutdown_result = NPT_SR_RESULT_TRANSPORT_LOST;
   lost = NVSDK_NGX_D3D12_Shutdown();
   CHECK(lost == 0xBAD00000u && (lost & 0xFFF00000u) == 0xBAD00000u);
   CHECK(NVSDK_NGX_D3D12_EvaluateFeature(
             (ID3D12GraphicsCommandList *)&g_list, handle4, p,
             nullptr) == NGX_RESULT_FAIL);
   g_shutdown_result = NPT_SR_RESULT_SUCCESS;
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == NGX_RESULT_OK);

   /* While SHUTTING_DOWN, a new CreateFeature must not reach the wire. */
   unsigned int *handle5 = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &handle5) ==
         NGX_RESULT_OK);
   g_reenter_list = &g_list;
   g_reenter_params = p;
   g_reenter_create = true;
   const int creates_before = g_create_calls;
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
   CHECK(g_reenter_result == NGX_RESULT_FAIL);
   CHECK(g_create_calls == creates_before);
   g_reenter_create = false;

   /* The live-feature table is bounded. */
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == NGX_RESULT_OK);
   unsigned int *many[NGX_MAX_FEATURES + 1] = {};
   for (unsigned i = 0; i < NGX_MAX_FEATURES; i++) {
      CHECK(NVSDK_NGX_D3D12_CreateFeature(
                (ID3D12GraphicsCommandList *)&g_list, 1, p, &many[i]) ==
            NGX_RESULT_OK);
   }
   CHECK(NVSDK_NGX_D3D12_CreateFeature(
             (ID3D12GraphicsCommandList *)&g_list, 1, p, &many[64]) ==
         NGX_RESULT_FAIL);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);

   CHECK(NVSDK_NGX_D3D12_DestroyParameters(p) == NGX_RESULT_OK);
}

static void
test_release_generation_race()
{
   reset_counters();
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device,
                              nullptr, 1) == 1u);
   CallerParams *p = alloc_create_params();
   p->vt->Set_int(p, "PerfQualityValue", 3);
   unsigned int *unsupported_handle = nullptr;
   uint32_t unsupported = NVSDK_NGX_D3D12_CreateFeature(
      (ID3D12GraphicsCommandList *)&g_list, 1, p, &unsupported_handle);
   CHECK(unsupported == 0xBAD00001u &&
         (unsupported & 0xFFF00000u) == 0xBAD00000u && !unsupported_handle);
   p->vt->Set_int(p, "PerfQualityValue", 0);
   FakeObj other = {fake_obj_vtbl, NPT_SR_KIND_DEVICE, 9, 9,
                    &g_id_b, nullptr, false, 0};
   CHECK(NVSDK_NGX_D3D12_Shutdown1((ID3D12Device *)&other) == 0xBAD00005u);
   CHECK(g_sessions[0].state == NGX_SESSION_LIVE);
   for (int field = 0; field < 4; ++field) {
      unsigned int *h = nullptr;
      CHECK(NVSDK_NGX_D3D12_CreateFeature(
               (ID3D12GraphicsCommandList *)&g_list, 1, p, &h) == 1u);
      NgxFeature *f = (NgxFeature *)h;
      {
         std::lock_guard<std::mutex> lock(g_release_mtx);
         g_pause_release = true;
         g_release_entered = g_release_continue = false;
      }
      uint32_t result = 0;
      std::thread release([&] { result = NVSDK_NGX_D3D12_ReleaseFeature(h); });
      {
         std::unique_lock<std::mutex> lock(g_release_mtx);
         g_release_cv.wait(lock, [] { return g_release_entered; });
      }
      CHECK(NVSDK_NGX_D3D12_Shutdown1((ID3D12Device *)&g_device) == 0xBAD00000u);
      {
         std::lock_guard<std::mutex> lock(g_mtx);
         if (field == 0) ++f->session_gen;
         if (field == 1) ++f->feature_gen;
         if (field == 2) ++f->token;
         if (field == 3) f->api = 1;
      }
      {
         std::lock_guard<std::mutex> lock(g_release_mtx);
         g_release_continue = true;
         g_release_cv.notify_one();
      }
      release.join();
      CHECK(result == 1u);
      {
         std::lock_guard<std::mutex> lock(g_mtx);
         CHECK(f->state == NGX_FEATURE_RELEASING);
         f->api = 0;
         ngx_feature_free_locked(f);
      }
      g_pause_release = false;
   }
   CHECK(NVSDK_NGX_D3D12_Shutdown1((ID3D12Device *)&g_device) == 1u);
   CHECK(NVSDK_NGX_D3D12_DestroyParameters(p) == 1u);
}

static void
test_scratch_and_shutdown1()
{
   size_t sz = 9;
   CHECK(NVSDK_NGX_D3D12_GetScratchBufferSize(1, nullptr, &sz) ==
             NGX_RESULT_OK &&
         sz == 0);
   CHECK(NVSDK_NGX_D3D12_GetScratchBufferSize(1, nullptr, nullptr) ==
         NGX_RESULT_INVALID_PARAMETER);
   /* No session: Shutdown1 on a device is a no-op success. */
   FakeObj plain = {fake_obj_vtbl, NPT_SR_KIND_DEVICE, 9, 9,
                    &g_id_a, nullptr, false, 0};
   CHECK(NVSDK_NGX_D3D12_Shutdown1((ID3D12Device *)&plain) ==
         NGX_RESULT_OK);
}

static void
test_d3d11_session()
{
   reset_counters();
   g_dxgi_adapter = &g_adapter;
   FakeObj dev = {fake_device11_vtbl, NPT_NGX_BRIDGE_KIND_D3D11_DEVICE,
                  0x901, 0xabc, &g_id_b, nullptr, true, 0};
   FakeObj ctx = {fake_context11_vtbl, NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT,
                  0x902, 0xabc, &g_id_b, &dev, true, 0};
   FakeObj res = {fake_obj_vtbl, NPT_NGX_BRIDGE_KIND_D3D11_RESOURCE,
                  0x903, 0xabc, &g_id_b, nullptr, true, 0};
   g_sessions[1].caps = -1;
   g_caps_sr = NGX_CAP_D3D12;
   int calls = g_init_calls;
   CHECK(NVSDK_NGX_D3D11_Init(1, nullptr, (ID3D11Device *)&dev, nullptr, 1) == NGX_RESULT_NOT_SUPPORTED);
   CHECK(g_init_calls == calls);
   void *caps = nullptr;
   unsigned int available = 9;
   CHECK(NVSDK_NGX_D3D11_GetParameters(&caps) == NGX_RESULT_OK);
   CHECK(((CallerParams *)caps)->vt->Get_uint(caps, "SuperSampling.Available", &available) == NGX_RESULT_OK && available == 0);
   CHECK(NVSDK_NGX_D3D11_DestroyParameters(caps) == NGX_RESULT_OK);
   g_sessions[1].caps = -1;
   g_caps_sr = NGX_CAP_D3D11 | NGX_CAP_EXTENDED_QUALITY;
   CHECK(NVSDK_NGX_D3D11_Init(1, nullptr, (ID3D11Device *)&dev, nullptr, 1) == NGX_RESULT_OK);
   CHECK(g_caps_calls == 2 && g_init_calls == calls + 1);
   CHECK(NVSDK_NGX_D3D11_GetCapabilityParameters(&caps) == NGX_RESULT_OK);
   CHECK(((CallerParams *)caps)->vt->Get_uint(caps, "SuperSampling.Available", &available) == NGX_RESULT_OK && available == 1);
   CHECK(NVSDK_NGX_D3D11_DestroyParameters(caps) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&g_device, nullptr, 1) == NGX_RESULT_OK);
   CallerParams *p = alloc_params();
   p->vt->Set_uint(p, "Width", 1280);
   p->vt->Set_uint(p, "Height", 720);
   p->vt->Set_uint(p, "OutWidth", 1280);
   p->vt->Set_uint(p, "OutHeight", 720);
   p->vt->Set_void(p, "DLSS.ScratchBuffer", nullptr);
   p->vt->Set_ull(p, "DLSS.ScratchSize", 0);
   unsigned int *h11 = nullptr;
   CHECK(NVSDK_NGX_D3D11_CreateFeature((ID3D11DeviceContext *)&g_list, 1, p, &h11) == NGX_RESULT_INVALID_PARAMETER);
   FakeObj wrong_ctx = ctx;
   wrong_ctx.device = &g_device;
   CHECK(NVSDK_NGX_D3D11_CreateFeature((ID3D11DeviceContext *)&wrong_ctx, 1, p, &h11) == NGX_RESULT_INVALID_PARAMETER);
   for (int q = 0; q <= 5; ++q) {
      p->vt->Set_int(p, "PerfQualityValue", q);
      CHECK(NVSDK_NGX_D3D11_CreateFeature((ID3D11DeviceContext *)&ctx, 1, p, &h11) == NGX_RESULT_OK);
      CHECK(g_seen_create.quality == q);
      if (q != 5)
         CHECK(NVSDK_NGX_D3D11_ReleaseFeature(h11) == NGX_RESULT_OK);
   }
   p->vt->Set_int(p, "PerfQualityValue", 2);
   unsigned int *h12 = nullptr;
   CHECK(NVSDK_NGX_D3D12_CreateFeature((ID3D12GraphicsCommandList *)&g_list, 1, p, &h12) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D11_ReleaseFeature(h12) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(h11) == NGX_RESULT_INVALID_PARAMETER);
   p->vt->Set_res11(p, "Color", (ID3D11Resource *)&res);
   p->vt->Set_res11(p, "Output", (ID3D11Resource *)&res);
   p->vt->Set_res11(p, "Depth", (ID3D11Resource *)&res);
   p->vt->Set_res11(p, "MotionVectors", (ID3D11Resource *)&res);
   CHECK(NVSDK_NGX_D3D11_EvaluateFeature((ID3D11DeviceContext *)&ctx, h12, p, nullptr) == NGX_RESULT_INVALID_PARAMETER);
   CHECK(NVSDK_NGX_D3D11_EvaluateFeature((ID3D11DeviceContext *)&ctx, h11, p, nullptr) == NGX_RESULT_OK);
   CHECK(g_seen_eval.color == res.id);
   CHECK(NVSDK_NGX_UpdateFeature(nullptr, 1) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D11_Shutdown() == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_ReleaseFeature(h12) == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
   CHECK(NVSDK_NGX_D3D11_DestroyParameters(p) == NGX_RESULT_OK);
}

#if !defined(_WIN32)
static void
test_d3d12_caps_qi_fallback()
{
   reset_counters();
   g_dxgi_adapter = &g_adapter;
   g_allow_d3d12_dxgi = true;
   g_sessions[0].caps = -1;
   g_sessions[0].extended = -1;
   g_caps_sr = NGX_CAP_D3D12 | NGX_CAP_EXTENDED_QUALITY;
   FakeObj dev = {fake_device11_vtbl, NPT_SR_KIND_DEVICE, 0xabc,
                  0xabc, &g_id_a, nullptr, true, 0};
   const int adapter_refs = g_adapter.refs;
   CHECK(NVSDK_NGX_D3D12_Init(1, nullptr, (ID3D12Device *)&dev,
                              nullptr, 1) == NGX_RESULT_OK);
   CHECK(g_caps_calls == 1);
   CHECK(g_sessions[0].caps == 1 && g_sessions[0].extended == 1);
   NGXFeatureDiscoveryInfo info = {};
   info.FeatureID = 1;
   NGXFeatureRequirement req = {};
   g_caps_sr = 0;
   CHECK(NVSDK_NGX_D3D12_GetFeatureRequirements(
            (IDXGIAdapter *)&g_adapter, &info, &req) == 1u);
   CHECK(req.FeatureSupported == 1 && g_sessions[0].extended == 1);
   g_caps_result = NPT_SR_RESULT_TRANSPORT_LOST;
   uint32_t result = NVSDK_NGX_D3D12_GetFeatureRequirements(
      (IDXGIAdapter *)&g_adapter, &info, &req);
   CHECK(result == 0xBAD00000u && (result & 0xFFF00000u) == 0xBAD00000u);
   CHECK(g_sessions[0].extended == 1);
   g_caps_result = NPT_SR_RESULT_SUCCESS;
   CHECK(g_adapter.refs == adapter_refs && dev.refs == 1);
   CHECK(NVSDK_NGX_D3D12_Shutdown() == NGX_RESULT_OK);
   CHECK(g_sessions[0].caps == -1 && g_sessions[0].extended == -1);
   g_allow_d3d12_dxgi = false;
}
#endif

int
main()
{
   test_param_slots();
   test_init_and_requirements();
   test_optimal_settings_callback();
   test_create_eval_release();
   test_reused_eval_subrect();
   test_release_and_shutdown_states();
   test_release_generation_race();
   test_scratch_and_shutdown1();
   test_d3d11_session();
#if !defined(_WIN32)
   test_d3d12_caps_qi_fallback();
#endif
   printf("ngx_guest_api_test: PASS\n");
   return 0;
}
