/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#define TEST_INTERCEPT_WIN32
#include "swapchain_test_support.h"
#include "../npt_swapchain12.c"

struct d3d12_test {
   struct test_fixture *fixture;
   struct npt_guest_swapchain12 owner;
   struct npt_com_base *swapchain;
   struct npt_com_base *queue;
   struct npt_com_base *fence;
   uint64_t completed;
   uint64_t last_signal;
   unsigned signals;
   HRESULT signal_result;
};

static UINT64 NPT_STDMETHODCALLTYPE
test_completed_value(void *self)
{
   struct d3d12_test *t = ((struct npt_com_base *)self)->aux;
   CHECK(test_object_alive(t->fixture, self));
   return t->completed;
}

static HRESULT NPT_STDMETHODCALLTYPE
test_signal(void *self, ID3D12Fence *fence, UINT64 value)
{
   struct d3d12_test *t = ((struct npt_com_base *)self)->aux;
   CHECK((void *)fence == t->fence);
   CHECK(test_object_alive(t->fixture, self));
   CHECK(test_object_alive(t->fixture, fence));
   ++t->signals;
   t->last_signal = value;
   return t->signal_result;
}

static const struct npt_id3d12commandqueue_client_vtbl queue_vtbl = {
   .Signal = test_signal,
};
static const struct npt_id3d12fence_client_vtbl fence_vtbl = {
   .GetCompletedValue = test_completed_value,
};

static void
d3d12_init(struct d3d12_test *t)
{
   memset(t, 0, sizeof(*t));
   t->fixture = test_fixture_create();
   t->swapchain = test_object_create(t->fixture, sizeof(*t->swapchain), NULL, 1);
   t->queue = test_object_create(t->fixture, sizeof(*t->queue),
                                 (const void **)&queue_vtbl, 2);
   t->fence = test_object_create(t->fixture, sizeof(*t->fence),
                                 (const void **)&fence_vtbl, 3);
   t->queue->aux = t;
   t->fence->aux = t;
   DXGI_SWAP_CHAIN_DESC1 desc = {0};
   desc.Width = 64;
   desc.Height = 64;
   desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
   desc.SampleDesc.Count = 1;
   desc.BufferCount = 2;
   npt_gsc_init(&t->owner.base, t->swapchain, &desc, NULL, NULL, "test12");
   test_fixture_bind(t->fixture, &t->owner.base, t->swapchain);
   t->swapchain->aux = &t->owner;
   t->owner.queue = t->queue;
   t->owner.fence = t->fence;
   t->owner.fence_id = 3;
   t->owner.base.gpu_poll = gsc12_gpu_poll;
   t->owner.base.gpu_poll_ctx = &t->owner;
   t->signal_result = S_OK;
}

static void
d3d12_fini(struct d3d12_test *t)
{
   npt_gsc_fini(&t->owner.base);
   test_fixture_destroy(t->fixture);
}

static void
test_poll_and_drain(void)
{
   test_begin("d3d12-poll-drain");
   struct d3d12_test t;
   d3d12_init(&t);
   CHECK(gsc12_gpu_poll(&t.owner, 0, 0) == S_OK);
   CHECK(gsc12_gpu_poll(&t.owner, 0, 2) == S_FALSE);
   t.completed = 2;
   CHECK(gsc12_gpu_poll(&t.owner, 1, 2) == S_OK);
   t.completed = UINT64_MAX;
   CHECK(gsc12_gpu_poll(&t.owner, 1, 2) == NPT_DXGI_ERROR_DEVICE_REMOVED);

   void *resource = test_object_create(t.fixture, sizeof(struct npt_com_base),
                                       NULL, 4);
   t.owner.base.present_tex[0] = resource;
   t.owner.base.present_tex_id[0] = 4;
   t.completed = 0;
   test_enter_nonblocking(t.fixture);
   CHECK(gsc12_gpu_drain(&t.owner, true) == S_FALSE);
   CHECK(t.signals == 1 && t.last_signal == 1 && t.owner.drain_value == 1);
   CHECK(gsc12_gpu_drain(&t.owner, true) == S_FALSE);
   CHECK(t.signals == 1 && t.last_signal == 1 && t.owner.drain_value == 1);
   t.completed = 1;
   CHECK(gsc12_gpu_drain(&t.owner, true) == S_OK);
   CHECK(t.signals == 1 && t.owner.drain_value == 0);
   t.signal_result = E_FAIL;
   CHECK(gsc12_gpu_drain(&t.owner, true) == E_FAIL);
   CHECK(t.signals == 2 && t.owner.drain_value == 0);
   test_leave_nonblocking(t.fixture);
   CHECK(test_object_alive(t.fixture, resource));
   CHECK(test_object_release_count(t.fixture, resource) == 0);
   d3d12_fini(&t);
   test_end();
}


void
test_d3d12_group(void)
{
   test_poll_and_drain();
}
