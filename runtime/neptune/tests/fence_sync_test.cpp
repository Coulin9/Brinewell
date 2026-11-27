#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>

static FILE *output;
static unsigned failures;

static void
expect(bool condition, const char *message)
{
   if (!condition) {
      ++failures;
      std::fprintf(output, "FAIL: %s\n", message);
   }
}

static void
require(bool condition, const char *message)
{
   if (!condition) {
      std::fprintf(output, "FATAL: %s\n", message);
      std::exit(2);
   }
}

static void
check(HRESULT result, const char *message)
{
   if (FAILED(result)) {
      std::fprintf(output, "FATAL: %s HRESULT=%08lx\n", message,
                   (unsigned long)result);
      std::exit(2);
   }
}

template <typename T> struct Com {
   T *ptr = nullptr;
   ~Com() { if (ptr) ptr->Release(); }
   T *operator->() const { return ptr; }
   void **address() { return reinterpret_cast<void **>(&ptr); }
};

struct Event {
   HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
   Event() { require(handle != nullptr, "CreateEvent"); }
   ~Event() { CloseHandle(handle); }
};

static void
create_device12(Com<ID3D12Device> &device)
{
   check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                          __uuidof(ID3D12Device), device.address()),
         "D3D12CreateDevice");
}

static void
create_fence12(ID3D12Device *device, Com<ID3D12Fence> &fence)
{
   check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                             __uuidof(ID3D12Fence), fence.address()),
         "CreateFence");
}

static void
test_create_fence12(ID3D12Device *device)
{
   const GUID unsupported_iid = {0x8f614c48, 0xe14a, 0x4c54,
                                {0xa2, 0x97, 0x6b, 0x5c, 0x71, 0xe7, 0x8a, 0xf3}};
   void *result = device;
   HRESULT hr = device->CreateFence(17, D3D12_FENCE_FLAG_NONE,
                                    unsupported_iid, &result);
   std::fprintf(output, "D3D12 CreateFence unsupported IID HRESULT=%08lx\n",
                (unsigned long)hr);
   expect(FAILED(hr), "CreateFence must return the unsupported IID error");
   expect(result == nullptr, "failed CreateFence must clear the output pointer");
   if (SUCCEEDED(hr) && result && result != device)
      static_cast<IUnknown *>(result)->Release();

   Com<ID3D12Fence> fence;
   check(device->CreateFence(17, D3D12_FENCE_FLAG_NONE,
                             __uuidof(ID3D12Fence), fence.address()),
         "valid CreateFence after an unsupported IID");
   require(fence.ptr != nullptr, "successful CreateFence output");
   expect(fence->GetCompletedValue() == 17, "CreateFence initial value");
}

static void
create_queue12(ID3D12Device *device, Com<ID3D12CommandQueue> &queue,
               D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT)
{
   D3D12_COMMAND_QUEUE_DESC desc = {};
   desc.Type = type;
   check(device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                    queue.address()), "CreateCommandQueue");
}

static void
test_shared_fence12(ID3D12Device *device)
{
   Com<ID3D12Fence1> shared;
   check(device->CreateFence(7, D3D12_FENCE_FLAG_SHARED,
                             __uuidof(ID3D12Fence1), shared.address()),
         "CreateFence with D3D12_FENCE_FLAG_SHARED");
   require(shared.ptr != nullptr, "shared CreateFence output");
   expect(shared->GetCreationFlags() == D3D12_FENCE_FLAG_SHARED,
          "shared fence creation flags");
   expect(shared->GetCompletedValue() == 7, "shared fence initial value");

   Com<ID3D12Fence> done;
   create_fence12(device, done);
   Com<ID3D12CommandQueue> producer, consumer;
   create_queue12(device, producer, D3D12_COMMAND_LIST_TYPE_COMPUTE);
   create_queue12(device, consumer);
   Event event;
   check(consumer->Wait(shared.ptr, 8), "DIRECT queue wait for shared fence");
   check(consumer->Signal(done.ptr, 1), "DIRECT queue completion marker");
   check(done->SetEventOnCompletion(1, event.handle), "arm completion marker");
   DWORD early = WaitForSingleObject(event.handle, 50);
   expect(early == WAIT_TIMEOUT, "shared fence wait must hold the DIRECT queue");
   check(producer->Signal(shared.ptr, 8), "COMPUTE queue signal shared fence");
   if (early == WAIT_TIMEOUT) {
      require(WaitForSingleObject(event.handle, 2000) == WAIT_OBJECT_0,
              "shared fence must release the waiting DIRECT queue");
   }
   expect(shared->GetCompletedValue() == 8, "shared fence completed value");
}

struct CpuSignalThread {
   ID3D12Fence *fence;
   HANDLE ready;
   HANDLE ack;
   unsigned iterations;
   HRESULT result = S_OK;
};

static DWORD WINAPI
cpu_signal_thread(void *data)
{
   auto *state = static_cast<CpuSignalThread *>(data);
   for (UINT64 value = 1; value <= state->iterations; ++value) {
      state->result = state->fence->Signal(value);
      SetEvent(state->ready);
      if (WaitForSingleObject(state->ack, 2000) != WAIT_OBJECT_0)
         return 1;
      if (FAILED(state->result))
         return 2;
   }
   return 0;
}

static void
test_cpu_signal(ID3D12Device *device)
{
   Com<ID3D12Fence> fence;
   create_fence12(device, fence);
   unsigned stale = 0;
   for (UINT64 value = 1; value <= 1000; ++value) {
      check(fence->Signal(value), "CPU Signal");
      if (fence->GetCompletedValue() != value)
         ++stale;
   }
   std::fprintf(output, "D3D12 CPU Signal stale=%u/1000\n", stale);
   expect(stale == 0, "CPU Signal must be immediately visible");

   Com<ID3D12Fence1> tier1;
   HRESULT qi = fence->QueryInterface(__uuidof(ID3D12Fence1), tier1.address());
   if (SUCCEEDED(qi)) {
      check(tier1->Signal(1001), "Fence1 CPU Signal");
      expect(fence->GetCompletedValue() == 1001,
             "Fence1 Signal must update the base interface");
      expect(tier1->GetCompletedValue() == 1001,
             "Fence1 query must observe its Signal");
   } else {
      require(qi == E_NOINTERFACE, "Fence1 QueryInterface");
      std::fprintf(output, "SKIP Fence1: E_NOINTERFACE\n");
   }

   Com<ID3D12Fence> threaded;
   create_fence12(device, threaded);
   Event ready, ack;
   CpuSignalThread state = {threaded.ptr, ready.handle, ack.handle, 128};
   HANDLE thread = CreateThread(nullptr, 0, cpu_signal_thread, &state, 0, nullptr);
   require(thread != nullptr, "CreateThread CPU Signal");
   stale = 0;
   for (UINT64 value = 1; value <= state.iterations; ++value) {
      require(WaitForSingleObject(ready.handle, 2000) == WAIT_OBJECT_0,
              "CPU Signal thread ready");
      check(state.result, "threaded CPU Signal");
      if (threaded->GetCompletedValue() != value)
         ++stale;
      SetEvent(ack.handle);
   }
   require(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0,
           "CPU Signal thread completion");
   DWORD exit_code;
   require(GetExitCodeThread(thread, &exit_code) && exit_code == 0,
           "CPU Signal thread exit");
   CloseHandle(thread);
   std::fprintf(output, "D3D12 cross-thread CPU Signal stale=%u/128\n", stale);
   expect(stale == 0, "another thread must observe a completed CPU Signal");
}

static void
test_persistent_readback12(ID3D12Device *device)
{
   Com<ID3D12Heap> upload_heap, readback_heap;
   Com<ID3D12Resource> upload, readback;
   D3D12_HEAP_DESC heap = {};
   heap.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
   heap.SizeInBytes = heap.Alignment;
   heap.Properties.CreationNodeMask = heap.Properties.VisibleNodeMask = 1;
   D3D12_RESOURCE_DESC buffer = {};
   buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   buffer.Width = sizeof(UINT);
   buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
   buffer.SampleDesc.Count = 1;
   buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   const UINT64 offset = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

   heap.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
   check(device->CreateHeap(&heap, IID_PPV_ARGS(&upload_heap.ptr)), "upload heap");
   check(device->CreatePlacedResource(upload_heap.ptr, offset, &buffer,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&upload.ptr)), "placed upload");
   heap.Properties.Type = D3D12_HEAP_TYPE_READBACK;
   check(device->CreateHeap(&heap, IID_PPV_ARGS(&readback_heap.ptr)), "readback heap");
   expect(readback_heap->GetDesc().Alignment == heap.Alignment, "heap alignment");
   check(device->CreatePlacedResource(readback_heap.ptr, offset, &buffer,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&readback.ptr)), "placed readback");

   Com<ID3D12CommandAllocator> allocator;
   Com<ID3D12GraphicsCommandList> list;
   Com<ID3D12CommandQueue> queue;
   Com<ID3D12Fence> fence;
   create_queue12(device, queue);
   create_fence12(device, fence);
   check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator.ptr)), "allocator");
   check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.ptr,
                                   nullptr, IID_PPV_ARGS(&list.ptr)), "copy list");
   list->CopyBufferRegion(readback.ptr, 0, upload.ptr, 0, buffer.Width);
   check(list->Close(), "close copy list");

   D3D12_RANGE none = {}, all = {0, sizeof(UINT)};
   void *src = nullptr, *dst = nullptr;
   check(upload->Map(0, &none, &src), "persistent upload Map");
   check(readback->Map(0, &all, &dst), "persistent readback Map");
   Event done;
   ID3D12CommandList *lists[] = {list.ptr};
   for (UINT value = 1; value <= 2; ++value) {
      *static_cast<UINT *>(src) = value;
      queue->ExecuteCommandLists(1, lists);
      check(queue->Signal(fence.ptr, value), "signal copy completion");
      check(fence->SetEventOnCompletion(value, done.handle), "watch copy completion");
      require(WaitForSingleObject(done.handle, 5000) == WAIT_OBJECT_0, "wait for copy");
      expect(*static_cast<volatile UINT *>(dst) == value,
             "mapped readback must reflect each completed GPU copy");
   }
   readback->Unmap(0, &none);
   upload->Unmap(0, &all);
}

static void
test_event12(ID3D12Device *device)
{
   Com<ID3D12CommandQueue> queue;
   Com<ID3D12Fence> fence, gate;
   create_queue12(device, queue);
   create_fence12(device, fence);
   create_fence12(device, gate);
   Event event;
   unsigned stale = 0;
   for (UINT64 value = 1; value <= 1000; ++value) {
      check(queue->Wait(gate.ptr, value), "queue Wait");
      check(queue->Signal(fence.ptr, value), "queue Signal");
      check(fence->SetEventOnCompletion(value, event.handle), "event arm");
      check(gate->Signal(value), "release queue gate");
      DWORD result = WaitForSingleObject(event.handle, 2000);
      UINT64 completed = fence->GetCompletedValue();
      require(result == WAIT_OBJECT_0, "D3D12 event completion");
      if (completed < value)
         ++stale;
   }
   std::fprintf(output, "D3D12 event stale=%u/1000\n", stale);
   expect(stale == 0, "D3D12 query must observe the completed event value");
}

struct BlockingWait {
   ID3D12Fence *fence12 = nullptr;
   ID3D11Fence *fence11 = nullptr;
   UINT64 value;
   HANDLE started;
   HRESULT result = E_FAIL;
};

static DWORD WINAPI
blocking_wait_thread(void *data)
{
   auto *state = static_cast<BlockingWait *>(data);
   SetEvent(state->started);
   state->result = state->fence12
      ? state->fence12->SetEventOnCompletion(state->value, nullptr)
      : state->fence11->SetEventOnCompletion(state->value, nullptr);
   return 0;
}

static bool
wait_value(ID3D12Fence *fence, UINT64 value)
{
   ULONGLONG start = GetTickCount64();
   do {
      if (fence->GetCompletedValue() == value)
         return true;
      Sleep(1);
   } while (GetTickCount64() - start < 1000);
   return false;
}

static void
test_rewind(ID3D12Device *device)
{
   Com<ID3D12CommandQueue> slow, fast;
   Com<ID3D12Fence> fence, gate, drain;
   create_queue12(device, slow);
   create_queue12(device, fast);
   create_fence12(device, fence);
   create_fence12(device, gate);
   create_fence12(device, drain);
   Event drained, new_wait, final_event, started;

   check(slow->Wait(gate.ptr, 1), "hold slow queue");
   check(slow->Signal(fence.ptr, 1), "request value 1");
   check(slow->Signal(drain.ptr, 1), "slow queue drain marker");
   check(fast->Signal(fence.ptr, 2), "request value 2");
   require(wait_value(fence.ptr, 2), "observe value 2 before rewind");
   check(drain->SetEventOnCompletion(1, drained.handle), "arm drain marker");
   check(gate->Signal(1), "release slow queue");
   require(WaitForSingleObject(drained.handle, 2000) == WAIT_OBJECT_0,
           "confirm value 1 executed after value 2");
   Sleep(30);
   UINT64 rewound = fence->GetCompletedValue();
   expect(rewound == 1, "query must observe a completed cross-queue rewind");

   check(fence->SetEventOnCompletion(2, new_wait.handle), "new wait after rewind");
   DWORD early = WaitForSingleObject(new_wait.handle, 0);
   expect(early == WAIT_TIMEOUT, "a previous high value must not satisfy a new wait");

   BlockingWait blocking = {fence.ptr, nullptr, 2, started.handle};
   HANDLE thread = CreateThread(nullptr, 0, blocking_wait_thread, &blocking, 0, nullptr);
   require(thread != nullptr, "CreateThread blocking wait");
   require(WaitForSingleObject(started.handle, 2000) == WAIT_OBJECT_0,
           "blocking wait started");
   DWORD blocked = WaitForSingleObject(thread, 50);
   expect(blocked == WAIT_TIMEOUT, "NULL-event wait must block after rewind");

   check(fast->Wait(gate.ptr, 2), "hold value 3");
   check(fast->Signal(fence.ptr, 3), "request value 3");
   Sleep(30);
   expect(fence->GetCompletedValue() == 1, "pending value 3 must not hide rewind");
   check(fence->SetEventOnCompletion(3, final_event.handle), "arm value 3");
   check(gate->Signal(2), "release value 3");
   require(WaitForSingleObject(final_event.handle, 2000) == WAIT_OBJECT_0,
           "value 3 completion");
   UINT64 completed = fence->GetCompletedValue();
   expect(completed == 3, "query must keep advancing after a rewind");
   require(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0,
           "NULL-event wait must complete without blocking the dispatch ring");
   check(blocking.result, "NULL-event wait result");
   CloseHandle(thread);
   if (early == WAIT_TIMEOUT)
      expect(WaitForSingleObject(new_wait.handle, 2000) == WAIT_OBJECT_0,
             "new wait must complete when the value is reached again");
   std::fprintf(output, "D3D12 rewind value=%llu early_wait=%lu blocking=%lu final=%llu\n",
                (unsigned long long)rewound, (unsigned long)early,
                (unsigned long)blocked, (unsigned long long)completed);
}

static void
test_d3d11(void)
{
   Com<ID3D11Device> base;
   Com<ID3D11DeviceContext> context;
   D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
   check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                          &level, 1, D3D11_SDK_VERSION, &base.ptr, nullptr,
                          &context.ptr), "D3D11CreateDevice");
   Com<ID3D11Device5> device;
   Com<ID3D11DeviceContext4> ctx;
   Com<ID3D11Fence> fence;
   check(base->QueryInterface(__uuidof(ID3D11Device5), device.address()),
         "D3D11Device5");
   check(context->QueryInterface(__uuidof(ID3D11DeviceContext4), ctx.address()),
         "D3D11DeviceContext4");
   check(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                             fence.address()), "D3D11 CreateFence");
   Event event;
   unsigned stale = 0;
   for (UINT64 value = 1; value <= 5000; ++value) {
      check(ctx->Signal(fence.ptr, value), "D3D11 Signal");
      context->Flush();
      check(fence->SetEventOnCompletion(value, event.handle), "D3D11 event arm");
      DWORD result = WaitForSingleObject(event.handle, 2000);
      UINT64 completed = fence->GetCompletedValue();
      require(result == WAIT_OBJECT_0, "D3D11 event completion");
      if (completed < value)
         ++stale;
   }
   std::fprintf(output, "D3D11 event stale=%u/5000\n", stale);
   expect(stale == 0, "D3D11 query must observe the completed event value");

   Event started;
   BlockingWait blocking = {nullptr, fence.ptr, 5001, started.handle};
   HANDLE thread = CreateThread(nullptr, 0, blocking_wait_thread, &blocking, 0, nullptr);
   require(thread != nullptr, "CreateThread D3D11 blocking wait");
   require(WaitForSingleObject(started.handle, 2000) == WAIT_OBJECT_0,
           "D3D11 blocking wait started");
   expect(WaitForSingleObject(thread, 50) == WAIT_TIMEOUT,
          "D3D11 NULL-event wait must block until Signal");
   check(ctx->Signal(fence.ptr, 5001), "D3D11 release blocking wait");
   context->Flush();
   require(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0,
           "D3D11 NULL-event wait completion");
   check(blocking.result, "D3D11 NULL-event wait result");
   CloseHandle(thread);
}

int
main(int argc, char **argv)
{
   output = argc > 1 ? std::fopen(argv[1], "w") : stdout;
   if (!output)
      return 2;
   std::setvbuf(output, nullptr, _IONBF, 0);
   {
      Com<ID3D12Device> device;
      create_device12(device);
      test_create_fence12(device.ptr);
      test_shared_fence12(device.ptr);
      test_cpu_signal(device.ptr);
      test_event12(device.ptr);
      test_persistent_readback12(device.ptr);
      test_rewind(device.ptr);
   }
   test_d3d11();
   std::fprintf(output, "FENCE SYNC: %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
   if (output != stdout)
      std::fclose(output);
   return failures ? 1 : 0;
}
