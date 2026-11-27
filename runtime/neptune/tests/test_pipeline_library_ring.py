import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


source_dir = Path(__file__).resolve().parents[1]


def extract_function(path, name):
    source = path.read_text()
    pattern = (
        rf"(?:static inline )?(?:(?:struct npt_ring|void) \*|HRESULT NPT_STDMETHODCALLTYPE)"
        rf"\n{name}\([^)]*\)\n\{{.*?^\}}"
    )
    match = re.search(pattern, source, re.S | re.M)
    if not match:
        raise RuntimeError(f"{name} definition not found")
    return match[0]


family_source = (
    source_dir / "neptune-protocol/npt_protocol_client_id3d12pipelinelibrary.c"
).read_text()
family = re.search(
    r"static const GUID \*const npt_com_family_id3d12pipelinelibrary_tiers\[\].*?"
    r"^const struct npt_com_family npt_com_family_id3d12pipelinelibrary = \{.*?^\};",
    family_source,
    re.S | re.M,
)
if not family:
    raise RuntimeError("PipelineLibrary family descriptor not found")

prefix = r"""
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/list.h"

#define NPT_IFACE_FAMILY_ID3D12PIPELINELIBRARY 64u
#define NPT_OBJECT_TYPE_ID3D12PIPELINELIBRARY 140u
#define NPT_OBJECT_TYPE_ID3D12PIPELINELIBRARY1 141u
#define NPT_DC_SC_RING_BUFFER_SIZE (16u * 1024u)
#define NPT_STDMETHODCALLTYPE
#define NPT_SUCCEEDED(hr) ((hr) >= 0)
#define mtx_lock pthread_mutex_lock
#define mtx_unlock pthread_mutex_unlock
typedef unsigned GUID;
typedef unsigned npt_object_type;
typedef int32_t HRESULT;
typedef size_t SIZE_T;
typedef GUID IID;
static const HRESULT driver_version_mismatch = (HRESULT)0x887e0002;
static const GUID NPT_IID_ID3D12PipelineLibrary = 140;
static const GUID NPT_IID_ID3D12PipelineLibrary1 = 141;
static const GUID NPT_IID_IUnknown = 1;
static const GUID NPT_IID_ID3D12Object = 2;
static const GUID NPT_IID_ID3D12DeviceChild = 3;
struct npt_com_family {
   const char *name;
   uint32_t index;
   const GUID *const *tier_iids;
   const npt_object_type *tier_types;
   uint32_t tier_count;
   const GUID *const *ancestor_iids;
   const void **vtbl;
   void (*vtbl_install)(void);
};
static const void *npt_id3d12pipelinelibrary1_default_vtbl_storage;
static void npt_id3d12pipelinelibrary1_install_default_vtbl(void) {}
struct npt_ring {
   uint64_t id;
   bool staging;
   atomic_uint tail;
   struct list_head instance_head;
};
struct npt_device {
   struct npt_ring *ring;
   _Atomic(struct npt_ring *) pipeline_library_ring;
   pthread_mutex_t instance_rings_mutex;
   struct list_head instance_rings;
   atomic_uint_fast64_t next_ring_id;
   bool multi_ring_enabled;
};
struct npt_com_base {
   const void **lpVtbl;
   struct {
      struct npt_device *device;
      struct npt_ring *instance_ring;
      bool ring_ordered;
      atomic_uint stage_index;
      atomic_uint_fast64_t order_ring_id;
   } base;
   const struct npt_com_family *family;
   unsigned tier;
};
struct npt_ring_layout { size_t buffer_size, extra_size; };
static atomic_uint create_calls, flush_calls, order_calls, ready;
static bool fail_create;
static struct npt_ring *tls_ring;
static _Thread_local unsigned phase;
static _Thread_local uint32_t ordered_tail;

static void *npt_alloc(size_t size) { return calloc(1, size); }
static void
npt_com_base_init(struct npt_com_base *com, const void **vtbl,
                  struct npt_device *dev, uint64_t id)
{
   (void)id;
   com->lpVtbl = vtbl;
   com->base.device = dev;
}
static void
npt_ring_get_layout(size_t size, size_t extra, struct npt_ring_layout *layout)
{
   layout->buffer_size = size;
   layout->extra_size = extra;
}
static struct npt_ring *
npt_ring_create(struct npt_device *dev, const struct npt_ring_layout *layout,
                 uint64_t id, bool is_tls)
{
   (void)dev;
   atomic_fetch_add(&create_calls, 1);
   assert(layout->buffer_size == NPT_DC_SC_RING_BUFFER_SIZE);
   assert(layout->extra_size == sizeof(uint32_t));
   assert(!is_tls);
   if (fail_create)
      return NULL;
   struct npt_ring *ring = calloc(1, sizeof(*ring));
   assert(ring);
   ring->id = id;
   list_inithead(&ring->instance_head);
   return ring;
}
static void npt_ring_stage_flush_all(struct npt_ring *ring)
{
   assert(phase == 0);
   atomic_fetch_add(&ring->tail, 1);
   atomic_fetch_add(&flush_calls, 1);
   phase = 1;
}
static uint32_t npt_ring_seqno_now(struct npt_ring *ring)
{
   assert(phase == 1);
   ordered_tail = atomic_load(&ring->tail);
   phase = 2;
   return ordered_tail;
}
static void
npt_ring_order_after_ring(struct npt_ring *ring, struct npt_ring *target,
                           uint32_t tail)
{
   assert(ring != target && target->id == 1);
   assert(phase == 2 && tail == ordered_tail);
   atomic_fetch_add(&order_calls, 1);
   phase = 0;
}
static struct npt_ring *npt_tls_get_ring(struct npt_device *dev)
{ return tls_ring ? tls_ring : dev->ring; }
static uint32_t npt_ring_stage_self_index(struct npt_ring *ring)
{ (void)ring; return 1; }
static void npt_ring_stage_flush_index(struct npt_ring *ring, uint32_t index)
{ (void)ring; (void)index; }
static bool
npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t id, uint32_t *tail)
{ (void)dev; (void)id; (void)tail; return false; }
static void npt_ring_order_after(struct npt_ring *ring, uint64_t id, uint32_t tail)
{ (void)ring; (void)id; (void)tail; }

static uint64_t npt_com_self_id(void *self) { (void)self; return 2; }
static struct npt_device *npt_com_self_device(void *self)
{ return ((struct npt_com_base *)self)->base.device; }
static struct npt_com_base wrapped_library;
static unsigned wrap_calls;
static void *npt_com_get_or_wrap(struct npt_device *dev, const IID *iid,
                                uint64_t id, struct npt_com_base *self)
{
   assert(dev == self->base.device && iid == &NPT_IID_ID3D12PipelineLibrary && id == 3);
   ++wrap_calls;
   return &wrapped_library;
}

static HRESULT
npt_call_ID3D12Device1_CreatePipelineLibrary(struct npt_ring *ring, uint64_t id,
                                          const void *blob, SIZE_T size,
                                          const IID *iid, void **output)
{
   assert(ring && id == 2 && iid == &NPT_IID_ID3D12PipelineLibrary);
   assert((blob == NULL) == (size == 0));
   if (output)
      *output = (void *)(uintptr_t)3;
   if (size)
      return driver_version_mismatch;
   return output ? 0 : 1;
}
"""

suffix = r"""
static void init_device(struct npt_device *dev, struct npt_ring *primary)
{
   memset(dev, 0, sizeof(*dev));
   memset(primary, 0, sizeof(*primary));
   primary->id = 1;
   dev->ring = primary;
   atomic_init(&dev->next_ring_id, 2);
   assert(pthread_mutex_init(&dev->instance_rings_mutex, NULL) == 0);
   list_inithead(&dev->instance_rings);
   atomic_store(&create_calls, 0);
   atomic_store(&flush_calls, 0);
   atomic_store(&order_calls, 0);
}
static void finish_device(struct npt_device *dev)
{
   struct npt_ring *ring = atomic_load(&dev->pipeline_library_ring);
   assert(ring && dev->instance_rings.next == &ring->instance_head);
   assert(dev->instance_rings.prev == &ring->instance_head);
   assert(ring->instance_head.next == &dev->instance_rings);
   list_del(&ring->instance_head);
   free(ring);
   assert(pthread_mutex_destroy(&dev->instance_rings_mutex) == 0);
}
static void *concurrent_lookup(void *data)
{
   struct npt_device *dev = data;
   struct npt_com_base *library = npt_id3d12pipelinelibrary_client_create(dev, 1);
   struct npt_com_base *library1 = npt_id3d12pipelinelibrary1_client_create(dev, 2);
   assert(library && library1 && library->tier == 0 && library1->tier == 1);
   assert(library->family == library1->family);
   atomic_fetch_add(&ready, 1);
   while (atomic_load(&ready) < 8)
      sched_yield();
   struct npt_ring *ring = npt_com_self_ring(library);
   assert(ring != dev->ring && !ring->staging);
   for (unsigned i = 0; i < 100; i++)
      assert(npt_com_self_ring(i % 2 ? library : library1) == ring);
   free(library);
   free(library1);
   return ring;
}
int main(void)
{
   struct npt_device dev;
   struct npt_ring primary;
   init_device(&dev, &primary);
   pthread_t threads[8];
   void *results[8];
   for (unsigned i = 0; i < 8; i++)
      assert(pthread_create(&threads[i], NULL, concurrent_lookup, &dev) == 0);
   for (unsigned i = 0; i < 8; i++) {
      assert(pthread_join(threads[i], &results[i]) == 0);
      assert(results[i] == results[0]);
   }
   assert(atomic_load(&create_calls) == 1);
   assert(atomic_load(&flush_calls) == 808 && atomic_load(&order_calls) == 808);
   finish_device(&dev);
   puts("PASS concurrent creation, both library tiers, per-call ordering");

   init_device(&dev, &primary);
   struct npt_com_base *library = npt_id3d12pipelinelibrary1_client_create(&dev, 1);
   fail_create = true;
   assert(npt_com_self_ring(library) == &primary);
   assert(atomic_load(&create_calls) == 1 && list_is_empty(&dev.instance_rings));
   assert(atomic_load(&flush_calls) == 0 && atomic_load(&order_calls) == 0);
   fail_create = false;
   assert(npt_com_self_ring(library) != &primary);
   assert(atomic_load(&create_calls) == 2 && atomic_load(&order_calls) == 1);
   finish_device(&dev);
   free(library);
   puts("PASS allocation failure fallback and retry");

   init_device(&dev, &primary);
   struct npt_ring other = {0};
   struct npt_com_base normal = {0};
   normal.base.device = &dev;
   assert(npt_com_self_ring(&normal) == &primary);
   static const char old_cache[] = "another driver's pipeline cache";
   void *output = &normal;
   assert(npt_id3d12device1_default_CreatePipelineLibrary(
      &normal, old_cache, sizeof(old_cache), &NPT_IID_ID3D12PipelineLibrary, &output)
      == driver_version_mismatch && !output && wrap_calls == 0);
   assert(npt_id3d12device1_default_CreatePipelineLibrary(
      &normal, NULL, 0, &NPT_IID_ID3D12PipelineLibrary, NULL) == 1 && wrap_calls == 0);
   assert(npt_id3d12device1_default_CreatePipelineLibrary(
      &normal, NULL, 0, &NPT_IID_ID3D12PipelineLibrary, &output) == 0 &&
      output == &wrapped_library && wrap_calls == 1);
   puts("PASS pipeline library creation returns cache mismatch, validation and success");
   normal.base.instance_ring = &other;
   assert(npt_com_self_ring(&normal) == &other);
   library = npt_id3d12pipelinelibrary1_client_create(&dev, 1);
   dev.multi_ring_enabled = true;
   tls_ring = &other;
   assert(npt_com_self_ring(library) == &other);
   assert(atomic_load(&create_calls) == 0 && atomic_load(&flush_calls) == 0);
   free(library);
   assert(pthread_mutex_destroy(&dev.instance_rings_mutex) == 0);
   puts("PASS ordinary and explicit multi-ring routing");
   return 0;
}
"""

functions = [
    extract_function(source_dir / "npt_device.c", "npt_device_get_pipeline_library_ring"),
    extract_function(source_dir / "npt_com.h", "npt_com_self_ring"),
    extract_function(
        source_dir / "neptune-protocol/npt_protocol_client_id3d12device.c",
        "npt_id3d12device1_default_CreatePipelineLibrary",
    ),
]
functions += [
    extract_function(
        source_dir / "neptune-protocol/npt_protocol_client_id3d12pipelinelibrary.c",
        name,
    )
    for name in (
        "npt_id3d12pipelinelibrary_client_create",
        "npt_id3d12pipelinelibrary1_client_create",
    )
]

with tempfile.TemporaryDirectory(prefix="npt-library-ring-") as directory:
    directory = Path(directory)
    test_source = directory / "test.c"
    executable = directory / "test"
    test_source.write_text(prefix + family[0] + "\n".join(functions) + suffix)
    command = shlex.split(os.environ.get("CC", "cc"))
    command += ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    mesa = source_dir / "subprojects/mesa"
    if not mesa.is_dir():
        mesa = source_dir / "../../.build/out/kosmicrisp/src"
    command += ["-I", str(mesa / "src")]
    subprocess.run(command + [str(test_source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)
