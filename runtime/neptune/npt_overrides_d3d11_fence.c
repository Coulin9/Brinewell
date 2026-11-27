/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_event.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d11fence.h"
#include "neptune-protocol/npt_protocol_guest_id3d11fence.h"

static bool
fence_reached(void *self, uint64_t value)
{
   if (npt_device_lost(npt_com_self_device(self)))
      return true;
   return npt_id3d11fence_default_GetCompletedValue(self) >= value;
}

static UINT64 NPT_STDMETHODCALLTYPE
fence_GetCompletedValue_override(void *self)
{
   if (npt_device_lost(npt_com_self_device(self)))
      return UINT64_MAX;
   return npt_id3d11fence_default_GetCompletedValue(self);
}

static void
fence_block_arm(void *self, uint64_t value, uint64_t token)
{
   struct npt_device *dev = npt_com_self_device(self);
   npt_async_ID3D11Fence_SetEventOnCompletion(
      npt_device_method_ring(dev), npt_com_self_id(self), value,
      (HANDLE)(uintptr_t)token);
}

static const struct npt_event_block_ops fence_block_ops = {
   .reached = fence_reached,
   .reached_sync = fence_reached,
   .arm = fence_block_arm,
   .name = "d3d11",
};

static HRESULT NPT_STDMETHODCALLTYPE
fence_SetEventOnCompletion_override(void *self, UINT64 Value, HANDLE hEvent)
{
   struct npt_device *dev = npt_com_self_device(self);

   if (npt_device_lost(dev)) {
      if (hEvent)
         SetEvent(hEvent);
      return NPT_S_OK;
   }

   if (!hEvent) {
      npt_event_block_until_value(dev, &fence_block_ops, self, Value);
      return NPT_S_OK;
   }

   uint64_t token = npt_event_arm(dev, (void *)hEvent);
   if (!token)
      return NPT_E_OUTOFMEMORY;

   npt_async_ID3D11Fence_SetEventOnCompletion(
      npt_device_method_ring(dev), npt_com_self_id(self), Value,
      (HANDLE)(uintptr_t)token);
   return NPT_S_OK;
}

void
npt_overrides_d3d11_fence_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d11fence, SetEventOnCompletion,
                         fence_SetEventOnCompletion_override);
   NPT_REGISTER_OVERRIDE(id3d11fence, GetCompletedValue,
                         fence_GetCompletedValue_override);
}
