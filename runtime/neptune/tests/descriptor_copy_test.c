/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static void npt_overrides_d3d12_device_init(void);

#include "../npt_overrides_d3d12_device.c"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define BASE(i) (0x100000ull + (uint64_t)(i) * 0x10000ull)
#define HANDLE_AT(i, slot) ((D3D12_CPU_DESCRIPTOR_HANDLE){ .ptr = BASE(i) + (slot) * 16 })
#define MAX_EVENTS 4096

enum event_kind { ENSURE, MASK, EDGE, SUBMIT, BATCH };
struct event { enum event_kind kind; uint64_t a, b; };
static struct event events[MAX_EVENTS];
static unsigned event_count;
static _Thread_local uint32_t stage_index = 1;
static _Thread_local unsigned zero_indices;
static _Thread_local uint64_t next_ref = 100;
static _Thread_local uint64_t published_ref;
static uint64_t invalidated_heap;
static struct npt_d3d12_desc_heap *observe_heap;
static uint32_t observe_slot;
static uint64_t observe_old;
static uint64_t observe_readers;
static bool observe_enabled;
static const D3D12_CONSTANT_BUFFER_VIEW_DESC *last_cbv_desc;
static D3D12_CPU_DESCRIPTOR_HANDLE last_cbv_destination;
static _Thread_local struct {
   _Atomic uint64_t *slot;
   uint64_t old;
} pending_slots[4096];
static _Thread_local unsigned pending_count;
static atomic_uint tail;
static struct npt_ring ring;
static struct npt_com_base object;

static void
record(enum event_kind kind, uint64_t a, uint64_t b)
{
   CHECK(event_count < MAX_EVENTS);
   events[event_count++] = (struct event){kind, a, b};
}

uint32_t npt_ring_stage_self_index(struct npt_ring *r)
{
   (void)r;
   if (zero_indices) { --zero_indices; return 0; }
   return stage_index;
}
npt_ring_stage_ref npt_ring_stage_last_ref(struct npt_ring *r)
{ (void)r; return published_ref; }
static bool batch_ok;
static _Thread_local unsigned batch_attempts;
static uint32_t last_batch_kind, last_batch_heap;
static uint64_t last_batch_dev;
static struct npt_desc_copy_entry last_copy_e;
static struct npt_desc_cbv_entry last_cbv_e;
bool npt_ring_stage_append_desc(struct npt_ring *r, uint32_t kind,
   uint64_t dev, uint32_t heap, uint32_t inc, const void *e, uint32_t es)
{
   (void)inc; (void)es;
   ++batch_attempts;
   if (!r->staging || !batch_ok)
      return false;
   record(BATCH, kind, 0);
   last_batch_kind = kind; last_batch_heap = heap; last_batch_dev = dev;
   if (kind == NPT_DESC_BATCH_COPY_SIMPLE)
      memcpy(&last_copy_e, e, sizeof(last_copy_e));
   else
      memcpy(&last_cbv_e, e, sizeof(last_cbv_e));
   /* What a real append does: this becomes the stage's last ref. */
   published_ref = next_ref;
   return true;
}
void npt_ring_stage_ensure_flushed(struct npt_ring *r, npt_ring_stage_ref ref)
{ (void)r; record(ENSURE, ref, 0); }
void npt_ring_stage_flush_mask(struct npt_ring *r, uint64_t mask)
{ (void)r; record(MASK, mask, 0); }
void npt_ring_order_after(struct npt_ring *r, uint64_t id, uint32_t seqno)
{ (void)r; record(EDGE, id, seqno); }
void npt_ring_stage_flush_index(struct npt_ring *r, uint32_t index)
{ (void)r; (void)index; }
struct npt_ring *npt_tls_get_ring(struct npt_device *dev)
{ (void)dev; return NULL; }
struct npt_ring *npt_device_get_pipeline_library_ring(struct npt_device *dev)
{ (void)dev; CHECK(false); return NULL; }
bool npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t id, uint32_t *out)
{ (void)dev; (void)id; (void)out; return false; }

static void
submit(void)
{
   for (unsigned i = 0; i < pending_count; ++i)
      CHECK(atomic_load(pending_slots[i].slot) == pending_slots[i].old);
   if (observe_enabled) {
      CHECK(atomic_load(&observe_heap->slots[observe_slot]) == observe_old);
      CHECK(atomic_load(&observe_heap->readers[observe_slot]) == observe_readers);
   }
   record(SUBMIT, 0, 0);
   if (invalidated_heap) {
      desc_heaps_unregister(invalidated_heap);
      invalidated_heap = 0;
   }
   atomic_fetch_add(&tail, 1);
   published_ref = next_ref;
}

void NPT_STDMETHODCALLTYPE
npt_id3d12device_default_CreateConstantBufferView(void *self,
   const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
   CHECK(self == &object);
   last_cbv_desc = desc;
   last_cbv_destination = dst;
   submit();
}
void NPT_STDMETHODCALLTYPE
npt_id3d12device_default_CopyDescriptors(void *self, UINT nd,
   const D3D12_CPU_DESCRIPTOR_HANDLE *dst, const UINT *ds, UINT ns,
   const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *ss,
   D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   (void)self; (void)nd; (void)dst; (void)ds; (void)ns; (void)src;
   (void)ss; (void)type;
   submit();
}
void NPT_STDMETHODCALLTYPE
npt_id3d12device_default_CopyDescriptorsSimple(void *self, UINT n,
   D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
   D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   (void)self; (void)n; (void)dst; (void)src; (void)type;
   submit();
}

static struct npt_d3d12_desc_heap *
heap(unsigned i)
{
   struct npt_d3d12_desc_heap *h = desc_heaps_lookup(BASE(i));
   CHECK(h != NULL);
   return h;
}
static uint64_t slot(unsigned h, unsigned i) { return atomic_load(&heap(h)->slots[i]); }
static uint64_t readers(unsigned h, unsigned i) { return atomic_load(&heap(h)->readers[i]); }
static void set_slot(unsigned h, unsigned i, uint64_t v) { atomic_store(&heap(h)->slots[i], v); }
static void set_readers(unsigned h, unsigned i, uint64_t v) { atomic_store(&heap(h)->readers[i], v); }
static void reset_events(void) { event_count = 0; observe_enabled = false; }
static void begin(const char *name) { printf("CASE %s\n", name); fflush(stdout); reset_events(); }
static void
remember_destination(uint64_t handle, unsigned count)
{
   struct desc_slot_range range = desc_tracked_slots(handle, count);
   if (!range.heap)
      return;
   for (unsigned i = 0; i < range.count; ++i) {
      CHECK(pending_count < 4096);
      _Atomic uint64_t *p = &range.heap->slots[range.first + i];
      pending_slots[pending_count].slot = p;
      pending_slots[pending_count++].old = atomic_load(p);
   }
}
static void simple(unsigned dst, unsigned src, unsigned n)
{
   pending_count = 0;
   remember_destination(BASE(dst), n);
   dev12_CopyDescriptorsSimple_override(&object, n, HANDLE_AT(dst, 0),
      HANDLE_AT(src, 0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}
static void cbv(D3D12_CPU_DESCRIPTOR_HANDLE dst,
                const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc)
{
   pending_count = 0;
   remember_destination(dst.ptr, 1);
   id3d12device_CreateConstantBufferView_override(&object, desc, dst);
   CHECK(last_cbv_desc == desc && last_cbv_destination.ptr == dst.ptr);
}
static void copy(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE *dst, const UINT *ds,
                 UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *ss)
{
   pending_count = 0;
   for (UINT i = 0; dst && i < nd; ++i)
      remember_destination(dst[i].ptr, ds ? ds[i] : 1);
   dev12_CopyDescriptors_override(&object, nd, dst, ds, ns, src, ss,
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}
static unsigned count_events(enum event_kind k, uint64_t a)
{
   unsigned n = 0;
   for (unsigned i = 0; i < event_count; ++i)
      if (events[i].kind == k && events[i].a == a) ++n;
   return n;
}
static void observe(unsigned h, unsigned index, uint64_t old, uint64_t mask)
{
   observe_heap = heap(h); observe_slot = index; observe_old = old;
   observe_readers = mask; observe_enabled = true;
}

static void test_simple(void)
{
   begin("simple-order");
   ring.staging = true; stage_index = 1; next_ref = 101;
   set_slot(0, 0, 11); set_slot(1, 0, 22); set_readers(1, 0, 3);
   observe(1, 0, 22, 0);
   simple(1, 0, 1);
   CHECK(event_count == 4);
   CHECK(events[0].kind == ENSURE && events[0].a == 11);
   CHECK(events[1].kind == ENSURE && events[1].a == 22);
   CHECK(events[2].kind == MASK && events[2].a == 2);
   CHECK(events[3].kind == SUBMIT);
   CHECK(slot(1, 0) == 101 && readers(0, 0) == 1 && readers(1, 0) == 0);
}

static void test_cbv_writer(void)
{
   const D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {
      .BufferLocation = 0x400000, .SizeInBytes = 256,
   };
   begin("cbv-write-order");
   ring.staging = true; stage_index = 1; next_ref = 701;
   set_slot(1, 0, 55); set_readers(1, 0, 3);
   set_slot(1, 1, 56); set_readers(1, 1, 4);
   observe(1, 0, 55, 0);
   cbv(HANDLE_AT(1, 0), &desc);
   CHECK(event_count == 3);
   CHECK(events[0].kind == ENSURE && events[0].a == 55);
   CHECK(events[1].kind == MASK && events[1].a == 2);
   CHECK(events[2].kind == SUBMIT);
   CHECK(slot(1, 0) == 701 && readers(1, 0) == 0);
   CHECK(slot(1, 1) == 56 && readers(1, 1) == 4);

   begin("cbv-shader-visible-null-desc");
   cbv(HANDLE_AT(9, 0), NULL);
   CHECK(event_count == 1 && events[0].kind == SUBMIT);

   begin("cbv-nonstaging");
   ring.staging = false; ring.id = 7; atomic_store(&tail, 40);
   set_slot(1, 0, 57); set_readers(1, 0, 8);
   observe(1, 0, 57, 8);
   cbv(HANDLE_AT(1, 0), &desc);
   CHECK(event_count == 1 && events[0].kind == SUBMIT);
   CHECK(slot(1, 0) == DESC_LAST_PACK(7, 41) && readers(1, 0) == 8);

   begin("cbv-unrelated-invalidation-during-submit");
   ring.staging = true; next_ref = 702;
   set_slot(1, 0, 58); set_readers(1, 0, 3);
   observe(1, 0, 58, 0);
   invalidated_heap = BASE(10);
   cbv(HANDLE_AT(1, 0), &desc);
   CHECK(event_count == 3 && events[2].kind == SUBMIT);
   CHECK(slot(1, 0) == 702 && readers(1, 0) == 0);
   desc_heaps_register(BASE(10), 16, 512, true);

   begin("cbv-reresolve-after-heap-replacement");
   next_ref = 703;
   set_slot(1, 2, 60); set_readers(1, 2, 3);
   observe(1, 2, 60, 0);
   cbv(HANDLE_AT(1, 2), &desc);
   CHECK(slot(1, 2) == 703);
   desc_heaps_unregister(BASE(1));
   desc_heaps_register(BASE(1), 32, 8, true);
   reset_events(); next_ref = 704;
   set_slot(1, 1, 59); set_readers(1, 1, 3);
   observe(1, 1, 59, 0);
   cbv(HANDLE_AT(1, 2), &desc);
   CHECK(event_count == 3 && events[0].kind == ENSURE && events[0].a == 59);
   CHECK(events[1].kind == MASK && events[1].a == 2);
   CHECK(events[2].kind == SUBMIT);
   CHECK(slot(1, 1) == 704 && readers(1, 1) == 0 && slot(1, 2) == 0);
   desc_heaps_unregister(BASE(1));
   desc_heaps_register(BASE(1), 16, 512, true);
}

static void test_ranges(unsigned n)
{
   D3D12_CPU_DESCRIPTOR_HANDLE dst[257], src[257];
   UINT sizes[257];
   begin(n == 2 ? "ranges-2" : n == 16 ? "ranges-16" : n == 17 ? "ranges-17" : "ranges-257");
   ring.staging = true; stage_index = 2; next_ref = 200 + n;
   for (unsigned i = 0; i < n; ++i) {
      dst[i] = HANDLE_AT(2 + i % 5, i / 5);
      src[i] = HANDLE_AT(8, i);
      sizes[i] = 1;
      set_slot(2 + i % 5, i / 5, 50 + i);
      set_readers(2 + i % 5, i / 5, 4);
      set_slot(8, i, 1000 + i);
      set_readers(8, i, 0);
   }
   observe(2, 0, 50, 0);
   copy(n, dst, sizes, n, src, sizes);
   CHECK(events[event_count - 1].kind == SUBMIT);
   CHECK(count_events(SUBMIT, 0) == 1);
   for (unsigned i = 0; i < n; ++i) {
      CHECK(slot(2 + i % 5, i / 5) == next_ref);
      CHECK(readers(2 + i % 5, i / 5) == 0);
      CHECK(readers(8, i) == 2);
      CHECK(events[i].kind == ENSURE && events[i].a == 1000 + i);
      CHECK(events[n + 2 * i].kind == ENSURE &&
            events[n + 2 * i].a == 50 + i);
      CHECK(events[n + 2 * i + 1].kind == MASK &&
            events[n + 2 * i + 1].a == 4);
   }
}

static void test_null_and_boundaries(void)
{
   D3D12_CPU_DESCRIPTOR_HANDLE dst[2] = {HANDLE_AT(1, 2), HANDLE_AT(1, 3)};
   D3D12_CPU_DESCRIPTOR_HANDLE src[2] = {HANDLE_AT(0, 2), HANDLE_AT(0, 3)};
   UINT sizes[2] = {1, 1};
   begin("null-sizes"); ring.staging = true; stage_index = 1;
   for (unsigned i = 0; i < 3; ++i) {
      next_ref = 300 + i;
      set_readers(0, 2, 0); set_readers(0, 3, 0);
      copy(2, dst, i == 0 || i == 2 ? NULL : sizes,
           2, src, i == 1 || i == 2 ? NULL : sizes);
      CHECK(slot(1, 2) == next_ref && slot(1, 3) == next_ref);
      CHECK(readers(0, 2) == 1 && readers(0, 3) == 1);
   }
   begin("empty-untracked-clipping");
   struct desc_slot_range clipped = desc_tracked_slots(BASE(0) + 511 * 16, 20);
   CHECK(clipped.heap == heap(0) && clipped.first == 511 && clipped.count == 1);
   CHECK(!desc_tracked_slots(BASE(0), 0).heap);
   CHECK(!desc_tracked_slots(0xdeadbeef, 1).heap);
   CHECK(!desc_tracked_slots(BASE(9), 1).heap);
   UINT zero[4] = {0, 1, 1, 1};
   D3D12_CPU_DESCRIPTOR_HANDLE mixed[4] = {
      HANDLE_AT(1, 4), HANDLE_AT(9, 0), { .ptr = 0xdeadbeef }, HANDLE_AT(1, 5)
   };
   set_slot(1, 4, 444);
   copy(4, mixed, zero, 0, NULL, NULL);
   CHECK(slot(1, 4) == 444 && slot(1, 5) == next_ref);
   set_slot(0, 10, 710); set_readers(0, 10, 0x40);
   set_slot(1, 10, 711); set_readers(1, 10, 0x80);
   copy(0, NULL, NULL, 0, NULL, NULL);
   CHECK(slot(0, 10) == 710 && readers(0, 10) == 0x40);
   CHECK(slot(1, 10) == 711 && readers(1, 10) == 0x80);
   copy(3, NULL, NULL, 3, NULL, NULL);
   CHECK(slot(0, 10) == 710 && readers(0, 10) == 0x40);
   CHECK(slot(1, 10) == 711 && readers(1, 10) == 0x80);
}

static void test_nonstaging(void)
{
   begin("nonstaging-edges"); ring.staging = false; ring.id = 7;
   set_slot(0, 0, DESC_LAST_PACK(9, 12));
   set_slot(0, 1, DESC_LAST_PACK(7, 13));
   set_slot(0, 2, DESC_LAST_PACK(0, 14));
   set_slot(1, 0, 55); atomic_store(&tail, 40);
   observe(1, 0, 55, 0);
   simple(1, 0, 3);
   CHECK(event_count == 2 && events[0].kind == EDGE && events[0].a == 9 && events[0].b == 12);
   CHECK(events[1].kind == SUBMIT);
   for (unsigned i = 0; i < 3; ++i) CHECK(slot(1, i) == DESC_LAST_PACK(7, 41));
}

static void test_reader_edges(void)
{
   begin("reader-self64"); ring.staging = true; stage_index = 64; next_ref = 401;
   set_slot(1, 0, 12); set_readers(1, 0, (1ull << 63) | 4);
   simple(1, 0, 1);
   CHECK(count_events(MASK, 4) == 1 && readers(1, 0) == 0);
   CHECK(readers(0, 0) & (1ull << 63));
   begin("zero-then-success"); stage_index = 3; zero_indices = 1;
   set_readers(0, 0, 0); set_readers(0, 1, 0); set_readers(0, 2, 0);
   D3D12_CPU_DESCRIPTOR_HANDLE src[3] = {HANDLE_AT(0,0), HANDLE_AT(0,1), HANDLE_AT(0,2)};
   copy(0, NULL, NULL, 3, src, NULL);
   CHECK(readers(0, 0) == 0 && readers(0, 1) == 4 && readers(0, 2) == 4);
   begin("zero-post-submit-reference"); next_ref = 0;
   D3D12_CPU_DESCRIPTOR_HANDLE dst[2] = {HANDLE_AT(1,0), HANDLE_AT(1,1)};
   set_slot(1, 0, 123); set_slot(1, 1, 124);
   copy(2, dst, NULL, 0, NULL, NULL);
   CHECK(slot(1, 0) == 0 && slot(1, 1) == 0);
}

static void test_invalidation(void)
{
   begin("heap-generation"); ring.staging = true; stage_index = 1; next_ref = 500;
   simple(1, 0, 1);
   desc_heaps_unregister(BASE(1));
   desc_heaps_register(BASE(1) + 0x100, 16, 8, true);
   CHECK(!desc_tracked_slots(BASE(1), 1).heap);
   CHECK(desc_tracked_slots(BASE(1) + 0x100, 1).heap != NULL);
   D3D12_CPU_DESCRIPTOR_HANDLE dst = { .ptr = BASE(1) + 0x100 };
   copy(1, &dst, NULL, 0, NULL, NULL);
   CHECK(atomic_load(&desc_heaps_lookup(dst.ptr)->slots[0]) == 500);
   desc_heaps_unregister(BASE(1) + 0x100);
   desc_heaps_register(BASE(1), 16, 512, true);
   begin("unrelated-invalidation-during-submit"); next_ref = 501;
   set_slot(1, 0, 77); observe(1, 0, 77, 0);
   invalidated_heap = BASE(10);
   simple(1, 0, 1);
   CHECK(slot(1, 0) == 501);
   desc_heaps_register(BASE(10), 16, 512, true);
}

struct handoff { HANDLE a_done, b_done; };
static DWORD WINAPI thread_b(void *arg)
{
   struct handoff *h = arg;
   CHECK(WaitForSingleObject(h->a_done, INFINITE) == WAIT_OBJECT_0);
   stage_index = 2; next_ref = 602;
   simple(2, 1, 1);
   CHECK(SetEvent(h->b_done));
   return 0;
}
static void test_handoff(void)
{
   begin("thread-handoff"); ring.staging = true; stage_index = 1; next_ref = 601;
   set_slot(1, 0, 0); set_readers(1, 0, 0);
   struct handoff h = {CreateEvent(NULL, FALSE, FALSE, NULL), CreateEvent(NULL, FALSE, FALSE, NULL)};
   CHECK(h.a_done && h.b_done);
   HANDLE thread = CreateThread(NULL, 0, thread_b, &h, 0, NULL);
   CHECK(thread != NULL);
   simple(1, 0, 1);
   unsigned b_start = event_count;
   CHECK(SetEvent(h.a_done));
   CHECK(WaitForSingleObject(h.b_done, INFINITE) == WAIT_OBJECT_0);
   unsigned b_end = event_count;
   CHECK(readers(1, 0) & 2);
   next_ref = 603;
   simple(1, 0, 1);
   unsigned a_end = event_count;
   CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
   bool saw_write_dependency = false, saw_reader_dependency = false;
   for (unsigned i = b_start; i < b_end; ++i)
      if (events[i].kind == ENSURE && events[i].a == 601)
         saw_write_dependency = true;
   for (unsigned i = b_end; i < a_end; ++i)
      if (events[i].kind == MASK && events[i].a == 2)
         saw_reader_dependency = true;
   CHECK(saw_write_dependency && saw_reader_dependency);
   CHECK(events[b_end - 1].kind == SUBMIT && events[a_end - 1].kind == SUBMIT);
   CHECK(slot(1, 0) == 603 && readers(1, 0) == 0);
   CloseHandle(thread); CloseHandle(h.a_done); CloseHandle(h.b_done);
}

static void test_batch(void)
{
   const D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {
      .BufferLocation = 0x400000, .SizeInBytes = 256,
   };
   begin("batch-copy");
   ring.staging = true; stage_index = 1; next_ref = 901; batch_ok = true;
   set_slot(1, 0, 33); set_readers(1, 0, 3);
   simple(1, 0, 4);
   CHECK(count_events(SUBMIT, 0) == 0);
   CHECK(events[event_count - 1].kind == BATCH);
   CHECK(last_batch_kind == NPT_DESC_BATCH_COPY_SIMPLE);
   CHECK(last_batch_heap == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   CHECK(last_batch_dev == npt_com_self_id(&object));
   CHECK(last_copy_e.dst == BASE(1) && last_copy_e.src == BASE(0) &&
         last_copy_e.num == 4);
   CHECK(slot(1, 0) == 901);

   /* A reader of the slot the batch wrote still orders on its ref. */
   next_ref = 904;
   simple(0, 1, 1);
   CHECK(count_events(ENSURE, 901) == 1);

   begin("batch-cbv");
   batch_ok = true; next_ref = 902;
   set_slot(1, 0, 34); set_readers(1, 0, 3);
   id3d12device_CreateConstantBufferView_override(&object, &desc,
                                                  HANDLE_AT(1, 0));
   CHECK(count_events(SUBMIT, 0) == 0);
   CHECK(events[event_count - 1].kind == BATCH);
   CHECK(last_batch_kind == NPT_DESC_BATCH_CREATE_CBV);
   CHECK(last_batch_heap == 0);
   CHECK(last_cbv_e.buffer_location == 0x400000 &&
         last_cbv_e.size_in_bytes == 256 && last_cbv_e.has_desc == 1 &&
         last_cbv_e.dst == BASE(1));
   CHECK(slot(1, 0) == 902);

   begin("batch-cbv-null-desc");
   id3d12device_CreateConstantBufferView_override(&object, NULL,
                                                  HANDLE_AT(9, 0));
   CHECK(events[event_count - 1].kind == BATCH);
   CHECK(last_cbv_e.has_desc == 0 && last_cbv_e.buffer_location == 0);

   begin("batch-fallback-full");
   batch_ok = false; next_ref = 903; batch_attempts = 0;
   simple(1, 0, 2);
   CHECK(batch_attempts == 1);
   CHECK(count_events(SUBMIT, 0) == 1);
   CHECK(slot(1, 0) == 903);

   begin("batch-fallback-multiring");
   ring.staging = false; ring.id = 7; atomic_store(&tail, 60);
   batch_ok = true; batch_attempts = 0;
   simple(1, 0, 2);
   CHECK(batch_attempts == 1);
   CHECK(count_events(SUBMIT, 0) == 1);
   CHECK(slot(1, 0) == DESC_LAST_PACK(7, 61));
   ring.staging = true; batch_ok = false;
}

int main(void)
{
   ring.id = 7; ring.tail = &tail; ring.staging = true;
   object.base.instance_ring = &ring;
   for (unsigned i = 0; i < 11; ++i)
      desc_heaps_register(BASE(i), 16, 512, i != 9);
   test_simple();
   test_cbv_writer();
   test_batch();
   test_ranges(2); test_ranges(16); test_ranges(17); test_ranges(257);
   test_null_and_boundaries(); test_nonstaging(); test_reader_edges();
   test_invalidation(); test_handoff();
   puts("NPT_DESCRIPTOR_COPY_TEST: PASS");
   return 0;
}
