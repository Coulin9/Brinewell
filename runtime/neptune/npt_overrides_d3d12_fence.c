/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Fence{,1} overrides:
 *  - SetEventOnCompletion is skip_default: the NULL-event form blocks
 *    the calling thread on a sync round trip that the host waits
 *    out, and the event form pre-arms the host eventfd proxy so the
 *    host signals it on GPU completion.
 *  - GetCompletedValue reads the per-fence feedback slot (plain
 *    snapshot; D3D12 fences may be rewound, so the value is whatever
 *    the host last observed, not a monotonic max).
 *  - Device::CreateFence defers to the default thunk, then seeds +
 *    registers the feedback slot.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_event.h"
#include "npt_overrides.h"
#include "npt_ring.h"
#include "neptune-protocol/npt_protocol_transport.h"

#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_client_id3d12fence.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d12fence.h"

static const GUID *const fence12_tiers[] = {
   &NPT_IID_ID3D12Fence, &NPT_IID_ID3D12Fence1, NULL,
};

/* Slot layout shared with D3D11 (npt_d3d11_fence_feedback_slot). */
struct npt_d3d12_fence_aux {
   struct npt_d3d11_feedback_aux base;
   /* Highest value ever requested through Fence::Signal or
    * CommandQueue::Signal; used to detect a rewind (D3D12 permits
    * Signal to a lower value). */
   _Atomic uint64_t signal_high;
   /* Sticky once any Signal decreases below signal_high.  The feedback
    * slot is a non-monotonic snapshot, so after a rewind it can read
    * stale-high; a latching wait decision (SetEventOnCompletion) off
    * that snapshot would fire early.  Rewound fences bypass the
    * snapshot fast paths and use the host-authoritative arm / sync. */
   _Atomic bool rewound;
   /* First queue that ever signalled this fence through
    * CommandQueue::Signal (queue object id; 0 = none yet).  Two
    * queues signalling the same fence can execute their signals in
    * either order on the GPU, so the feedback slot's snapshot can read
    * stale-high even while each queue's requested values increase;
    * detecting that case switches the fence to sync queries exactly
    * like a rewind. */
   _Atomic uint64_t first_queue;
   D3D12_FENCE_FLAGS creation_flags;
   bool creation_flags_valid;
};

static void npt_d3d12_fence_aux_destroy(void *aux_raw);

static void
npt_d3d12_fence_aux_init(struct npt_com_base *com,
                         struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_fence_aux *aux = com->aux;
   aux->base.com = com;
   aux->base.fb_shmem = NULL;
   aux->base.fb_offset = 0;
   aux->base.registered = false;
   atomic_store_explicit(&aux->signal_high, 0, memory_order_relaxed);
   atomic_store_explicit(&aux->rewound, false, memory_order_relaxed);
   atomic_store_explicit(&aux->first_queue, 0, memory_order_relaxed);
   com->aux_destroy = npt_d3d12_fence_aux_destroy;
   /* Slot alloc + REGISTER happen post-Create (aux_init has no
    * InitialValue).  A fence opened from a shared handle stays
    * unregistered and takes the sync GetCompletedValue: another device
    * can signal it out of the guest's sight. */
   (void)dev; (void)host_id;
}

/* Record a requested Signal value and flag the fence non-monotonic on
 * any decrease.  Cross-TU: the queue's Signal override reaches the
 * fence aux through npt_d3d12_fence_note_signal below. */
static void
fence12_note_signal(struct npt_d3d12_fence_aux *aux, UINT64 value)
{
   if (!aux)
      return;
   uint64_t hi = atomic_load_explicit(&aux->signal_high, memory_order_relaxed);
   for (;;) {
      if (value < hi) {
         atomic_store_explicit(&aux->rewound, true, memory_order_release);
         return;
      }
      if (atomic_compare_exchange_weak_explicit(
             &aux->signal_high, &hi, value, memory_order_relaxed,
             memory_order_relaxed))
         return;
   }
}

static void
npt_d3d12_fence_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_fence_aux *aux = aux_raw;
   /* Host-side unregister piggybacks on COM_RELEASE. */
   if (aux->base.fb_shmem && aux->base.com && aux->base.com->base.device) {
      npt_renderer_shmem_unref(aux->base.com->base.device->renderer,
                               aux->base.fb_shmem);
   }
   free(aux);
}

void npt_d3d12_fence_finalize_create(struct npt_device *dev, void *wrapper,
                                     UINT64 initial_value,
                                     D3D12_FENCE_FLAGS flags);

void
npt_d3d12_fence_finalize_create(struct npt_device *dev, void *wrapper,
                                UINT64 initial_value,
                                D3D12_FENCE_FLAGS flags)
{
   if (!dev || !wrapper)
      return;
   struct npt_com_base *com = wrapper;
   struct npt_d3d12_fence_aux *aux = com->aux;
   if (!aux)
      return;
   aux->creation_flags = flags;
   aux->creation_flags_valid = true;
   if (NPT_PERF(NO_FENCE_FEEDBACK))
      return;
   if (flags & (D3D12_FENCE_FLAG_SHARED |
                D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER))
      return;
   if (aux->base.registered)
      return;

   atomic_store_explicit(&aux->signal_high, initial_value,
                         memory_order_relaxed);

   uint32_t fb_offset = 0;
   struct npt_renderer_shmem *shmem =
      npt_device_alloc_feedback_slot(dev, NPT_FENCE_FEEDBACK_SLOT_SIZE,
                                     &fb_offset);
   if (!shmem)
      return;

   /* Seed with InitialValue: the host poll only writes after a
    * Signal, so pre-Signal reads would otherwise see memset-zero. */
   struct npt_d3d11_fence_feedback_slot *slot =
      (struct npt_d3d11_fence_feedback_slot *)
      ((uint8_t *)shmem->mmap_ptr + fb_offset);
   atomic_store_explicit(&slot->completed_value, initial_value,
                         memory_order_relaxed);
   atomic_store_explicit(&slot->flags, 0, memory_order_relaxed);

   aux->base.fb_shmem = shmem;
   aux->base.fb_offset = fb_offset;

   npt_ring_ensure_shmem_visible(dev->ring, shmem);

   if (npt_dispatch_feedback_register_fence(dev->ring, com->base.id,
                                            shmem->res_id, fb_offset,
                                            NPT_FENCE_FEEDBACK_API_D3D12))
      aux->base.registered = true;
}

/* NULL on wrappers outside the fence family. */
static struct npt_d3d12_fence_aux *
fence12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_fence_aux_destroy);
}

static struct npt_d3d11_fence_feedback_slot *
fence12_slot(struct npt_d3d12_fence_aux *aux)
{
   if (!aux || !aux->base.fb_shmem)
      return NULL;
   return (struct npt_d3d11_fence_feedback_slot *)
      ((uint8_t *)aux->base.fb_shmem->mmap_ptr + aux->base.fb_offset);
}

static bool
fence12_slot_value(struct npt_d3d12_fence_aux *aux, UINT64 *out)
{
   struct npt_d3d11_fence_feedback_slot *slot = fence12_slot(aux);
   if (!slot || !aux->base.registered ||
       atomic_load_explicit(&aux->rewound, memory_order_acquire))
      return false;
   const uint64_t flags = atomic_load_explicit(&slot->flags, memory_order_acquire);
   if (!(flags & NPT_FENCE_FEEDBACK_FLAG_LIVE) ||
       (flags & NPT_FENCE_FEEDBACK_FLAG_REWOUND))
      return false;
   /* Acquire pairs with the host's release store.  The snapshot is a
    * valid lower bound, matching the API contract. */
   *out = atomic_load_explicit(&slot->completed_value,
                              memory_order_acquire);
   return true;
}

static UINT64 NPT_STDMETHODCALLTYPE
fence12_GetCompletedValue_override(void *self)
{
   if (npt_device_lost(npt_com_self_device(self)))
      return UINT64_MAX;
   UINT64 value;
   if (!fence12_slot_value(fence12_aux(self), &value)) {
      /* Env opt-out, pool OOM, a fence not made by CreateFence, a
       * shared/handle-opened fence, or a rewound fence whose snapshot
       * can read stale-high -- take the authoritative sync read. */
      return npt_id3d12fence_default_GetCompletedValue(self);
   }
   return value;
}

static bool
fence12_reached(void *self, uint64_t value)
{
   if (npt_device_lost(npt_com_self_device(self)))
      return true;
   UINT64 completed;
   if (fence12_slot_value(fence12_aux(self), &completed))
      return completed >= value;
   /* Rewound fence (snapshot can read stale-high) or no slot: take the
    * authoritative sync read. */
   return npt_id3d12fence_default_GetCompletedValue(self) >= value;
}

static bool
fence12_reached_sync(void *self, uint64_t value)
{
   if (npt_device_lost(npt_com_self_device(self)))
      return true;
   return npt_id3d12fence_default_GetCompletedValue(self) >= value;
}

static void
fence12_block_arm(void *self, uint64_t value, uint64_t token)
{
   struct npt_device *dev = npt_com_self_device(self);
   npt_async_ID3D12Fence_SetEventOnCompletion(
      npt_device_method_ring(dev), npt_com_self_id(self), value,
      (HANDLE)(uintptr_t)token);
}

static const struct npt_event_block_ops fence12_block_ops = {
   .reached = fence12_reached,
   .reached_sync = fence12_reached_sync,
   .arm = fence12_block_arm,
   .name = "d3d12",
};

static HRESULT NPT_STDMETHODCALLTYPE
fence12_SetEventOnCompletion_body(void *self, UINT64 Value, HANDLE hEvent)
{
   struct npt_device *dev = npt_com_self_device(self);

   if (npt_device_lost(dev)) {
      if (hEvent)
         SetEvent(hEvent);
      return NPT_S_OK;
   }

   /* NULL-event form: block guest-side until the fence reaches Value;
    * never park the host ring dispatch thread in the backend's
    * blocking SEOC(NULL). */
   if (!hEvent) {
      npt_event_block_until_value(dev, &fence12_block_ops, self, Value);
      return NPT_S_OK;
   }

   /* Fast path: the host publishes completed values to the feedback
    * slot; if the fence already reached Value, signal the event right
    * here and skip the wire arm entirely, which pacing waits hit
    * often.  A stale-low slot only means we
    * fall through to the normal arm -- conservative.  Skipped for a
    * rewound fence: its snapshot can read stale-high, which would latch
    * the event early with no way to un-signal. */
   UINT64 completed;
   if (fence12_slot_value(fence12_aux(self), &completed) &&
       completed >= Value) {
      SetEvent(hEvent);
      return NPT_S_OK;
   }

   /* Event form: arm first so the host's npt_win32_handle_replace sees
    * the proxy when it decodes hEvent; the registration itself then
    * rides async on the same ring -- ordered after the arm, and the
    * host fence worker fires the proxy whenever the value is reached,
    * so nothing needs a reply. */
   uint64_t token = npt_event_arm(dev, (void *)hEvent);
   if (!token)
      return NPT_E_OUTOFMEMORY;
   /* The wire HANDLE slot carries the single-use token; the host
    * resolves it to this arm's proxy. */
   npt_async_ID3D12Fence_SetEventOnCompletion(
      npt_device_method_ring(dev), npt_com_self_id(self), Value,
      (HANDLE)(uintptr_t)token);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
fence12_SetEventOnCompletion_override(void *self, UINT64 Value, HANDLE hEvent)
{
   if (!hEvent)
      return fence12_SetEventOnCompletion_body(self, Value, hEvent);
   npt_event_trace_seoc_enter();
   const HRESULT hr = fence12_SetEventOnCompletion_body(self, Value, hEvent);
   npt_event_trace_seoc_leave();
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
fence12_Signal_override(void *self, UINT64 Value)
{
   fence12_note_signal(fence12_aux(self), Value);
   return npt_call_ID3D12Fence_Signal(npt_com_self_ring(self),
                                     npt_com_self_id(self), Value);
}

static D3D12_FENCE_FLAGS NPT_STDMETHODCALLTYPE
fence1_GetCreationFlags_override(void *self)
{
   struct npt_d3d12_fence_aux *aux = fence12_aux(self);
   if (aux && aux->creation_flags_valid)
      return aux->creation_flags;
   return npt_id3d12fence1_default_GetCreationFlags(self);
}

/* Cross-TU (npt_overrides_d3d12_queue.c): a CommandQueue::Signal
 * requests a fence value too, and can rewind it.  `queue_id` is the
 * signalling queue's object id; a second distinct queue latches the
 * fence into sync queries (GPU-side signal order across queues is
 * unordered, so the snapshot can read stale-high). */
void npt_d3d12_fence_note_signal(void *fence_wrapper, UINT64 value,
                                 uint64_t queue_id);

void
npt_d3d12_fence_note_signal(void *fence_wrapper, UINT64 value,
                            uint64_t queue_id)
{
   struct npt_d3d12_fence_aux *aux = fence12_aux(fence_wrapper);
   if (aux && queue_id) {
      uint64_t expected = 0;
      if (!atomic_compare_exchange_strong_explicit(
             &aux->first_queue, &expected, queue_id,
             memory_order_acq_rel, memory_order_acquire) &&
          expected != queue_id)
         atomic_store_explicit(&aux->rewound, true, memory_order_release);
   }
   fence12_note_signal(aux, value);
}

/*
 * Cross-TU entry for the D3D12 DDI (tritonQueue12.c): arm a GPU-true gate
 * on `fence_wrapper` (an ID3D12Fence npt wrapper -- the per-queue drain
 * fence) reaching `value`.  Wires up: KMD event-ring fence + token
 * (npt_event_gate_arm) and the host-side SetEventOnCompletion that
 * fires the proxy when the drain value completes on the GPU.  The caller
 * embeds *kmd_token_out in a VIOGPU_CMD_GATE DMA packet on the queue's
 * kernel context so dxgkrnl's monitored-fence packets wait for it.
 */
BOOL npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value, UINT64 *kmd_token_out);

BOOL
npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value, UINT64 *kmd_token_out)
{
   struct npt_device *dev = npt_com_self_device(fence_wrapper);
   if (!dev)
      return 0;
   uint64_t kmd = npt_event_gate_arm(dev, npt_com_self_id(fence_wrapper), value);
   if (!kmd)
      return 0;
   *kmd_token_out = kmd;
   return 1;
}

void
npt_overrides_d3d12_fence_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d12fence, SetEventOnCompletion,
                                     fence12_SetEventOnCompletion_override);
   NPT_REGISTER_OVERRIDE(id3d12fence, GetCompletedValue,
                                     fence12_GetCompletedValue_override);
   NPT_REGISTER_OVERRIDE(id3d12fence, Signal, fence12_Signal_override);
   NPT_REGISTER_OVERRIDE(id3d12fence1, GetCreationFlags,
                                     fence1_GetCreationFlags_override);
   npt_com_register_family(fence12_tiers,
                           sizeof(struct npt_d3d12_fence_aux),
                           npt_d3d12_fence_aux_init);
}
