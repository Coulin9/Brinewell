/* npt_d3d_map_ring slot-choice unit test: per-context NO_OVERWRITE
 * bases across several deferred contexts on one resource.  Includes the
 * production translation unit and stubs the shmem pool, renderer and
 * ring-wait boundaries. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../npt_resource.c"
#include "../npt_env.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#pragma GCC diagnostic ignored "-Wunused-parameter"

struct npt_env npt_env;
void npt_env_init(void) {}
void npt_log_impl(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
}

static struct npt_renderer renderer;
static struct npt_device device;
static struct npt_ring dev_ring;
static struct npt_com_base com;
static uint32_t next_res_id;
static size_t next_offset;
static _Atomic uint32_t destroyed;
static unsigned waits, ensured_visible;

static void
fake_shmem_destroy(struct npt_renderer *r,
                   struct npt_renderer_shmem *shmem)
{
   ++destroyed;
   free(shmem->mmap_ptr);
   free(shmem);
}

struct npt_renderer_shmem *
npt_shmem_pool_alloc(struct npt_renderer *r, struct npt_shmem_pool *pool,
                     size_t size, size_t *out_offset)
{
   struct npt_renderer_shmem *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   npt_refcount_init(&s->refcount);
   s->res_id = ++next_res_id;
   s->size = size;
   s->mmap_ptr = calloc(1, size);
   *out_offset = next_offset;
   next_offset += size;
   return s;
}

uint32_t
npt_ring_wait_seqno(struct npt_ring *ring, uint32_t seqno)
{
   ++waits;
   return 0;
}

void
npt_ring_ensure_shmem_visible(struct npt_ring *ring,
                              struct npt_renderer_shmem *shmem)
{
   ++ensured_visible;
}

/* Kept globals of npt_resource.c reference these; the ring tests never
 * reach them. */
HRESULT
npt_dispatch_bind_d3d11_buffer_shmem(struct npt_ring *ring,
                                     uint64_t buffer_id,
                                     uint32_t shmem_res_id,
                                     uint32_t cookie,
                                     uint64_t byte_size)
{
   CHECK(false);
   return E_FAIL;
}

bool
npt_dispatch_resource_update(struct npt_ring *ring, uint64_t context_id,
                             uint64_t resource_host_id, uint32_t subresource,
                             uint32_t row_pitch, uint32_t depth_pitch,
                             const D3D11_BOX *box, const void *data,
                             uint32_t byte_size, uint32_t copy_size)
{
   CHECK(false);
   return false;
}

static void
ring_setup(struct npt_d3d_map_ring *r, uint32_t slot_size)
{
   memset(r, 0, sizeof(*r));
   npt_d3d_map_ring_init(r, &com);
   CHECK(npt_d3d_map_ring_alloc_shmem(r, slot_size));
}

static uint32_t
open_slot(struct npt_d3d_map_ring *r, const void *ctx,
          enum npt_d3d_map_slot_choice choice)
{
   struct npt_d3d_map_record rec = { .context = ctx, .subresource = 0 };
   CHECK(npt_d3d_map_ring_open(r, &rec, choice));
   return rec.slot;
}

static void
close_map(struct npt_d3d_map_ring *r, const void *ctx)
{
   /* NULL ring: the Unmap's hold drops without going in flight. */
   npt_d3d_map_ring_close(r, ctx, 0, NULL, 0);
}

/* The Unmap went out on `ring` at `seqno`: the slot stays in flight
 * until the host consumes it. */
static void
close_map_inflight(struct npt_d3d_map_ring *r, const void *ctx)
{
   npt_d3d_map_ring_close(r, ctx, 0, &dev_ring, 1);
}

/* (a) Single context: DISCARD rotates, NO_OVERWRITE stays on the
 * DISCARD slot. */
static void
test_single_context(void)
{
   struct npt_d3d_map_ring r;
   ring_setup(&r, 1024);
   const void *a = (const void *)0xA;

   uint32_t s1 = open_slot(&r, a, NPT_D3D_MAP_SLOT_NEXT);
   close_map_inflight(&r, a);
   uint32_t s2 = open_slot(&r, a, NPT_D3D_MAP_SLOT_NEXT);
   close_map_inflight(&r, a);
   CHECK(s2 != s1);
   CHECK(open_slot(&r, a, NPT_D3D_MAP_SLOT_CURRENT) == s2);
   close_map(&r, a);

   /* A context with no base keeps the current_slot fallback. */
   struct npt_d3d_map_ring r2;
   ring_setup(&r2, 1024);
   const void *b = (const void *)0xB;
   CHECK(open_slot(&r2, b, NPT_D3D_MAP_SLOT_CURRENT) == r2.current_slot);
   close_map(&r2, b);
   npt_d3d_map_ring_fini(&r2);

   npt_d3d_map_ring_fini(&r);
   fprintf(stderr, "ok single_context\n");
}

/* (b) Two interleaved contexts keep separate append slots. */
static void
test_two_contexts(void)
{
   struct npt_d3d_map_ring r;
   ring_setup(&r, 1024);
   const void *a = (const void *)0xA, *b = (const void *)0xB;

   uint32_t sa = open_slot(&r, a, NPT_D3D_MAP_SLOT_NEXT);
   close_map(&r, a);
   uint32_t sb = open_slot(&r, b, NPT_D3D_MAP_SLOT_NEXT);
   close_map(&r, b);
   CHECK(sa != sb);
   CHECK(open_slot(&r, a, NPT_D3D_MAP_SLOT_CURRENT) == sa);
   close_map(&r, a);
   CHECK(open_slot(&r, b, NPT_D3D_MAP_SLOT_CURRENT) == sb);
   close_map(&r, b);
   npt_d3d_map_ring_fini(&r);
   fprintf(stderr, "ok two_contexts\n");
}

/* (c) Interleaved DISCARDs of B never land on A's base slot while
 * other slots are free. */
static void
test_discard_avoids_base(void)
{
   struct npt_d3d_map_ring r;
   ring_setup(&r, 1024);
   const void *a = (const void *)0xA, *b = (const void *)0xB;

   uint32_t sa = open_slot(&r, a, NPT_D3D_MAP_SLOT_NEXT);
   close_map(&r, a);
   for (unsigned i = 0; i < NPT_D3D_MAP_SLOT_MAX * 2; i++) {
      uint32_t sb = open_slot(&r, b, NPT_D3D_MAP_SLOT_NEXT);
      CHECK(sb != sa);
      close_map(&r, b);
   }
   npt_d3d_map_ring_fini(&r);
   fprintf(stderr, "ok discard_avoids_base\n");
}

/* (d) At cap with every slot another context's base: the oldest base
 * is evicted and that context's CURRENT falls back to current_slot. */
static void
test_base_eviction_at_cap(void)
{
   struct npt_d3d_map_ring r;
   ring_setup(&r, 128 * 1024);   /* max_slots = 8 */
   const void *ctxs[9];
   for (unsigned i = 0; i < 9; i++)
      ctxs[i] = (const void *)(uintptr_t)(0x100 + i);

   for (unsigned i = 0; i < 8; i++) {
      open_slot(&r, ctxs[i], NPT_D3D_MAP_SLOT_NEXT);
      close_map(&r, ctxs[i]);
   }
   CHECK(r.base_count == 8);
   uint32_t s9 = open_slot(&r, ctxs[8], NPT_D3D_MAP_SLOT_NEXT);
   close_map(&r, ctxs[8]);
   /* No open Map holds anything; ctxs[0]'s base was oldest. */
   CHECK(find_base(&r, ctxs[0]) == NULL);
   CHECK(find_base(&r, ctxs[8]) != NULL &&
         find_base(&r, ctxs[8])->slot == s9);
   CHECK(open_slot(&r, ctxs[0], NPT_D3D_MAP_SLOT_CURRENT) ==
         r.current_slot);
   close_map(&r, ctxs[0]);
   npt_d3d_map_ring_fini(&r);
   fprintf(stderr, "ok base_eviction_at_cap\n");
}

/* (e) ZERO drops another context's base on slot 0. */
static void
test_zero_drops_base(void)
{
   struct npt_d3d_map_ring r;
   ring_setup(&r, 1024);
   const void *x = (const void *)(uintptr_t)0x10,
              *y = (const void *)(uintptr_t)0x11;

   /* y DISCARDs until it lands on slot 0. */
   uint32_t s = ~0u;
   for (unsigned i = 0; i < NPT_D3D_MAP_SLOT_MAX && s != 0; i++) {
      s = open_slot(&r, y, NPT_D3D_MAP_SLOT_NEXT);
      close_map(&r, y);
   }
   CHECK(s == 0);
   CHECK(find_base(&r, y) && find_base(&r, y)->slot == 0);

   CHECK(open_slot(&r, x, NPT_D3D_MAP_SLOT_ZERO) == 0);
   close_map(&r, x);
   CHECK(find_base(&r, y) == NULL);
   npt_d3d_map_ring_fini(&r);
   fprintf(stderr, "ok zero_drops_base\n");
}

/* (f) Beyond NPT_D3D_MAP_BASE_MAX contexts the least recently set
 * base is replaced. */
static void
test_base_lru(void)
{
   struct npt_d3d_map_ring r;
   ring_setup(&r, 1024);   /* max_slots = 32 > BASE_MAX */
   const void *ctxs[NPT_D3D_MAP_BASE_MAX + 1];
   for (unsigned i = 0; i <= NPT_D3D_MAP_BASE_MAX; i++)
      ctxs[i] = (const void *)(uintptr_t)(0x200 + i);

   for (unsigned i = 0; i <= NPT_D3D_MAP_BASE_MAX; i++) {
      open_slot(&r, ctxs[i], NPT_D3D_MAP_SLOT_NEXT);
      close_map(&r, ctxs[i]);
   }
   CHECK(r.base_count == NPT_D3D_MAP_BASE_MAX);
   CHECK(find_base(&r, ctxs[0]) == NULL);
   for (unsigned i = 1; i <= NPT_D3D_MAP_BASE_MAX; i++)
      CHECK(find_base(&r, ctxs[i]) != NULL);
   /* The evicted context's CURRENT falls back to current_slot. */
   CHECK(open_slot(&r, ctxs[0], NPT_D3D_MAP_SLOT_CURRENT) ==
         r.current_slot);
   close_map(&r, ctxs[0]);
   npt_d3d_map_ring_fini(&r);
   fprintf(stderr, "ok base_lru\n");
}

int
main(void)
{
   memset(&renderer, 0, sizeof(renderer));
   renderer.shmem_ops.destroy = fake_shmem_destroy;
   memset(&device, 0, sizeof(device));
   device.renderer = &renderer;
   device.ring = &dev_ring;
   memset(&dev_ring, 0, sizeof(dev_ring));
   memset(&com, 0, sizeof(com));
   com.base.device = &device;

   test_single_context();
   test_two_contexts();
   test_discard_avoids_base();
   test_base_eviction_at_cap();
   test_zero_drops_base();
   test_base_lru();

   /* Let the reaper drain before reporting. */
   for (unsigned i = 0; i < 1000 &&
        destroyed != next_res_id; i++)
      Sleep(1);
   fprintf(stderr, "PASS map_ring (%u shmems destroyed, %u waits)\n",
           destroyed, waits);
   return 0;
}
