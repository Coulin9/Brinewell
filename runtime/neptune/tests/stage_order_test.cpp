/* Staged D3D11 device-context commands: cross-thread ordering on a
 * shared context.  (a) Two threads alternate batches of
 * ClearRenderTargetView on the same immediate context; the host must
 * decode them in call order, so a readback sees the last writer's
 * colour.  (b) A deferred context recorded on a worker thread must
 * land behind that thread's FinishCommandList when the main thread
 * executes the list.  (c) An EVENT query's GetData spin must not
 * deadlock while earlier commands still sit in a staging buffer.
 * Links against the built d3d11.dll like npt-fence-sync-test. */

#include <windows.h>
#include <d3d11_4.h>
#include <cstdint>
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
   T **address() { return &ptr; }
   operator bool() const { return ptr != nullptr; }
};

struct Event {
   HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
   Event() { require(handle != nullptr, "CreateEvent"); }
   ~Event() { CloseHandle(handle); }
   void set() { SetEvent(handle); }
   void wait() { WaitForSingleObject(handle, INFINITE); }
   void reset() { ResetEvent(handle); }
};

static const float RED[4]   = {1.0f, 0.0f, 0.0f, 1.0f};
static const float GREEN[4] = {0.0f, 1.0f, 0.0f, 1.0f};
static const float BLUE[4]  = {0.0f, 0.0f, 1.0f, 1.0f};

static uint32_t
rgba8(const float c[4])
{
   return (uint32_t)(c[0] * 255.0f + 0.5f) |
          ((uint32_t)(c[1] * 255.0f + 0.5f) << 8) |
          ((uint32_t)(c[2] * 255.0f + 0.5f) << 16) |
          ((uint32_t)(c[3] * 255.0f + 0.5f) << 24);
}

static Com<ID3D11Texture2D>
make_rt(ID3D11Device *device)
{
   D3D11_TEXTURE2D_DESC d = {};
   d.Width = 8;
   d.Height = 8;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_RENDER_TARGET;
   Com<ID3D11Texture2D> t;
   check(device->CreateTexture2D(&d, nullptr, t.address()),
         "CreateTexture2D(rt)");
   return t;
}

static Com<ID3D11Texture2D>
make_staging(ID3D11Device *device)
{
   D3D11_TEXTURE2D_DESC d = {};
   d.Width = 8;
   d.Height = 8;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_STAGING;
   d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   Com<ID3D11Texture2D> t;
   check(device->CreateTexture2D(&d, nullptr, t.address()),
         "CreateTexture2D(staging)");
   return t;
}

/* Copy the RT into the staging texture and read texel (0,0). */
static uint32_t
readback(ID3D11DeviceContext *ctx, ID3D11Texture2D *rt,
         ID3D11Texture2D *staging)
{
   ctx->CopyResource(staging, rt);
   D3D11_MAPPED_SUBRESOURCE m = {};
   check(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m), "Map(readback)");
   const uint32_t px = *(const uint32_t *)m.pData;
   ctx->Unmap(staging, 0);
   return px;
}

/* (a) Worker clearing the RT `count` times with `color`, then
 * signalling.  Only one worker touches the context at a time. */
struct clear_job {
   ID3D11DeviceContext *ctx;
   ID3D11RenderTargetView *rtv;
   const float *color;
   unsigned count;
   Event *go;
   Event *done;
};

static DWORD WINAPI
clear_worker(void *arg)
{
   clear_job *j = static_cast<clear_job *>(arg);
   for (;;) {
      j->go->wait();
      j->go->reset();
      if (!j->color)
         return 0;
      for (unsigned i = 0; i < j->count; i++)
         j->ctx->ClearRenderTargetView(j->rtv, j->color);
      j->done->set();
   }
}

static void
test_immediate_handoff(ID3D11Device *device, ID3D11DeviceContext *ctx,
                       ID3D11RenderTargetView *rtv,
                       ID3D11Texture2D *rt, ID3D11Texture2D *staging)
{
   clear_job a = {ctx, rtv, nullptr, 0, new Event, new Event};
   clear_job b = {ctx, rtv, nullptr, 0, new Event, new Event};
   HANDLE ta = CreateThread(nullptr, 0, clear_worker, &a, 0, nullptr);
   HANDLE tb = CreateThread(nullptr, 0, clear_worker, &b, 0, nullptr);
   require(ta && tb, "CreateThread");

   unsigned mismatches = 0;
   for (unsigned i = 0; i < 200; i++) {
      clear_job *j = (i & 1) ? &b : &a;
      const float *color = (i & 1) ? GREEN : RED;
      j->color = color;
      j->count = 100;
      j->go->set();
      j->done->wait();
      j->done->reset();
      if (readback(ctx, rt, staging) != rgba8(color))
         mismatches++;
   }
   expect(mismatches == 0,
          "immediate-context handoff kept every batch in call order");

   a.color = b.color = nullptr;
   a.go->set();
   b.go->set();
   WaitForSingleObject(ta, INFINITE);
   WaitForSingleObject(tb, INFINITE);
   CloseHandle(ta);
   CloseHandle(tb);
   delete a.go; delete a.done;
   delete b.go; delete b.done;
}

/* (b) Worker records a blue clear on a deferred context and finishes
 * the list; the main thread executes it and must read back blue. */
struct record_job {
   ID3D11Device *device;
   ID3D11RenderTargetView *rtv;
   ID3D11CommandList *list;
   Event *go;
   Event *done;
};

static DWORD WINAPI
record_worker(void *arg)
{
   record_job *j = static_cast<record_job *>(arg);
   j->go->wait();
   Com<ID3D11DeviceContext> def;
   HRESULT hr = j->device->CreateDeferredContext(0, def.address());
   if (FAILED(hr))
      return (DWORD)hr;
   def->ClearRenderTargetView(j->rtv, BLUE);
   hr = def->FinishCommandList(FALSE, &j->list);
   j->done->set();
   return (DWORD)hr;
}

static void
test_deferred_list(ID3D11Device *device, ID3D11DeviceContext *ctx,
                   ID3D11RenderTargetView *rtv,
                   ID3D11Texture2D *rt, ID3D11Texture2D *staging)
{
   ctx->ClearRenderTargetView(rtv, RED);
   record_job j = {device, rtv, nullptr, new Event, new Event};
   HANDLE t = CreateThread(nullptr, 0, record_worker, &j, 0, nullptr);
   require(t, "CreateThread(record)");
   j.go->set();
   j.done->wait();
   WaitForSingleObject(t, INFINITE);
   CloseHandle(t);
   require(j.list != nullptr, "FinishCommandList produced a list");

   ctx->ExecuteCommandList(j.list, FALSE);
   j.list->Release();
   expect(readback(ctx, rt, staging) == rgba8(BLUE),
          "executed deferred list observed the recorded clear");
   delete j.go; delete j.done;
}

/* (c) Commands still staged when an EVENT query's GetData spins must
 * be published by the not-ready path, so the poll completes. */
static void
test_getdata_no_hang(ID3D11DeviceContext *ctx,
                     ID3D11RenderTargetView *rtv,
                     ID3D11Texture2D *rt, ID3D11Texture2D *staging)
{
   Com<ID3D11Device> device;
   ctx->GetDevice(device.address());
   Com<ID3D11Query> query;
   D3D11_QUERY_DESC qd = {};
   qd.Query = D3D11_QUERY_EVENT;
   check(device->CreateQuery(&qd, query.address()), "CreateQuery(EVENT)");

   for (unsigned i = 0; i < 64; i++)
      ctx->ClearRenderTargetView(rtv, GREEN);
   ctx->CopyResource(staging, rt);
   ctx->End(query.ptr);

   BOOL done = FALSE;
   const ULONGLONG deadline = GetTickCount64() + 5000;
   HRESULT hr = S_FALSE;
   while (GetTickCount64() < deadline) {
      hr = ctx->GetData(query.ptr, &done, sizeof(done),
                        D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (hr != S_FALSE)
         break;
      Sleep(1);
   }
   expect(hr == S_OK,
          "GetData(DONOTFLUSH) completed with staged commands pending");
   expect(done == TRUE, "EVENT query reported completion");
}

int
main(int argc, char **argv)
{
   (void)argc; (void)argv;
   output = stdout;

   Com<ID3D11Device> device;
   Com<ID3D11DeviceContext> ctx;
   check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                           nullptr, 0, D3D11_SDK_VERSION, device.address(),
                           nullptr, ctx.address()),
         "D3D11CreateDevice");

   Com<ID3D11Texture2D> rt = make_rt(device.ptr);
   Com<ID3D11Texture2D> staging = make_staging(device.ptr);
   Com<ID3D11RenderTargetView> rtv;
   check(device->CreateRenderTargetView(rt.ptr, nullptr, rtv.address()),
         "CreateRenderTargetView");

   test_immediate_handoff(device.ptr, ctx.ptr, rtv.ptr, rt.ptr,
                          staging.ptr);
   test_deferred_list(device.ptr, ctx.ptr, rtv.ptr, rt.ptr, staging.ptr);
   test_getdata_no_hang(ctx.ptr, rtv.ptr, rt.ptr, staging.ptr);

   if (failures) {
      std::fprintf(output, "%u check(s) failed\n", failures);
      return 1;
   }
   std::fprintf(output, "all checks passed\n");
   return 0;
}
