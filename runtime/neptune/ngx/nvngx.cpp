/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * nvngx.dll: guest-side D3D11 and D3D12 NGX entry points backed by the
 * private per-object NGX bridge (npt_ngx_bridge_v1.h).
 *
 * Limits: one live device session per API, feature 1 only,
 * no subrect bases, EVAL render dimensions within 1..in_w x 1..in_h,
 * create flags restricted to the low 8 bits (0xff), no scratch, no non-empty
 * progress callbacks, node masks must be default.
 */

#include "../npt_ngx_bridge_v1.h"

#include "ngx_parameter.hpp"

#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#endif

/* ------------------------------------------------------------------ */
/* Gated diagnostics.  Enabled when NPT_DEBUG contains "ngx" or        */
/* BRINEWELL_NGX_DEBUG is set; lines go to stderr and (under Windows)  */
/* OutputDebugStringA so they surface in Proton/debugstr channels.     */
/* A budget keeps per-frame paths from flooding if an error repeats.   */
/* ------------------------------------------------------------------ */

static std::atomic<int> g_ngx_log_budget{4000};

static bool
ngx_debug_enabled()
{
   static const bool enabled = []() {
      const char *dbg = getenv("NPT_DEBUG");
      const char *alt = getenv("BRINEWELL_NGX_DEBUG");
      return (dbg && strstr(dbg, "ngx")) || (alt && alt[0]);
   }();
   return enabled;
}

static bool
ngx_log_take_slot()
{
   if (!ngx_debug_enabled())
      return false;
   int cur = g_ngx_log_budget.load(std::memory_order_relaxed);
   while (cur > 0) {
      if (g_ngx_log_budget.compare_exchange_weak(
             cur, cur - 1, std::memory_order_relaxed))
         return true;
   }
   return false;
}

static void
ngx_log_emit(const char *buf)
{
#if defined(_WIN32)
   OutputDebugStringA("nvngx: ");
   OutputDebugStringA(buf);
   OutputDebugStringA("\n");
#endif
   fprintf(stderr, "nvngx: %s\n", buf);
   fflush(stderr);
}

static void
ngx_logf(const char *fmt, ...)
{
   if (!ngx_log_take_slot())
      return;
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   ngx_log_emit(buf);
}

static void
ngx_dump_params(NgxParameters *p, const char *what)
{
   if (!ngx_debug_enabled() || !p || !ngx_log_take_slot())
      return;

   static const char *const scalar_keys[] = {
      "Width", "Height", "OutWidth", "OutHeight", "PerfQualityValue",
      "DLSS.Feature.Create.Flags", "DLSS.Render.Subrect.Dimensions.Width",
      "DLSS.Render.Subrect.Dimensions.Height", "DLSS.Render.Subrect.Base.X",
      "DLSS.Render.Subrect.Base.Y", "DLSS.Render.Subrect.Origin.X",
      "DLSS.Render.Subrect.Origin.Y", "DLSS.Input.Color.Subrect.Base.X",
      "DLSS.Input.Color.Subrect.Base.Y", "DLSS.Input.Depth.Subrect.Base.X",
      "DLSS.Input.Depth.Subrect.Base.Y", "DLSS.Input.MV.Subrect.Base.X",
      "DLSS.Input.MV.Subrect.Base.Y", "DLSS.Input.Exposure.Subrect.Base.X",
      "DLSS.Input.Exposure.Subrect.Base.Y", "DLSS.Output.Subrect.Base.X",
      "DLSS.Output.Subrect.Base.Y", "Jitter.Offset.X", "Jitter.Offset.Y",
      "MV.Scale.X", "MV.Scale.Y", "DLSS.Pre.Exposure", "Reset",
      "DLSS.Enable.Output.Subrects", "CreationNodeMask", "VisibilityNodeMask",
   };
   static const char *const resource_keys[] = {
      "Color", "Output", "Depth", "MotionVectors", "ExposureTexture",
   };
   struct Scalar {
      bool present;
      NgxParamValue::Kind kind;
      uint64_t i;
      double d;
   } scalars[sizeof(scalar_keys) / sizeof(scalar_keys[0])] = {};
   bool resources[sizeof(resource_keys) / sizeof(resource_keys[0])] = {};
   {
      std::lock_guard<std::mutex> lock(p->mtx);
      for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
         NgxParamValue v;
         if (ngx_param_find(p, scalar_keys[i], &v) &&
             v.kind != NgxParamValue::KIND_PTR) {
            scalars[i] = {true, v.kind, v.i, v.d};
         }
      }
      for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
         NgxParamValue v;
         void *value = nullptr;
         resources[i] = ngx_param_find(p, resource_keys[i], &v) &&
                        ngx_param_convert(v, &value) && value;
      }
   }

   std::string line = std::string(what) + " params:";
   for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
      if (!scalars[i].present)
         continue;
      char tmp[192];
      if (scalars[i].kind == NgxParamValue::KIND_INT)
         snprintf(tmp, sizeof(tmp), " %s=i%lld", scalar_keys[i],
                  (long long)scalars[i].i);
      else
         snprintf(tmp, sizeof(tmp), " %s=f%g", scalar_keys[i], scalars[i].d);
      line += tmp;
   }
   for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
      line += " ";
      line += resource_keys[i];
      line += resources[i] ? "_present=1" : "_present=0";
   }
   ngx_log_emit(line.c_str());
}

/* ------------------------------------------------------------------ */
/* Minimal COM plumbing: objects are accessed through raw vtable slots  */
/* so this file needs no Windows SDK headers outside _WIN32 DllMain.    */
/* ------------------------------------------------------------------ */

typedef int32_t (NPT_SR_METHODCALLTYPE *ngx_com_qi_fn)(void *self,
                                                     const NptSrGuid *iid,
                                                     void **out);
typedef uint32_t (NPT_SR_METHODCALLTYPE *ngx_com_addref_fn)(void *self);
typedef uint32_t (NPT_SR_METHODCALLTYPE *ngx_com_release_fn)(void *self);
typedef int32_t (NPT_SR_METHODCALLTYPE *ngx_get_device_fn)(void *self,
                                                         const NptSrGuid *iid,
                                                         void **out);

/* {189819f1-1db6-4b57-be54-1821339b85f7} IID_ID3D12Device */
static const NptSrGuid NGX_IID_ID3D12DEVICE = {
   {0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
    0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

static void *const *
ngx_vtbl(void *obj)
{
   return *static_cast<void *const **>(obj);
}

static int32_t
ngx_com_qi(void *obj, const NptSrGuid *iid, void **out)
{
   return ((ngx_com_qi_fn)ngx_vtbl(obj)[0])(obj, iid, out);
}

static void
ngx_com_release(void *obj)
{
   ((ngx_com_release_fn)ngx_vtbl(obj)[2])(obj);
}

/* ID3D12Object::GetDevice, slot 7 on every D3D12 device child. */
static int32_t
ngx_com_get_device(void *obj, const NptSrGuid *iid, void **out)
{
   return ((ngx_get_device_fn)ngx_vtbl(obj)[7])(obj, iid, out);
}

/* Canonical IUnknown identity of an object (QI IUnknown, released by
 * the caller). */
static void *
ngx_identity(void *obj)
{
   void *p = nullptr;
   if (ngx_com_qi(obj, &NPT_SR_IID_IUNKNOWN, &p) != NPT_SR_S_OK)
      return nullptr;
   return p;
}

/* ------------------------------------------------------------------ */
/* Caller-visible parameter vtable, frozen slot order:                  */
/*   Set(void*, ID3D12Resource*, ID3D11Resource*, int, uint, double,    */
/*       float, ull)                                                   */
/*   Get(void**, ID3D12Resource**, double*, uint*, int*,               */
/*       ID3D11Resource**, float*, ull*)                               */
/*   Reset                                                             */
/* ------------------------------------------------------------------ */

#define NGX_SET_FN(name, type)                                              \
   static void NPT_SR_METHODCALLTYPE                                        \
   name(void *self, const char *key, type value)                            \
   {                                                                        \
      ngx_param_set<type>(static_cast<NgxParameters *>(self), key, value);  \
   }

#define NGX_GET_FN(name, type)                                              \
   static uint32_t NPT_SR_METHODCALLTYPE                                    \
   name(void *self, const char *key, type *out)                             \
   {                                                                        \
      return ngx_param_get<type>(static_cast<NgxParameters *>(self),        \
                               key, out);                                   \
   }

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIAdapter;

NGX_SET_FN(ngx_set_void, void *)
static void NPT_SR_METHODCALLTYPE
ngx_set_res12(void *self, const char *key, ID3D12Resource *value)
{
   ngx_param_set<void *>(static_cast<NgxParameters *>(self), key,
                         (void *)value);
}
static void NPT_SR_METHODCALLTYPE
ngx_set_res11(void *self, const char *key, ID3D11Resource *value)
{
   ngx_param_set<void *>(static_cast<NgxParameters *>(self), key,
                         (void *)value);
}
NGX_SET_FN(ngx_set_int, int)
NGX_SET_FN(ngx_set_uint, unsigned int)
NGX_SET_FN(ngx_set_double, double)
NGX_SET_FN(ngx_set_float, float)
NGX_SET_FN(ngx_set_ull, unsigned long long)

NGX_GET_FN(ngx_get_void, void *)
NGX_GET_FN(ngx_get_res12, ID3D12Resource *)
NGX_GET_FN(ngx_get_double, double)
NGX_GET_FN(ngx_get_uint, unsigned int)
NGX_GET_FN(ngx_get_int, int)
NGX_GET_FN(ngx_get_res11, ID3D11Resource *)
NGX_GET_FN(ngx_get_float, float)
NGX_GET_FN(ngx_get_ull, unsigned long long)

static void NPT_SR_METHODCALLTYPE
ngx_param_reset(void *self)
{
   NgxParameters *p = static_cast<NgxParameters *>(self);
   std::lock_guard<std::mutex> lock(p->mtx);
   p->values.clear();
}

static const void *const ngx_param_vtbl[] = {
   (const void *)ngx_set_void,
   (const void *)ngx_set_res12,
   (const void *)ngx_set_res11,
   (const void *)ngx_set_int,
   (const void *)ngx_set_uint,
   (const void *)ngx_set_double,
   (const void *)ngx_set_float,
   (const void *)ngx_set_ull,
   (const void *)ngx_get_void,
   (const void *)ngx_get_res12,
   (const void *)ngx_get_double,
   (const void *)ngx_get_uint,
   (const void *)ngx_get_int,
   (const void *)ngx_get_res11,
   (const void *)ngx_get_float,
   (const void *)ngx_get_ull,
   (const void *)ngx_param_reset,
};

/* ------------------------------------------------------------------ */
/* Session and feature registry                                        */
/* ------------------------------------------------------------------ */

enum NgxFeatureState {
   NGX_FEATURE_FREE = 0,
   NGX_FEATURE_LIVE = 1,
   NGX_FEATURE_RELEASING = 2,
};

enum NgxSessionState {
   NGX_SESSION_NONE = 0,
   NGX_SESSION_LIVE = 1,
   /* Shutdown is in flight: Init/Create/Eval are blocked; Release is
    * still allowed to finish. */
   NGX_SESSION_SHUTTING_DOWN = 2,
};

/* Bounded live-feature table.  The caller-visible handle word encodes a
 * monotonically increasing generation plus the slot index, so a stale
 * handle fails the generation check even after its slot is reused --
 * no unbounded tombstone list and no per-eval history scan. */
#define NGX_MAX_FEATURES 64u

struct NgxFeature {
   unsigned int handle; /* caller-visible handle word; offset 0 */
   uint64_t token;
   uint32_t in_w, in_h;
   NgxFeatureState state;
   int api;
   uint64_t session_gen;
   uint64_t feature_gen;
};

static std::mutex g_mtx;
struct NgxSession {
   NptNgxBridgeV1 *bridge = nullptr;
   void *identity = nullptr;
   uint64_t id = 0;
   uintptr_t cookie = 0;
   NgxSessionState state = NGX_SESSION_NONE;
   int caps = -1;
   int extended = -1;
   uint64_t session_gen = 0;
};
static NgxSession g_sessions[2];
static int g_discovery_caps[2] = {-1, -1};
static constexpr uint32_t NGX_CAP_D3D12 = 1u << 0;
static constexpr uint32_t NGX_CAP_D3D11 = 1u << 1;
static constexpr uint32_t NGX_CAP_EXTENDED_QUALITY = 1u << 2;
static constexpr uint32_t NGX_QUALITY_MAX = 5;

static NgxFeature g_features[NGX_MAX_FEATURES];
static uint64_t g_feature_gen;

static void
ngx_feature_free_locked(NgxFeature *f)
{
   f->handle = 0;
   f->token = 0;
   f->state = NGX_FEATURE_FREE;
}

static NgxFeature *
ngx_feature_alloc_locked()
{
   for (uint32_t i = 0; i < NGX_MAX_FEATURES; i++) {
      NgxFeature *f = &g_features[i];
      if (f->state != NGX_FEATURE_FREE)
         continue;
      ++g_feature_gen;
      if (!(g_feature_gen & 0x3ffffffu))
         ++g_feature_gen;
      f->feature_gen = g_feature_gen;
      f->handle = ((uint32_t)(g_feature_gen & 0x3ffffffu) << 6) | i;
      f->state = NGX_FEATURE_LIVE;
      return f;
   }
   return nullptr;
}

/* Resolve a caller-supplied handle pointer to a live/releasing slot.
 * The pointer must address a slot's handle word exactly and the stored
 * generation word must still match what the caller holds. */
static NgxFeature *
ngx_feature_find_locked(const unsigned int *handle)
{
   if (!handle)
      return nullptr;
   const uintptr_t base = (uintptr_t)&g_features[0];
   const uintptr_t end = (uintptr_t)&g_features[NGX_MAX_FEATURES];
   if ((uintptr_t)handle < base || (uintptr_t)handle >= end ||
       ((uintptr_t)handle - base) % sizeof(NgxFeature))
      return nullptr;
   NgxFeature *f = (NgxFeature *)handle; /* handle is at offset 0 */
   if (f->state == NGX_FEATURE_FREE || !f->handle || f->handle != *handle)
      return nullptr;
   return f;
}

static void
ngx_session_clear_locked(int api)
{
   for (NgxFeature &f : g_features) {
      if (f.state != NGX_FEATURE_FREE && f.api == api)
         ngx_feature_free_locked(&f);
   }
   if (g_sessions[api].identity)
      ngx_com_release(g_sessions[api].identity);
   if (g_sessions[api].bridge)
      g_sessions[api].bridge->lpVtbl->Release(g_sessions[api].bridge);
   g_sessions[api].identity = nullptr;
   g_sessions[api].bridge = nullptr;
   g_sessions[api].id = 0;
   g_sessions[api].cookie = 0;
   g_sessions[api].caps = -1;
   g_sessions[api].extended = -1;
   g_sessions[api].state = NGX_SESSION_NONE;
}

/* QI `obj` for the bridge tear-off; on success checks the expected kind
 * and that the object shares the session transport context.  Caller
 * releases the returned bridge. */
static NptNgxBridgeV1 *
ngx_get_bridge(int api, void *obj, uint32_t want_kind, uint64_t *out_id,
               const char *what = "object")
{
   void *p = nullptr;
   if (!obj ||
       ngx_com_qi(obj, &NPT_SR_IID_NGX_BRIDGE_V1, &p) != NPT_SR_S_OK ||
       !p) {
      ngx_logf("%s: bridge QI failed (obj_present=%d bridge_present=%d)",
                what, obj != nullptr, p != nullptr);
      return nullptr;
   }
   NptNgxBridgeV1 *b = static_cast<NptNgxBridgeV1 *>(p);
   uint32_t kind = 0;
   uintptr_t cookie = 0;
   uint64_t id = 0;
   if (b->lpVtbl->GetRef(b, &kind, &cookie, &id) != NPT_SR_S_OK ||
       kind != want_kind || cookie != g_sessions[api].cookie) {
      ngx_logf("%s: bridge reject kind=%u want=%u context_match=%d",
               what, kind, want_kind, cookie == g_sessions[api].cookie);
      b->lpVtbl->Release(b);
      return nullptr;
   }
   if (out_id)
      *out_id = id;
   return b;
}

/* List must belong to the session's D3D12 device. */
static bool
ngx_same_device(int api, void *child)
{
   void *dev = nullptr;
   if (api) {
      typedef void (NPT_SR_METHODCALLTYPE *get_device11_fn)(void *, void **);
      ((get_device11_fn)ngx_vtbl(child)[3])(child, &dev);
      if (!dev)
         return false;
   } else if (ngx_com_get_device(child, &NGX_IID_ID3D12DEVICE, &dev) !=
                 NPT_SR_S_OK || !dev) {
      return false;
   }
   void *id = ngx_identity(dev);
   ngx_com_release(dev);
   bool same = id && id == g_sessions[api].identity;
   if (id)
      ngx_com_release(id);
   return same;
}

static uint32_t
ngx_public_result(uint32_t r)
{
   return r == NGX_RESULT_OK || r == NGX_RESULT_FAIL ||
          r == NGX_RESULT_NOT_SUPPORTED || r == NGX_RESULT_INVALID_PARAMETER
             ? r : NGX_RESULT_FAIL;
}

static uint32_t
ngx_query_adapter_caps(int api, void *adapter, int *caps, int *extended)
{
   void *p = nullptr;
   uint32_t sr = 0, r = NGX_RESULT_NOT_SUPPORTED;
   if (ngx_com_qi(adapter, &NPT_SR_IID_NGX_BRIDGE_V1, &p) == NPT_SR_S_OK && p) {
      NptNgxBridgeV1 *b = static_cast<NptNgxBridgeV1 *>(p);
      uint32_t kind = 0;
      uintptr_t cookie = 0;
      uint64_t id = 0;
      if (b->lpVtbl->GetRef(b, &kind, &cookie, &id) == NPT_SR_S_OK &&
          kind == NPT_SR_KIND_ADAPTER)
         r = b->lpVtbl->Caps(b, &sr);
      b->lpVtbl->Release(b);
   }
   if (r == NGX_RESULT_OK) {
      *caps = !!(sr & (api ? NGX_CAP_D3D11 : NGX_CAP_D3D12));
      *extended = !!(sr & NGX_CAP_EXTENDED_QUALITY);
   }
   return r;
}

#if defined(_WIN32)
static uint32_t
ngx_query_d3d12_caps(void *device, int *caps, int *extended)
{
   const LUID luid = static_cast<ID3D12Device *>(device)->GetAdapterLuid();
   HMODULE dxgi = LoadLibraryA("dxgi.dll");
   if (!dxgi)
      return NGX_RESULT_NOT_SUPPORTED;
   typedef HRESULT (WINAPI *create_factory_fn)(REFIID, void **);
   create_factory_fn create_factory =
      reinterpret_cast<create_factory_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
   static const GUID iid_factory1 = {
      0x770aae78, 0xf26f, 0x4dba,
      {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
   IDXGIFactory1 *factory = nullptr;
   uint32_t result = NGX_RESULT_NOT_SUPPORTED;
   if (create_factory &&
       SUCCEEDED(create_factory(iid_factory1, reinterpret_cast<void **>(&factory))) &&
       factory) {
      for (UINT index = 0;; ++index) {
         IDXGIAdapter1 *adapter = nullptr;
         HRESULT hr = factory->EnumAdapters1(index, &adapter);
         if (FAILED(hr) || !adapter) {
            if (adapter)
               adapter->Release();
            break;
         }
         DXGI_ADAPTER_DESC1 desc = {};
         const bool match = SUCCEEDED(adapter->GetDesc1(&desc)) &&
                            desc.AdapterLuid.LowPart == luid.LowPart &&
                            desc.AdapterLuid.HighPart == luid.HighPart;
         if (match)
            result = ngx_query_adapter_caps(0, adapter, caps, extended);
         adapter->Release();
         if (match)
            break;
      }
   }
   if (factory)
      factory->Release();
   FreeLibrary(dxgi);
   return result;
}
#endif

static uint32_t
ngx_query_device_caps(int api, void *device, int *caps, int *extended)
{
#if defined(_WIN32)
   if (!api) {
      uint32_t result = ngx_query_d3d12_caps(device, caps, extended);
      if (result == NGX_RESULT_OK)
         return result;
   }
#endif
   static const NptSrGuid iid_dxgi_device = {
      {0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
       0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
   void *dxgi = nullptr;
   if (ngx_com_qi(device, &iid_dxgi_device, &dxgi) != NPT_SR_S_OK || !dxgi) {
      if (dxgi)
         ngx_com_release(dxgi);
      return NGX_RESULT_NOT_SUPPORTED;
   }
   typedef int32_t (NPT_SR_METHODCALLTYPE *get_adapter_fn)(void *, void **);
   void *adapter = nullptr;
   int32_t hr = ((get_adapter_fn)ngx_vtbl(dxgi)[7])(dxgi, &adapter);
   ngx_com_release(dxgi);
   if (hr != NPT_SR_S_OK || !adapter) {
      if (adapter)
         ngx_com_release(adapter);
      return NGX_RESULT_NOT_SUPPORTED;
   }
   const uint32_t result = ngx_query_adapter_caps(api, adapter, caps, extended);
   ngx_com_release(adapter);
   return result;
}

static uint32_t
ngx_init(int api, void *device, uint64_t application_id, uint32_t sdk_version)
{
   if (!device)
      return NGX_RESULT_INVALID_PARAMETER;
   NptNgxBridgeV1 *b = nullptr;
   {
      void *p = nullptr;
      if (ngx_com_qi(device, &NPT_SR_IID_NGX_BRIDGE_V1, &p) !=
              NPT_SR_S_OK ||
          !p)
         return NGX_RESULT_NOT_SUPPORTED;
      b = static_cast<NptNgxBridgeV1 *>(p);
   }
   uint32_t kind = 0;
   uintptr_t cookie = 0;
   uint64_t dev_id = 0;
   if (b->lpVtbl->GetRef(b, &kind, &cookie, &dev_id) != NPT_SR_S_OK ||
       kind != (api ? NPT_NGX_BRIDGE_KIND_D3D11_DEVICE : NPT_SR_KIND_DEVICE)) {
      b->lpVtbl->Release(b);
      return NGX_RESULT_NOT_SUPPORTED;
   }
   void *identity = ngx_identity(device);
   if (!identity) {
      b->lpVtbl->Release(b);
      return NGX_RESULT_FAIL;
   }
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      if (g_sessions[api].state != NGX_SESSION_NONE) {
         bool same = g_sessions[api].state == NGX_SESSION_LIVE &&
                     identity == g_sessions[api].identity;
         ngx_com_release(identity);
         b->lpVtbl->Release(b);
         return same ? NGX_RESULT_OK : NGX_RESULT_FAIL;
      }
   }
   int caps = -1, extended = -1;
   uint32_t caps_result = ngx_query_device_caps(api, device, &caps, &extended);
   if (api && (caps_result != NGX_RESULT_OK || caps != 1)) {
      ngx_com_release(identity);
      b->lpVtbl->Release(b);
      return NGX_RESULT_NOT_SUPPORTED;
   }

   std::lock_guard<std::mutex> lock(g_mtx);
   if (g_sessions[api].state == NGX_SESSION_SHUTTING_DOWN) {
      ngx_com_release(identity);
      b->lpVtbl->Release(b);
      return NGX_RESULT_FAIL;
   }
   if (g_sessions[api].state == NGX_SESSION_LIVE) {
      /* Same device re-Init is idempotent; a different device is not. */
      bool same = identity == g_sessions[api].identity;
      ngx_com_release(identity);
      b->lpVtbl->Release(b);
      return same ? NGX_RESULT_OK : NGX_RESULT_FAIL;
   }

   uint32_t r = b->lpVtbl->Init(b, application_id, sdk_version);
   ngx_logf("Init device_id=%llu app=0x%llx sdk=0x%x -> 0x%x",
            (unsigned long long)dev_id, (unsigned long long)application_id,
            sdk_version, r);
   if (r != NGX_RESULT_OK) {
      ngx_com_release(identity);
      b->lpVtbl->Release(b);
      return ngx_public_result(r);
   }
   ++g_sessions[api].session_gen;
   g_sessions[api].caps = caps;
   g_sessions[api].extended = extended;
   g_sessions[api].bridge = b;
   g_sessions[api].identity = identity;
   g_sessions[api].id = dev_id;
   g_sessions[api].cookie = cookie;
   g_sessions[api].state = NGX_SESSION_LIVE;
   return NGX_RESULT_OK;
}

/* ------------------------------------------------------------------ */
/* Capability parameters and callbacks                                 */
/* ------------------------------------------------------------------ */

static uint32_t NPT_SR_METHODCALLTYPE
ngx_dlss_optimal_settings_api(int api, void *params_v)
{
   NgxParameters *params = static_cast<NgxParameters *>(params_v);
   uint32_t width = 0, height = 0, quality = 0;
   {
      std::lock_guard<std::mutex> lock(params->mtx);
      if (ngx_param_try_get_exact_uint32(params, "Width", &width) != NGX_UINT_OK ||
          ngx_param_try_get_exact_uint32(params, "Height", &height) != NGX_UINT_OK ||
          ngx_param_try_get_exact_uint32(params, "PerfQualityValue", &quality) !=
             NGX_UINT_OK)
         return NGX_RESULT_FAIL;
   }

   {
      std::lock_guard<std::mutex> lock(g_mtx);
      if (quality > 2 && g_sessions[api].extended != 1)
         return NGX_RESULT_NOT_SUPPORTED;
   }
   double scale;
   switch (quality) {
   case 0: scale = 0.5; break;                /* MAXPERF -> Performance  */
   case 1: scale = 0.58; break;               /* BALANCED -> Balanced    */
   case 2: scale = 2.0 / 3.0; break;          /* MAXQUALITY -> Quality   */
   case 3: scale = 1.0 / 3.0; break;
   case 4: scale = 1.0 / 1.3; break;
   case 5: scale = 1.0; break;
   default: return NGX_RESULT_NOT_SUPPORTED;
   }

   const unsigned int render_w =
      (unsigned int)std::ceil(width * scale);
   const unsigned int render_h =
      (unsigned int)std::ceil(height * scale);

   ngx_param_set<float>(params, "Scale", (float)scale);
   ngx_param_set<float>(params, "SuperSampling.ScaleFactor",
                        (float)scale);
   ngx_param_set<unsigned int>(params, "OutWidth", render_w);
   ngx_param_set<unsigned int>(params, "OutHeight", render_h);
   ngx_param_set<float>(params, "Sharpness", 0.0f);
   const unsigned int half_w = width > 1 ? width / 2 + width % 2 : 1;
   const unsigned int half_h = height > 1 ? height / 2 + height % 2 : 1;
   const unsigned int min_w = quality == 5 ? width :
      (render_w < half_w ? render_w : half_w);
   const unsigned int min_h = quality == 5 ? height :
      (render_h < half_h ? render_h : half_h);
   ngx_param_set<unsigned int>(params, "DLSS.Get.Dynamic.Min.Render.Width", min_w);
   ngx_param_set<unsigned int>(params, "DLSS.Get.Dynamic.Min.Render.Height", min_h);
   ngx_param_set<unsigned int>(params, "DLSS.Get.Dynamic.Max.Render.Width", width);
   ngx_param_set<unsigned int>(params, "DLSS.Get.Dynamic.Max.Render.Height", height);
   ngx_param_set<int>(params, "DLSSMode", 1);
   ngx_param_set<unsigned int>(params, "DLSS.Hint.Render.Preset.DLAA", 0);
   ngx_param_set<unsigned int>(params,
                               "DLSS.Hint.Render.Preset.UltraQuality", 0);
   ngx_param_set<unsigned int>(params, "DLSS.Hint.Render.Preset.Quality",
                               0);
   ngx_param_set<unsigned int>(params, "DLSS.Hint.Render.Preset.Balanced",
                               0);
   ngx_param_set<unsigned int>(params,
                               "DLSS.Hint.Render.Preset.Performance", 0);
   ngx_param_set<unsigned int>(
      params, "DLSS.Hint.Render.Preset.UltraPerformance", 0);
   return NGX_RESULT_OK;
}

static uint32_t NPT_SR_METHODCALLTYPE
ngx_dlss_optimal_settings(void *params) { return ngx_dlss_optimal_settings_api(0, params); }
static uint32_t NPT_SR_METHODCALLTYPE
ngx_dlss_optimal_settings11(void *params) { return ngx_dlss_optimal_settings_api(1, params); }

static uint32_t NPT_SR_METHODCALLTYPE
ngx_dlss_get_stats(void *params_v)
{
   NgxParameters *params = static_cast<NgxParameters *>(params_v);
   ngx_param_set<unsigned long long>(params, "SizeInBytes", 0);
   return NGX_RESULT_OK;
}

static NgxParameters *
ngx_new_params()
{
   NgxParameters *p = new (std::nothrow) NgxParameters();
   if (p)
      p->vt = ngx_param_vtbl;
   return p;
}

static uint32_t
ngx_capability_params(int api, void **out_params)
{
   if (!out_params)
      return NGX_RESULT_INVALID_PARAMETER;
   *out_params = nullptr;
   NgxParameters *p = ngx_new_params();
   if (!p)
      return NGX_RESULT_FAIL;

   int avail;
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      /* Init only succeeds when the host confirmed SR capability, so a
       * live session is itself the positive answer. */
      avail = g_sessions[api].state == NGX_SESSION_LIVE
                 ? 1
                 : (g_discovery_caps[api] > 0 ? 1 : 0);
   }
   ngx_param_set<unsigned int>(p, "SuperSampling.Available", avail);
   ngx_param_set<unsigned int>(p, "#\x01", avail);
   ngx_param_set<unsigned int>(p, "SuperSampling.MinDriverVersionMajor",
                             0);
   ngx_param_set<unsigned int>(p, "SuperSampling.MinDriverVersionMinor",
                             0);
   ngx_param_set<unsigned int>(p, "SuperSampling.NeedsUpdatedDriver", 0);
   ngx_param_set<unsigned int>(p, "SuperSampling.FeatureInitResult", 1);
   ngx_param_set<unsigned int>(p, "SuperSamplingDenoising.Available", 0);
   ngx_param_set<unsigned int>(p,
                               "SuperSamplingDenoising.NeedsUpdatedDriver",
                               0);
   ngx_param_set<unsigned int>(
      p, "SuperSamplingDenoising.MinDriverVersionMajor", 0);
   ngx_param_set<unsigned int>(
      p, "SuperSamplingDenoising.MinDriverVersionMinor", 0);
   ngx_param_set<unsigned int>(
      p, "SuperSamplingDenoising.FeatureInitResult", 1);
   ngx_param_set<unsigned int>(p, "Snippet.OptLevel", 0);
   ngx_param_set<unsigned int>(p, "Snippet.IsDevBranch", 0);
   ngx_param_set<void *>(p, "DLSSOptimalSettingsCallback",
                         (void *)(api ? &ngx_dlss_optimal_settings11 : &ngx_dlss_optimal_settings));
   ngx_param_set<void *>(p, "DLSSGetStatsCallback",
                         (void *)&ngx_dlss_get_stats);
   ngx_param_set<void *>(p, "DLSSDOptimalSettingsCallback",
                         (void *)(api ? &ngx_dlss_optimal_settings11 : &ngx_dlss_optimal_settings));
   ngx_param_set<void *>(p, "DLSSDGetStatsCallback",
                         (void *)&ngx_dlss_get_stats);
   ngx_param_set<float>(p, "Sharpness", 0.0f);
   ngx_param_set<float>(p, "MV.Scale.X", 1.0f);
   ngx_param_set<float>(p, "MV.Scale.Y", 1.0f);
   ngx_param_set<float>(p, "MV.Offset.X", 0.0f);
   ngx_param_set<float>(p, "MV.Offset.Y", 0.0f);
   ngx_param_set<float>(p, "DLSS.Exposure.Scale", 1.0f);
   ngx_param_set<int>(p, "PerfQualityValue", 2);
   ngx_param_set<int>(p, "CreationNodeMask", 1);
   ngx_param_set<int>(p, "VisibilityNodeMask", 1);
   ngx_param_set<int>(p, "DLSS.Enable.Output.Subrects", 0);
   ngx_param_set<int>(p, "RTXValue", 0);

   *out_params = p;
   return NGX_RESULT_OK;
}

/* ------------------------------------------------------------------ */
/* Create / evaluate parameter validation                              */
/* ------------------------------------------------------------------ */

/* NGX DLSS create flags pass through to the backend verbatim: the
 * framework is authoritative for their meaning, so every documented
 * flag bit (0xff) is accepted rather than a subset. */
#define NGX_CREATE_FLAG_MASK 0xffu

struct NgxCreateScalars {
   uint32_t in_w, in_h, out_w, out_h;
   int32_t quality;
   uint32_t flags;
};

static NgxExactUint32Result
ngx_get_exact_u(NgxParameters *p, const char *key, uint32_t *out)
{
   std::lock_guard<std::mutex> lock(p->mtx);
   return ngx_param_try_get_exact_uint32(p, key, out);
}

static uint32_t
ngx_read_create(NgxParameters *params, NgxCreateScalars *out)
{
#define NGX_CREATE_REJECT(r, why) \
   do { \
      ngx_logf("create reject: %s -> 0x%x", why, (uint32_t)(r)); \
      return (r); \
   } while (0)

   if (ngx_get_exact_u(params, "Width", &out->in_w) != NGX_UINT_OK ||
       ngx_get_exact_u(params, "Height", &out->in_h) != NGX_UINT_OK ||
       ngx_get_exact_u(params, "OutWidth", &out->out_w) != NGX_UINT_OK ||
       ngx_get_exact_u(params, "OutHeight", &out->out_h) != NGX_UINT_OK)
      NGX_CREATE_REJECT(NGX_RESULT_INVALID_PARAMETER,
                        "missing Width/Height/OutWidth/OutHeight");

   uint32_t quality = 2, flags = 0;
   if (ngx_get_exact_u(params, "PerfQualityValue", &quality) == NGX_UINT_INVALID ||
       ngx_get_exact_u(params, "DLSS.Feature.Create.Flags", &flags) == NGX_UINT_INVALID)
      NGX_CREATE_REJECT(NGX_RESULT_INVALID_PARAMETER, "invalid quality/flags");
   out->flags = flags;
   ngx_logf("create params: %ux%u -> %ux%u quality=%u flags=0x%x",
            out->in_w, out->in_h, out->out_w, out->out_h, quality,
            flags);

   if (quality > NGX_QUALITY_MAX)
      NGX_CREATE_REJECT(NGX_RESULT_NOT_SUPPORTED, "quality out of range");
   out->quality = (int32_t)quality;
   if (flags & ~NGX_CREATE_FLAG_MASK)
      NGX_CREATE_REJECT(NGX_RESULT_NOT_SUPPORTED,
                        "unsupported create flags");
   if (!out->in_w || !out->in_h || out->in_w > 16384 ||
       out->in_h > 16384 || !out->out_w || !out->out_h ||
       out->out_w > 16384 || out->out_h > 16384 ||
       out->in_w > out->out_w || out->in_h > out->out_h)
      NGX_CREATE_REJECT(NGX_RESULT_INVALID_PARAMETER, "bad dimensions");

   uint32_t subrects = 0;
   if (ngx_get_exact_u(params, "DLSS.Enable.Output.Subrects", &subrects) == NGX_UINT_INVALID)
      NGX_CREATE_REJECT(NGX_RESULT_INVALID_PARAMETER, "invalid output subrects");
   if (subrects != 0)
      NGX_CREATE_REJECT(NGX_RESULT_NOT_SUPPORTED,
                        "output subrects enabled");
   static const char *const node_keys[] = {
      "CreationNodeMask", "VisibilityNodeMask",
   };
   for (const char *key : node_keys) {
      uint32_t mask = 1;
      if (ngx_get_exact_u(params, key, &mask) == NGX_UINT_INVALID || mask != 1)
         NGX_CREATE_REJECT(NGX_RESULT_NOT_SUPPORTED,
                           "non-default node mask");
   }
   /* Explicit scratch is unsupported: our GetScratchBufferSize is 0 and
    * the host rejects nonzero scratch on CREATE. */
   {
      std::lock_guard<std::mutex> lock(params->mtx);
      for (const auto &kv : params->values) {
         if (kv.first.find("Scratch") != std::string::npos &&
             ((kv.second.kind == NgxParamValue::KIND_PTR && kv.second.p) ||
              (kv.second.kind == NgxParamValue::KIND_INT && kv.second.i) ||
              (kv.second.kind == NgxParamValue::KIND_NUM && kv.second.d != 0.0)))
            NGX_CREATE_REJECT(NGX_RESULT_INVALID_PARAMETER,
                              "explicit scratch");
      }
   }
   return NGX_RESULT_OK;
#undef NGX_CREATE_REJECT
}

/* ------------------------------------------------------------------ */
/* Exports                                                             */
/* ------------------------------------------------------------------ */

#define NVNGX_API extern "C"

NVNGX_API uint32_t
NVSDK_NGX_UpdateFeature(const void *app_id, unsigned int feature)
{
   (void)app_id;
   (void)feature;
   return NGX_RESULT_OK;
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_Init(unsigned long long id, const wchar_t *path,
                     ID3D12Device *device, const void *feature_info,
                     unsigned int sdk_version)
{
   (void)path; (void)feature_info;
   return ngx_init(0, device, id, sdk_version);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_Init_Ext(unsigned long long id, const wchar_t *path,
                         ID3D12Device *device, unsigned int sdk_version,
                         const void *parameters)
{
   (void)path; (void)parameters;
   return ngx_init(0, device, id, sdk_version);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_Init_ProjectID(const char *project,
                               unsigned int engine_type,
                               const char *engine_version,
                               const wchar_t *path, ID3D12Device *device,
                               unsigned int sdk_version,
                               const void *feature_info)
{
   (void)project; (void)engine_type; (void)engine_version;
   (void)path; (void)feature_info;
   return ngx_init(0, device, 0, sdk_version);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_Init_with_ProjectID(const char *project,
                                    unsigned int engine_type,
                                    const char *engine_version,
                                    const wchar_t *path,
                                    ID3D12Device *device,
                                    const void *feature_info,
                                    unsigned int sdk_version)
{
   (void)project; (void)engine_type; (void)engine_version;
   (void)path; (void)feature_info;
   return ngx_init(0, device, 0, sdk_version);
}

static uint32_t
ngx_shutdown(int api, void *expected_identity = nullptr)
{
   NptNgxBridgeV1 *b;
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      if (g_sessions[api].state == NGX_SESSION_NONE)
         return NGX_RESULT_OK;
      if (expected_identity && expected_identity != g_sessions[api].identity)
         return NGX_RESULT_INVALID_PARAMETER;
      if (g_sessions[api].state != NGX_SESSION_LIVE)
         return NGX_RESULT_FAIL;
      for (const NgxFeature &f : g_features) {
         if (f.api == api && f.state == NGX_FEATURE_RELEASING)
            return NGX_RESULT_FAIL;
      }
      g_sessions[api].state = NGX_SESSION_SHUTTING_DOWN;
      b = g_sessions[api].bridge;
      b->lpVtbl->AddRef(b);
   }
   /* The host rejects a Shutdown that still has live features, so
    * release them first; SHUTTING_DOWN keeps new Create/Eval from
    * sneaking a feature in after this enumeration. */
   uint64_t tokens[NGX_MAX_FEATURES];
   unsigned ntokens = 0;
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      for (NgxFeature &f : g_features) {
         if (f.api == api && f.state == NGX_FEATURE_LIVE) {
            f.state = NGX_FEATURE_RELEASING;
            tokens[ntokens++] = f.token;
         }
      }
   }
   for (unsigned i = 0; i < ntokens; i++) {
      const uint32_t rr = b->lpVtbl->ReleaseFeature(b, tokens[i]);
      std::lock_guard<std::mutex> lock(g_mtx);
      for (NgxFeature &f : g_features) {
         if (f.api == api && f.state == NGX_FEATURE_RELEASING && f.token == tokens[i]) {
            /* Invalidation commits only on confirmed success; on any
             * failure the handle stays live and retryable. */
            if (rr == NGX_RESULT_OK)
               ngx_feature_free_locked(&f);
            else
               f.state = NGX_FEATURE_LIVE;
            break;
         }
      }
   }
   const uint32_t r = b->lpVtbl->Shutdown(b);
   b->lpVtbl->Release(b);
   ngx_logf("Shutdown -> 0x%x", r);
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      if (r == NGX_RESULT_OK || r == NPT_SR_RESULT_TRANSPORT_LOST) {
         /* Confirmed teardown -- or transport lost, in which case the
          * session can never be used or retried either. */
         ngx_session_clear_locked(api);
      } else {
         /* Explicit rejection: the session and any unreleased feature
          * handles stay usable. */
         g_sessions[api].state = NGX_SESSION_LIVE;
      }
   }
   return ngx_public_result(r);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_Shutdown()
{
   return ngx_shutdown(0);
}

static uint32_t
ngx_Shutdown1(int api, void *device)
{
   if (!device)
      return ngx_shutdown(api);
   void *id = ngx_identity(device);
   if (!id)
      return NGX_RESULT_INVALID_PARAMETER;
   uint32_t r = ngx_shutdown(api, id);
   ngx_com_release(id);
   return r;
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_AllocateParameters(void **out_params)
{
   if (!out_params)
      return NGX_RESULT_INVALID_PARAMETER;
   *out_params = ngx_new_params();
   return *out_params ? NGX_RESULT_OK : NGX_RESULT_FAIL;
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_DestroyParameters(void *params)
{
   delete static_cast<NgxParameters *>(params);
   return NGX_RESULT_OK;
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_GetParameters(void **out_params)
{
   return ngx_capability_params(0, out_params);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_GetCapabilityParameters(void **out_params)
{
   return ngx_capability_params(0, out_params);
}

struct NGXFeatureDiscoveryInfo {
   unsigned int SDKVersion;
   unsigned int FeatureID;
   char Identifier[32];
   const wchar_t *ApplicationDataPath;
   const void *FeatureInfo;
};

struct NGXFeatureRequirement {
   unsigned int FeatureSupported; /* 0 supported, 1 unsupported */
   unsigned int MinHWArchitecture;
   char MinOSVersion[255];
};

static uint32_t
ngx_GetFeatureRequirements(int api,
   IDXGIAdapter *adapter, const NGXFeatureDiscoveryInfo *discovery_info,
   NGXFeatureRequirement *requirement)
{
   if (!requirement || !discovery_info)
      return NGX_RESULT_INVALID_PARAMETER;
   requirement->FeatureSupported = 1;
   requirement->MinHWArchitecture = 0;
   requirement->MinOSVersion[0] = 0;
   if (discovery_info->FeatureID != 1) {
      requirement->FeatureSupported = 1;
      return NGX_RESULT_NOT_SUPPORTED;
   }

   static const char min_os[] = "10.0.16299.0";
   for (size_t i = 0; i < sizeof(min_os); i++)
      requirement->MinOSVersion[i] = min_os[i];

   void *p = nullptr;
   uint32_t sr = 0;
   uint32_t r = NGX_RESULT_NOT_SUPPORTED;
   if (adapter &&
       ngx_com_qi(adapter, &NPT_SR_IID_NGX_BRIDGE_V1, &p) ==
           NPT_SR_S_OK &&
       p) {
      NptNgxBridgeV1 *b = static_cast<NptNgxBridgeV1 *>(p);
      uint32_t kind = 0;
      uintptr_t cookie = 0;
      uint64_t id = 0;
      if (b->lpVtbl->GetRef(b, &kind, &cookie, &id) == NPT_SR_S_OK &&
          kind == NPT_SR_KIND_ADAPTER)
         r = b->lpVtbl->Caps(b, &sr);
      b->lpVtbl->Release(b);
   }
   if (r == NGX_RESULT_OK) {
      requirement->FeatureSupported = (sr & (api ? NGX_CAP_D3D11 : NGX_CAP_D3D12)) ? 0 : 1;
      std::lock_guard<std::mutex> lock(g_mtx);
      g_discovery_caps[api] = !requirement->FeatureSupported;
      return NGX_RESULT_OK;
   }
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      g_discovery_caps[api] = -1;
   }
   return ngx_public_result(r);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_GetScratchBufferSize(unsigned int feature,
                                     const void *params, size_t *out_size)
{
   (void)feature; (void)params;
   if (!out_size)
      return NGX_RESULT_INVALID_PARAMETER;
   *out_size = 0;
   return NGX_RESULT_OK;
}

static uint32_t
ngx_CreateFeature(int api, void *list,
                              unsigned int feature, void *params_v,
                              unsigned int **out_handle)
{
   if (out_handle)
      *out_handle = nullptr;
   NgxParameters *params = static_cast<NgxParameters *>(params_v);
   ngx_logf("CreateFeature list_present=%d feature=%u params_present=%d",
            list != nullptr, feature, params != nullptr);
   if (!list || !out_handle || !params || params->vt != ngx_param_vtbl)
      return NGX_RESULT_INVALID_PARAMETER;
   ngx_dump_params(params, "create");
   if (feature != 1) {
      ngx_logf("CreateFeature reject: feature id %u", feature);
      return NGX_RESULT_NOT_SUPPORTED;
   }

   NgxCreateScalars c;
   uint32_t r = ngx_read_create(params, &c);
   if (r != NGX_RESULT_OK)
      return ngx_public_result(r);

   std::lock_guard<std::mutex> lock(g_mtx);
   if (c.quality > 2 && g_sessions[api].extended != 1)
      return NGX_RESULT_NOT_SUPPORTED;
   if (g_sessions[api].state != NGX_SESSION_LIVE) {
      ngx_logf("CreateFeature reject: no live session (state=%d)",
               g_sessions[api].state);
      return NGX_RESULT_FAIL;
   }
   NptNgxBridgeV1 *b = ngx_get_bridge(api, list, api ? NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT : NPT_SR_KIND_LIST, nullptr,
                                    "create list");
   if (!b)
      return NGX_RESULT_INVALID_PARAMETER;

   if (!ngx_same_device(api, list)) {
      b->lpVtbl->Release(b);
      ngx_logf("CreateFeature reject: list not on session device");
      return NGX_RESULT_INVALID_PARAMETER;
   }
   NptSrCreateV1 create;
   create.in_w = c.in_w;
   create.in_h = c.in_h;
   create.out_w = c.out_w;
   create.out_h = c.out_h;
   create.quality = c.quality;
   create.flags = c.flags;
   uint64_t token = 0;
   r = b->lpVtbl->Create(b, g_sessions[api].id, &create, &token);
   b->lpVtbl->Release(b);
   ngx_logf("CreateFeature %ux%u -> %ux%u q=%d flags=0x%x -> 0x%x "
            "token=%llu", c.in_w, c.in_h, c.out_w, c.out_h, c.quality,
            c.flags, r, (unsigned long long)token);
   if (r != NGX_RESULT_OK)
      return ngx_public_result(r);

   NgxFeature *f = ngx_feature_alloc_locked();
   if (!f) {
      g_sessions[api].bridge->lpVtbl->ReleaseFeature(g_sessions[api].bridge, token);
      return NGX_RESULT_FAIL;
   }
   f->api = api;
   f->session_gen = g_sessions[api].session_gen;
   f->token = token;
   f->in_w = c.in_w;
   f->in_h = c.in_h;
   *out_handle = &f->handle;
   return NGX_RESULT_OK;
}

static uint32_t
ngx_eval(int api, void *list, const unsigned int *handle,
         void *params_v, void *callback)
{
#define NGX_EVAL_REJECT(r, why) \
   do { \
      ngx_logf("eval reject: %s -> 0x%x", why, (uint32_t)(r)); \
      return (r); \
   } while (0)

   if (callback)
      NGX_EVAL_REJECT(NGX_RESULT_NOT_SUPPORTED, "non-null callback");
   NgxParameters *params = static_cast<NgxParameters *>(params_v);
   if (!list || !handle || !params || params->vt != ngx_param_vtbl)
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                      "bad list/handle/params");
   ngx_dump_params(params, "eval");

   std::lock_guard<std::mutex> lock(g_mtx);
   if (g_sessions[api].state != NGX_SESSION_LIVE)
      NGX_EVAL_REJECT(NGX_RESULT_FAIL, "no live session");
   NgxFeature *f = ngx_feature_find_locked(handle);
   if (!f || f->api != api || f->state != NGX_FEATURE_LIVE)
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                      "unknown/inactive feature handle");
   NptNgxBridgeV1 *kind_check = ngx_get_bridge(
      api, list, api ? NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT : NPT_SR_KIND_LIST,
      nullptr, "eval context");
   if (!kind_check)
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER, "invalid context kind");
   kind_check->lpVtbl->Release(kind_check);
   if (!ngx_same_device(api, list))
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                      "list not on session device");

   /* Snapshot every semantic field under one parameter lock so a Set
    * racing the Eval cannot mix values from different batches.  An
    * absent optional key takes its default; a present key that does not
    * convert is a type error, not "unset". */
   float mv_x = 1.0f, mv_y = 1.0f, jitter_x = 0.0f, jitter_y = 0.0f;
   float pre_exposure = 0.0f;
   uint32_t render_w = f->in_w, render_h = f->in_h;
   uint32_t reset = 0;
   void *color = nullptr, *output = nullptr, *depth = nullptr;
   void *motion = nullptr, *exposure = nullptr;
   {
      std::lock_guard<std::mutex> plock(params->mtx);
      NgxParamValue v;
      if (ngx_param_find(params, "MV.Scale.X", &v) &&
          !ngx_param_convert(v, &mv_x))
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad MV.Scale.X type");
      if (ngx_param_find(params, "MV.Scale.Y", &v) &&
          !ngx_param_convert(v, &mv_y))
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad MV.Scale.Y type");
      if (ngx_param_find(params, "Jitter.Offset.X", &v) &&
          !ngx_param_convert(v, &jitter_x))
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad Jitter.Offset.X type");
      if (ngx_param_find(params, "Jitter.Offset.Y", &v) &&
          !ngx_param_convert(v, &jitter_y))
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad Jitter.Offset.Y type");
      if (ngx_param_find(params, "DLSS.Pre.Exposure", &v) &&
          !ngx_param_convert(v, &pre_exposure))
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad DLSS.Pre.Exposure type");
      if (ngx_param_try_get_exact_uint32(params, "Reset", &reset) == NGX_UINT_INVALID)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad Reset type");
      if (!ngx_param_find(params, "Color", &v) ||
          !ngx_param_convert(v, &color) || !color)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "missing/NULL Color resource");
      if (!ngx_param_find(params, "Output", &v) ||
          !ngx_param_convert(v, &output) || !output)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "missing/NULL Output resource");
      if (!ngx_param_find(params, "Depth", &v) ||
          !ngx_param_convert(v, &depth) || !depth)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "missing/NULL Depth resource");
      if (!ngx_param_find(params, "MotionVectors", &v) ||
          !ngx_param_convert(v, &motion) || !motion)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "missing/NULL MotionVectors resource");
      if (ngx_param_find(params, "ExposureTexture", &v) &&
          !ngx_param_convert(v, &exposure))
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                         "bad ExposureTexture type");

      static const char *const subrect_base_keys[] = {
         "DLSS.Render.Subrect.Base.X",
         "DLSS.Render.Subrect.Base.Y",
         "DLSS.Render.Subrect.Origin.X",
         "DLSS.Render.Subrect.Origin.Y",
         "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
         "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y",
         "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
         "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y",
         "DLSS.Input.Exposure.Subrect.Base.X", "DLSS.Input.Exposure.Subrect.Base.Y",
      };
      for (const char *key : subrect_base_keys) {
         uint32_t base = 0;
         if (ngx_param_try_get_exact_uint32(params, key, &base) == NGX_UINT_INVALID || base != 0)
            NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                            "nonzero subrect base");
      }
      if (ngx_param_try_get_exact_uint32(params, "DLSS.Render.Subrect.Dimensions.Width", &render_w) == NGX_UINT_INVALID ||
          !render_w || render_w > f->in_w)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER, "invalid subrect width");
      if (ngx_param_try_get_exact_uint32(params, "DLSS.Render.Subrect.Dimensions.Height", &render_h) == NGX_UINT_INVALID ||
          !render_h || render_h > f->in_h)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER, "invalid subrect height");
      uint32_t subrects = 0;
      if (ngx_param_try_get_exact_uint32(params, "DLSS.Enable.Output.Subrects", &subrects) == NGX_UINT_INVALID)
         NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER, "invalid output subrects");
      if (subrects != 0)
         NGX_EVAL_REJECT(NGX_RESULT_NOT_SUPPORTED, "output subrects enabled");
   }
   if (!std::isfinite(mv_x) || !std::isfinite(mv_y) ||
       !std::isfinite(jitter_x) || !std::isfinite(jitter_y) ||
       !std::isfinite(pre_exposure))
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                      "non-finite scalar");
   if (pre_exposure == 0.0f)
      pre_exposure = 1.0f;
   else if (pre_exposure < 0.0f)
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                      "negative DLSS.Pre.Exposure");
   if (reset != 0 && reset != 1)
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER, "bad Reset value");

   /* Resolve COM ids through each object's own bridge.  The tear-offs
    * hold strong references on the resources and stay alive until the
    * synchronous Eval submit completes. */
   NptNgxBridgeV1 *list_b =
      ngx_get_bridge(api, list, api ? NPT_NGX_BRIDGE_KIND_D3D11_CONTEXT : NPT_SR_KIND_LIST, nullptr, "eval list");
   if (!list_b)
      NGX_EVAL_REJECT(NGX_RESULT_INVALID_PARAMETER,
                      "list bridge lookup failed");

   NptSrEvalIdsV1 ids = {};
   ids.render_w = render_w;
   ids.render_h = render_h;
   ids.jitter_x = jitter_x;
   ids.jitter_y = jitter_y;
   ids.mv_scale_x = mv_x;
   ids.mv_scale_y = mv_y;
   ids.pre_exposure = pre_exposure;
   ids.reset = (uint32_t)reset;

   static const char *const res_names[5] = {
      "color", "output", "depth", "motion", "exposure",
   };
   void *resources[5] = {color, output, depth, motion, exposure};
   uint64_t *id_slots[5] = {&ids.color, &ids.output, &ids.depth,
                            &ids.motion, &ids.exposure};
   NptNgxBridgeV1 *res_bridges[5] = {nullptr};
   uint32_t r = NGX_RESULT_OK;
   for (int i = 0; i < 5; i++) {
      *id_slots[i] = 0;
      if (!resources[i])
         continue;
      res_bridges[i] =
         ngx_get_bridge(api, resources[i], api ? NPT_NGX_BRIDGE_KIND_D3D11_RESOURCE : NPT_SR_KIND_RESOURCE,
                        id_slots[i], res_names[i]);
      if (!res_bridges[i]) {
         r = NGX_RESULT_INVALID_PARAMETER;
         break;
      }
   }
   if (r == NGX_RESULT_OK)
      r = list_b->lpVtbl->Eval(list_b, f->token, &ids);
   ngx_logf("eval token=%llu render=%ux%u jitter=(%g,%g) mv=(%g,%g) "
            "pre=%g reset=%u ids=(%llu,%llu,%llu,%llu,%llu) -> 0x%x",
            (unsigned long long)f->token, ids.render_w, ids.render_h,
            jitter_x, jitter_y, mv_x, mv_y, pre_exposure,
            (unsigned)reset, (unsigned long long)ids.color,
            (unsigned long long)ids.output,
            (unsigned long long)ids.depth,
            (unsigned long long)ids.motion,
            (unsigned long long)ids.exposure, r);
   for (int i = 0; i < 5; i++) {
      if (res_bridges[i])
         res_bridges[i]->lpVtbl->Release(res_bridges[i]);
   }
   list_b->lpVtbl->Release(list_b);
   return ngx_public_result(r);
#undef NGX_EVAL_REJECT
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *list,
                                const unsigned int *handle,
                                void *params, void *callback)
{
   return ngx_eval(0, list, handle, params, callback);
}

NVNGX_API uint32_t
NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList *list,
                                  const unsigned int *handle,
                                  void *params, void *callback)
{
   return ngx_eval(0, list, handle, params, callback);
}

static uint32_t
ngx_ReleaseFeature(int api, unsigned int *handle)
{
   if (!handle)
      return NGX_RESULT_INVALID_PARAMETER;

   NptNgxBridgeV1 *b;
   uint64_t token, session_gen, feature_gen;
   size_t slot_index;
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      if (g_sessions[api].state != NGX_SESSION_LIVE)
         return NGX_RESULT_FAIL;
      NgxFeature *f = ngx_feature_find_locked(handle);
      if (!f || f->api != api || f->state != NGX_FEATURE_LIVE)
         return NGX_RESULT_INVALID_PARAMETER;
      f->state = NGX_FEATURE_RELEASING;
      token = f->token;
      session_gen = f->session_gen;
      feature_gen = f->feature_gen;
      slot_index = f - g_features;
      b = g_sessions[api].bridge;
      b->lpVtbl->AddRef(b);
   }
   uint32_t r = b->lpVtbl->ReleaseFeature(b, token);
   b->lpVtbl->Release(b);
   ngx_logf("ReleaseFeature token=%llu -> 0x%x",
            (unsigned long long)token, r);
   {
      std::lock_guard<std::mutex> lock(g_mtx);
      NgxFeature *f = &g_features[slot_index];
      if (f->state == NGX_FEATURE_RELEASING && f->api == api &&
          f->session_gen == session_gen && f->feature_gen == feature_gen &&
          f->token == token) {
         /* Commit invalidation only on confirmed success; on failure
          * the handle stays live so the caller may retry.  When the
          * transport is lost the remote effect is unknown, but the
          * session itself is unusable either way, so a retry simply
          * fails cleanly. */
         if (r == NGX_RESULT_OK)
            ngx_feature_free_locked(f);
         else
            f->state = NGX_FEATURE_LIVE;
      }
   }
   return ngx_public_result(r);
}

NVNGX_API uint32_t NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *d)
{ return ngx_Shutdown1(0, d); }
NVNGX_API uint32_t NVSDK_NGX_D3D12_GetFeatureRequirements(
   IDXGIAdapter *a, const NGXFeatureDiscoveryInfo *d, NGXFeatureRequirement *r)
{ return ngx_GetFeatureRequirements(0, a, d, r); }
NVNGX_API uint32_t NVSDK_NGX_D3D12_CreateFeature(
   ID3D12GraphicsCommandList *l, unsigned int f, void *p, unsigned int **h)
{ return ngx_CreateFeature(0, l, f, p, h); }
NVNGX_API uint32_t NVSDK_NGX_D3D12_ReleaseFeature(unsigned int *h)
{ return ngx_ReleaseFeature(0, h); }

NVNGX_API uint32_t NVSDK_NGX_D3D11_Init(
   unsigned long long id, const wchar_t *path, ID3D11Device *device,
   const void *feature_info, unsigned int sdk)
{ (void)path; (void)feature_info; return ngx_init(1, device, id, sdk); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_Init_Ext(
   unsigned long long id, const wchar_t *path, ID3D11Device *device,
   unsigned int sdk, const void *parameters)
{ (void)path; (void)parameters; return ngx_init(1, device, id, sdk); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_Init_ProjectID(
   const char *project, unsigned int engine, const char *version,
   const wchar_t *path, ID3D11Device *device, unsigned int sdk, const void *info)
{ (void)project; (void)engine; (void)version; (void)path; (void)info; return ngx_init(1, device, 0, sdk); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_Init_with_ProjectID(
   const char *project, unsigned int engine, const char *version,
   const wchar_t *path, ID3D11Device *device, const void *info, unsigned int sdk)
{ (void)project; (void)engine; (void)version; (void)path; (void)info; return ngx_init(1, device, 0, sdk); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_Shutdown() { return ngx_shutdown(1); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *d)
{ return ngx_Shutdown1(1, d); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_AllocateParameters(void **p)
{ return NVSDK_NGX_D3D12_AllocateParameters(p); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_DestroyParameters(void *p)
{ return NVSDK_NGX_D3D12_DestroyParameters(p); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_GetParameters(void **p)
{ return ngx_capability_params(1, p); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_GetCapabilityParameters(void **p)
{ return ngx_capability_params(1, p); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_GetFeatureRequirements(
   IDXGIAdapter *a, const NGXFeatureDiscoveryInfo *d, NGXFeatureRequirement *r)
{ return ngx_GetFeatureRequirements(1, a, d, r); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_GetScratchBufferSize(
   unsigned int f, const void *p, size_t *n)
{ return NVSDK_NGX_D3D12_GetScratchBufferSize(f, p, n); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_CreateFeature(
   ID3D11DeviceContext *c, unsigned int f, void *p, unsigned int **h)
{ return ngx_CreateFeature(1, c, f, p, h); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_EvaluateFeature(
   ID3D11DeviceContext *c, const unsigned int *h, void *p, void *cb)
{ return ngx_eval(1, c, h, p, cb); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_EvaluateFeature_C(
   ID3D11DeviceContext *c, const unsigned int *h, void *p, void *cb)
{ return ngx_eval(1, c, h, p, cb); }
NVNGX_API uint32_t NVSDK_NGX_D3D11_ReleaseFeature(unsigned int *h)
{ return ngx_ReleaseFeature(1, h); }

#if defined(_WIN32)
BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
   (void)instance; (void)reason; (void)reserved;
   return TRUE;
}
#endif
