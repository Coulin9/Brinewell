/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Minimal nvapi64.dll: only nvapi_QueryInterface is exported, and only
 * the handful of functions a game needs to conclude "an NVIDIA driver
 * is present" for the NGX opt-in path.  A single process-local physical
 * GPU handle is reported; the driver version is a fixed compatibility
 * value, not the real host driver.  When the NGX payload does not pass
 * the process-side visibility check (npt_ngx_visible_for_process)
 * Initialize fails, matching the non-spoofed adapter identity the DXGI
 * side presents in that case.
 */

#include "npt_ngx_visibility.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#endif

typedef int32_t NvAPI_Status;
typedef uint32_t NvU32;
typedef void *NvPhysicalGpuHandle;
typedef char NvAPI_ShortString[64];

#define NVAPI_OK                    ((NvAPI_Status)0)
#define NVAPI_ERROR                 ((NvAPI_Status)-1)
#define NVAPI_INVALID_ARGUMENT      ((NvAPI_Status)-5)
#define NVAPI_API_NOT_INITIALIZED   ((NvAPI_Status)-4)
#define NVAPI_INCOMPATIBLE_STRUCT_VERSION ((NvAPI_Status)-9)

typedef struct {
   NvU32 version;
   NvU32 architecture;
   NvU32 implementation;
   NvU32 revision;
} NV_GPU_ARCH_INFO;

typedef struct {
   NvU32 version;
   uint8_t bLowLatencyMode;
   uint8_t bFsVrr;
   uint8_t bCplVsyncOn;
   NvU32 sleepIntervalUs;
   uint8_t bUseGameSleep;
   uint8_t rsvd[121];
} NV_GET_SLEEP_STATUS_PARAMS;

_Static_assert(sizeof(NV_GPU_ARCH_INFO) == 16, "NV_GPU_ARCH_INFO size");
_Static_assert(sizeof(NV_GET_SLEEP_STATUS_PARAMS) == 136, "sleep status size");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, version) == 0, "version offset");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, bLowLatencyMode) == 4, "low latency offset");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, bFsVrr) == 5, "VRR offset");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, bCplVsyncOn) == 6, "vsync offset");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, sleepIntervalUs) == 8, "interval offset");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, bUseGameSleep) == 12, "game sleep offset");
_Static_assert(offsetof(NV_GET_SLEEP_STATUS_PARAMS, rsvd) == 13, "reserved offset");

/* Reference-counted init state: each successful Initialize pairs with
 * one Unload; EnumPhysicalGPUs requires a positive count. */
static _Atomic int nvapi_init_count;
static int nvapi_gpu;
static int nvapi_logical_gpu;
static _Atomic unsigned nvapi_unknown_count;

static int
nvapi_active(void)
{
   return npt_ngx_visible_for_process() ? 1 : 0;
}

static NvAPI_Status
NvAPI_Initialize(void)
{
   if (!nvapi_active())
      return NVAPI_ERROR;
   atomic_fetch_add_explicit(&nvapi_init_count, 1, memory_order_acq_rel);
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_Unload(void)
{
   int cur = atomic_load_explicit(&nvapi_init_count, memory_order_acquire);
   for (;;) {
      if (cur <= 0)
         return NVAPI_API_NOT_INITIALIZED;
      if (atomic_compare_exchange_weak_explicit(&nvapi_init_count, &cur,
                                                cur - 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire))
         return NVAPI_OK;
   }
}

static NvAPI_Status
NvAPI_SYS_GetDriverAndBranchVersion(NvU32 *pDriverVersion,
                                    NvAPI_ShortString szBuildBranchString)
{
   if (!pDriverVersion || !szBuildBranchString)
      return NVAPI_INVALID_ARGUMENT;
   *pDriverVersion = 57000;
   memset(szBuildBranchString, 0, sizeof(NvAPI_ShortString));
   memcpy(szBuildBranchString, "r570_00", sizeof("r570_00") - 1);
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GetInterfaceVersionString(NvAPI_ShortString szDesc)
{
   static const char version[] =
      "NVAPI Open Source Interface (Brinewell-NVAPI)";
   if (!szDesc)
      return NVAPI_INVALID_ARGUMENT;
   memset(szDesc, 0, sizeof(NvAPI_ShortString));
   memcpy(szDesc, version, sizeof(version) - 1);
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GetErrorMessage(NvAPI_Status status, NvAPI_ShortString szDesc)
{
   const char *text;
   switch (status) {
   case NVAPI_OK:
      text = "NVAPI_OK";
      break;
   case NVAPI_INVALID_ARGUMENT:
      text = "NVAPI_INVALID_ARGUMENT";
      break;
   case NVAPI_API_NOT_INITIALIZED:
      text = "NVAPI_API_NOT_INITIALIZED";
      break;
   case NVAPI_INCOMPATIBLE_STRUCT_VERSION:
      text = "NVAPI_INCOMPATIBLE_STRUCT_VERSION";
      break;
   default:
      text = "NVAPI_ERROR";
      break;
   }
   if (!szDesc)
      return NVAPI_INVALID_ARGUMENT;
   memset(szDesc, 0, sizeof(NvAPI_ShortString));
   memcpy(szDesc, text, strlen(text));
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_EnumPhysicalGPUs(NvPhysicalGpuHandle nvGPUHandle[64],
                       NvU32 *pGpuCount)
{
   if (!nvGPUHandle || !pGpuCount)
      return NVAPI_INVALID_ARGUMENT;
   if (atomic_load_explicit(&nvapi_init_count, memory_order_acquire) <= 0)
      return NVAPI_API_NOT_INITIALIZED;
   nvGPUHandle[0] = (NvPhysicalGpuHandle)&nvapi_gpu;
   *pGpuCount = 1;
   return NVAPI_OK;
}

static NvAPI_Status
nvapi_check_gpu(NvPhysicalGpuHandle gpu)
{
   if (atomic_load_explicit(&nvapi_init_count, memory_order_acquire) <= 0)
      return NVAPI_API_NOT_INITIALIZED;
   if (gpu != &nvapi_gpu)
      return NVAPI_INVALID_ARGUMENT;
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GPU_GetArchInfo(NvPhysicalGpuHandle gpu, NV_GPU_ARCH_INFO *info)
{
   NvAPI_Status status = nvapi_check_gpu(gpu);
   if (status != NVAPI_OK)
      return status;
   if (!info)
      return NVAPI_INVALID_ARGUMENT;
   if (info->version != 0x10010 && info->version != 0x20010)
      return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
   info->architecture = 0x170;
   info->implementation = 0x4;
   info->revision = 0x11;
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GetLogicalGPUFromPhysicalGPU(NvPhysicalGpuHandle gpu, void **logical)
{
   NvAPI_Status status = nvapi_check_gpu(gpu);
   if (status != NVAPI_OK)
      return status;
   if (!logical)
      return NVAPI_INVALID_ARGUMENT;
   *logical = &nvapi_logical_gpu;
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GPU_GetAdapterIdFromPhysicalGpu(NvPhysicalGpuHandle gpu, void *adapter_id)
{
   NvAPI_Status status = nvapi_check_gpu(gpu);
   if (status != NVAPI_OK)
      return status;
   if (!adapter_id)
      return NVAPI_INVALID_ARGUMENT;
#if defined(_WIN32) || defined(NPT_NGX_TEST_HOOKS)
   static _Atomic int cache_state;
   static uint32_t luid[2];
   for (;;) {
      int state = atomic_load_explicit(&cache_state, memory_order_acquire);
      if (state == 2)
         break;
      if (state == 0 && atomic_compare_exchange_strong_explicit(
             &cache_state, &state, 1, memory_order_acq_rel, memory_order_acquire)) {
         int ok = 0;
#if defined(_WIN32)
         HMODULE dxgi = LoadLibraryA("dxgi.dll");
         if (dxgi) {
            typedef HRESULT (WINAPI *create_factory_fn)(REFIID, void **);
            create_factory_fn create_factory = (create_factory_fn)(void *)
               GetProcAddress(dxgi, "CreateDXGIFactory1");
            static const GUID iid_factory1 =
               {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
            IDXGIFactory1 *factory = NULL;
            if (create_factory && SUCCEEDED(create_factory(&iid_factory1, (void **)&factory))) {
               IDXGIAdapter1 *adapter = NULL;
               if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter))) {
                  DXGI_ADAPTER_DESC1 desc;
                  if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
                     memcpy(luid, &desc.AdapterLuid, sizeof(luid));
                     ok = 1;
                  }
                  IDXGIAdapter1_Release(adapter);
               }
               IDXGIFactory1_Release(factory);
            }
            FreeLibrary(dxgi);
         }
#else
         extern int npt_nvapi_test_get_luid(uint32_t out[2]);
         ok = npt_nvapi_test_get_luid(luid);
#endif
         atomic_store_explicit(&cache_state, ok ? 2 : 0, memory_order_release);
         if (!ok)
            return NVAPI_ERROR;
         break;
      }
   }
   memcpy(adapter_id, luid, sizeof(luid));
#else
   const uint32_t luid[2] = { 1, 0 };
   memcpy(adapter_id, luid, sizeof(luid));
#endif
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GPU_GetFullName(NvPhysicalGpuHandle gpu, NvAPI_ShortString name)
{
   NvAPI_Status status = nvapi_check_gpu(gpu);
   if (status != NVAPI_OK)
      return status;
   if (!name)
      return NVAPI_INVALID_ARGUMENT;
   memset(name, 0, sizeof(NvAPI_ShortString));
   memcpy(name, "NVIDIA GeForce RTX 3070", sizeof("NVIDIA GeForce RTX 3070"));
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_GPU_GetPCIIdentifiers(NvPhysicalGpuHandle gpu, NvU32 *device,
                            NvU32 *subsystem, NvU32 *revision, NvU32 *ext_device)
{
   NvAPI_Status status = nvapi_check_gpu(gpu);
   if (status != NVAPI_OK)
      return status;
   if (!device || !subsystem || !revision || !ext_device)
      return NVAPI_INVALID_ARGUMENT;
   *device = (0x2484u << 16) | 0x10deu;
   *subsystem = 0;
   *revision = 0xa1;
   *ext_device = 0x2484;
   return NVAPI_OK;
}

static NvAPI_Status
NvAPI_D3D_GetSleepStatus(void *dev, NV_GET_SLEEP_STATUS_PARAMS *params)
{
   if (atomic_load_explicit(&nvapi_init_count, memory_order_acquire) <= 0)
      return NVAPI_API_NOT_INITIALIZED;
   if (!dev || !params)
      return NVAPI_INVALID_ARGUMENT;
   if (params->version != 0x10088)
      return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
   params->bLowLatencyMode = 0;
   params->bFsVrr = 0;
   params->bCplVsyncOn = 0;
   params->sleepIntervalUs = 0;
   params->bUseGameSleep = 0;
   return NVAPI_OK;
}

void *nvapi_QueryInterface(NvU32 id);

void *
nvapi_QueryInterface(NvU32 id)
{
   switch (id) {
   case 0x0150e828:
      return (void *)&NvAPI_Initialize;
   case 0xd22bdd7e:
      return (void *)&NvAPI_Unload;
   case 0x2926aaad:
      return (void *)&NvAPI_SYS_GetDriverAndBranchVersion;
   case 0x01053fa5:
      return (void *)&NvAPI_GetInterfaceVersionString;
   case 0x6c2d048c:
      return (void *)&NvAPI_GetErrorMessage;
   case 0xe5ac921f:
      return (void *)&NvAPI_EnumPhysicalGPUs;
   case 0xd8265d24:
      return (void *)&NvAPI_GPU_GetArchInfo;
   case 0xadd604d1:
      return (void *)&NvAPI_GetLogicalGPUFromPhysicalGPU;
   case 0x0ff07fde:
      return (void *)&NvAPI_GPU_GetAdapterIdFromPhysicalGpu;
   case 0xceee8e9f:
      return (void *)&NvAPI_GPU_GetFullName;
   case 0x2ddfb66e:
      return (void *)&NvAPI_GPU_GetPCIIdentifiers;
   case 0xaef96ca1:
      return (void *)&NvAPI_D3D_GetSleepStatus;
   default: {
      const char *debug = getenv("NPT_DEBUG");
      const char *ngx_debug = getenv("BRINEWELL_NGX_DEBUG");
      if ((debug && strstr(debug, "ngx")) || (ngx_debug && *ngx_debug)) {
         if (atomic_fetch_add_explicit(&nvapi_unknown_count, 1, memory_order_relaxed) < 256) {
            char message[64];
            snprintf(message, sizeof(message), "nvapi64: unknown QueryInterface id=0x%08x\n", id);
#if defined(_WIN32)
            OutputDebugStringA(message);
#else
            fputs(message, stderr);
#endif
         }
      }
      return NULL;
   }
   }
}
