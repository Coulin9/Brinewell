/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#define TEST_INTERCEPT_WIN32
#include "swapchain_test_support.h"
#include "../npt_swapchain.c"

struct d3d11_test {
   struct test_fixture *fixture;
   struct npt_guest_swapchain owner;
   struct npt_com_base *swapchain;
   struct npt_com_base *context;
   struct npt_com_base *context4;
   struct npt_com_base *fence;
   struct npt_com_base *queries[NPT_SC_PRESENT_IMAGES + 1];
   uint64_t completed;
   uint64_t last_signal;
   unsigned signals;
   unsigned ends;
   unsigned flushes;
   ID3D11Asynchronous *last_query;
   HRESULT query_result;
   BOOL query_done;
};

static UINT64 NPT_STDMETHODCALLTYPE
test_completed_value(void *self)
{
   struct d3d11_test *t = ((struct npt_com_base *)self)->aux;
   CHECK(test_object_alive(t->fixture, self));
   return t->completed;
}

static HRESULT NPT_STDMETHODCALLTYPE
test_signal(void *self, ID3D11Fence *fence, UINT64 value)
{
   struct d3d11_test *t = ((struct npt_com_base *)self)->aux;
   CHECK(test_object_alive(t->fixture, self));
   CHECK((void *)fence == t->fence && test_object_alive(t->fixture, fence));
   ++t->signals;
   t->last_signal = value;
   return S_OK;
}

static void NPT_STDMETHODCALLTYPE
test_end_query(void *self, ID3D11Asynchronous *query)
{
   struct d3d11_test *t = ((struct npt_com_base *)self)->aux;
   CHECK(test_object_alive(t->fixture, self));
   CHECK((void *)query == t->queries[NPT_SC_PRESENT_IMAGES]);
   CHECK(test_object_alive(t->fixture, query));
   ++t->ends;
}

static HRESULT NPT_STDMETHODCALLTYPE
test_get_data(void *self, ID3D11Asynchronous *query, void *data,
              UINT size, UINT flags)
{
   struct d3d11_test *t = ((struct npt_com_base *)self)->aux;
   CHECK(test_object_alive(t->fixture, self));
   CHECK(test_object_alive(t->fixture, query));
   CHECK(data && size == sizeof(BOOL));
   CHECK(flags == D3D11_ASYNC_GETDATA_DONOTFLUSH);
   t->last_query = query;
   *(BOOL *)data = t->query_done;
   return t->query_result;
}

static void NPT_STDMETHODCALLTYPE
test_flush(void *self)
{
   struct d3d11_test *t = ((struct npt_com_base *)self)->aux;
   CHECK(test_object_alive(t->fixture, self));
   ++t->flushes;
}

static const struct npt_id3d11devicecontext_client_vtbl context_vtbl = {
   .End = test_end_query,
   .GetData = test_get_data,
   .Flush = test_flush,
};
static const struct npt_id3d11devicecontext4_client_vtbl context4_vtbl = {
   .Signal = test_signal,
};
static const struct npt_id3d11fence_client_vtbl fence_vtbl = {
   .GetCompletedValue = test_completed_value,
};

static void
d3d11_init(struct d3d11_test *t)
{
   memset(t, 0, sizeof(*t));
   t->fixture = test_fixture_create();
   t->swapchain = test_object_create(t->fixture, sizeof(*t->swapchain), NULL, 1);
   t->context = test_object_create(t->fixture, sizeof(*t->context),
                                   (const void **)&context_vtbl, 2);
   t->context4 = test_object_create(t->fixture, sizeof(*t->context4),
                                    (const void **)&context4_vtbl, 3);
   t->fence = test_object_create(t->fixture, sizeof(*t->fence),
                                 (const void **)&fence_vtbl, 4);
   t->context->aux = t;
   t->context4->aux = t;
   t->fence->aux = t;
   for (unsigned i = 0; i <= NPT_SC_PRESENT_IMAGES; ++i)
      t->queries[i] = test_object_create(t->fixture, sizeof(struct npt_com_base),
                                         NULL, 5 + i);
   DXGI_SWAP_CHAIN_DESC1 desc = {0};
   desc.Width = 64;
   desc.Height = 64;
   desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
   desc.SampleDesc.Count = 1;
   desc.BufferCount = 2;
   npt_gsc_init(&t->owner.base, t->swapchain, &desc, NULL, NULL, "test11");
   test_fixture_bind(t->fixture, &t->owner.base, t->swapchain);
   t->swapchain->aux = &t->owner;
   t->owner.dc = t->context;
   t->owner.dc4 = t->context4;
   t->owner.fence = t->fence;
   t->owner.backbuffer = t->swapchain;
   t->owner.base.gpu_poll = gsc_gpu_poll;
   t->owner.base.gpu_poll_ctx = &t->owner;
   for (unsigned i = 0; i < NPT_SC_PRESENT_IMAGES; ++i)
      t->owner.slot_query[i] = (ID3D11Query *)t->queries[i];
   t->owner.drain_query = (ID3D11Query *)t->queries[NPT_SC_PRESENT_IMAGES];
   t->query_result = S_FALSE;
}

static void
d3d11_fini(struct d3d11_test *t)
{
   npt_gsc_fini(&t->owner.base);
   test_fixture_destroy(t->fixture);
}

static HRESULT
drain_nonblocking(struct d3d11_test *t)
{
   test_enter_nonblocking(t->fixture);
   HRESULT hr = gsc_gpu_drain_context(&t->owner, true);
   test_leave_nonblocking(t->fixture);
   return hr;
}

static void
test_fence_poll_and_drain(void)
{
   test_begin("d3d11-fence-poll-drain");
   struct d3d11_test t;
   d3d11_init(&t);
   CHECK(gsc_gpu_poll(&t.owner, 0, 1) == S_FALSE);
   t.completed = 1;
   CHECK(gsc_gpu_poll(&t.owner, 0, 1) == S_OK);
   t.completed = UINT64_MAX;
   CHECK(gsc_gpu_poll(&t.owner, 0, 1) == NPT_DXGI_ERROR_DEVICE_REMOVED);

   t.completed = 0;
   for (unsigned i = 0; i < 2; ++i) {
      CHECK(drain_nonblocking(&t) == NPT_DXGI_ERROR_WAS_STILL_DRAWING);
      CHECK(t.signals == 1 && t.last_signal == 1 && t.flushes == 1);
      CHECK(t.owner.drain_pending && t.owner.drain_value == 1);
   }
   t.completed = 1;
   CHECK(drain_nonblocking(&t) == S_OK);
   CHECK(!t.owner.drain_pending && t.signals == 1 && t.flushes == 1);
   d3d11_fini(&t);
   test_end();
}

static void
test_query_poll_and_drain(void)
{
   test_begin("d3d11-query-poll-drain");
   struct d3d11_test t;
   d3d11_init(&t);
   t.owner.fence = NULL;
   t.owner.dc4 = NULL;
   for (unsigned i = 0; i < NPT_SC_PRESENT_IMAGES; ++i) {
      CHECK(gsc_gpu_poll(&t.owner, i, 0) == S_FALSE);
      CHECK((void *)t.last_query == t.queries[i]);
   }
   t.query_result = S_OK;
   t.query_done = FALSE;
   CHECK(gsc_gpu_poll(&t.owner, 0, 0) == S_FALSE);
   t.query_done = TRUE;
   CHECK(gsc_gpu_poll(&t.owner, 0, 0) == S_OK);
   t.query_result = E_FAIL;
   CHECK(gsc_gpu_poll(&t.owner, 0, 0) == E_FAIL);

   t.query_result = S_FALSE;
   t.query_done = FALSE;
   for (unsigned i = 0; i < 2; ++i) {
      CHECK(drain_nonblocking(&t) == NPT_DXGI_ERROR_WAS_STILL_DRAWING);
      CHECK(t.ends == 1 && t.flushes == 1 && !t.signals);
      CHECK(t.owner.drain_pending && t.owner.drain_value == 1);
      CHECK((void *)t.last_query == t.queries[NPT_SC_PRESENT_IMAGES]);
   }
   t.query_result = S_OK;
   t.query_done = TRUE;
   CHECK(drain_nonblocking(&t) == S_OK);
   CHECK(!t.owner.drain_pending && t.ends == 1 && t.flushes == 1);
   d3d11_fini(&t);
   test_end();
}

void
test_d3d11_group(void)
{
   test_fence_poll_and_drain();
   test_query_poll_and_drain();
}
