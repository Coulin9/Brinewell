/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Wine virtgpu backend.  All DRM ioctls go through the unixlib
 * bridge.  WSI ops are shared with vtest_wine.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_transport.h"

/* DRM UAPI constants; must match the unixlib's kernel headers. */
#define VIRTGPU_BLOB_MEM_HOST3D        0x0002
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE 0x0002
#define VIRTGPU_EXECBUF_FENCE_FD_OUT   0x02
#define VIRTGPU_EXECBUF_RING_IDX       0x04
#define VIRTGPU_PARAM_3D_FEATURES      1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_PARAM_RESOURCE_BLOB    3
#define VIRTGPU_PARAM_HOST_VISIBLE     4
#define VIRTGPU_PARAM_CONTEXT_INIT     6
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME 8
#define VIRTGPU_PARAM_BLOB_ALIGNMENT     9
/* Linux flag values (not Windows) -- passed through to PRIME_HANDLE_TO_FD. */
#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC 0x00080000
#endif
#ifndef DRM_RDWR
#define DRM_RDWR    0x00000002
#endif

#include "npt_renderer.h"

_Static_assert(NPT_UNIX_MAX_TIMELINES == NPT_MAX_TIMELINES,
               "nptunix and the transport header disagree on the timeline count");

/* util/u_math.h conflicts with npt_common.h's Windows threading defs. */
static inline uint64_t npt_align64(uint64_t v, uint64_t a)
{
   return (v + a - 1) & ~(a - 1);
}
#define align64 npt_align64

struct npt_virtgpu_wine_shmem {
   struct npt_renderer_shmem base;
   uint32_t gem_handle;
};

/* No lock: the virtio-gpu DRM ioctls are thread-safe, and submits from
 * different threads have no order to preserve between them. */
struct npt_virtgpu_wine {
   struct npt_renderer base;

   int drm_fd;
   uint64_t blob_alignment;
   uint32_t status_gem;
};

static int unixlib_virtgpu_open(void)
{
   struct npt_unix_virtgpu_open_params p = { .result_fd = -1 };
   npt_wine_unix_call(npt_unix_virtgpu_open, &p);
   return p.result_fd;
}

static uint64_t unixlib_virtgpu_getparam(int fd, uint64_t param)
{
   struct npt_unix_virtgpu_getparam_params p = { .fd = fd, .param = param };
   npt_wine_unix_call(npt_unix_virtgpu_getparam, &p);
   return p.result ? 0 : p.value;
}

static int unixlib_virtgpu_get_caps(int fd, uint32_t id, uint32_t ver,
                                    void *capset, uint32_t size)
{
   struct npt_unix_virtgpu_get_caps_params p = {
      .fd = fd, .cap_set_id = id, .cap_set_ver = ver,
      .capset = capset, .size = size,
   };
   npt_wine_unix_call(npt_unix_virtgpu_get_caps, &p);
   return p.result;
}

/* Brinewell per-launch backend tag: BRINEWELL_NEPTUNE_BACKEND is set by the
 * launch wrapper only for launches accepted for a specific backend. The
 * allowlisted values become a 5-byte diagnostic prefix on the context name so
 * the host render server can route this worker to the matching validated
 * adapter. Untagged contexts keep their legacy exe-name behavior. */
static const char *
brinewell_backend_tag(void)
{
   char backend[32];
   DWORD n = GetEnvironmentVariableA("BRINEWELL_NEPTUNE_BACKEND",
                                     backend, sizeof(backend));
   if (!n || n >= sizeof(backend))
      return NULL;
   if (!strcmp(backend, "d3dmetal"))
      return "ad:d:";
   if (!strcmp(backend, "mullion"))
      return "ad:m:";
   return NULL;
}

static int unixlib_virtgpu_context_init(int fd, uint32_t capset_id)
{
   struct npt_unix_virtgpu_context_init_params p = {
      .fd = fd, .capset_id = capset_id,
   };
   const char *tag = brinewell_backend_tag();
   /* Name the context after the program, as the Windows UMD does.  A
    * kernel without the param would fail the whole init; there the kernel
    * names it after the task, truncated to 15 bytes. */
   if (unixlib_virtgpu_getparam(fd, VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME)) {
      if (tag) {
         char name[sizeof(p.debug_name)];
         if (npt_renderer_exe_name(name, sizeof(name)))
            snprintf(p.debug_name, sizeof(p.debug_name), "%s%s", tag, name);
         else
            memcpy(p.debug_name, tag, strlen(tag) + 1);
      } else {
         npt_renderer_exe_name(p.debug_name, sizeof(p.debug_name));
      }
   } else if (tag) {
      /* An explicit backend request must not silently degrade to an
       * untagged context: the host could not route it. */
      return -1;
   }
   npt_wine_unix_call(npt_unix_virtgpu_context_init, &p);
   return p.result;
}

static int unixlib_virtgpu_create_blob(int fd, uint32_t blob_mem,
                                       uint32_t blob_flags, uint64_t size,
                                       uint64_t blob_id,
                                       uint32_t *bo_handle, uint32_t *res_handle)
{
   struct npt_unix_virtgpu_create_blob_params p = {
      .fd = fd, .blob_mem = blob_mem, .blob_flags = blob_flags,
      .size = size, .blob_id = blob_id,
   };
   npt_wine_unix_call(npt_unix_virtgpu_create_blob, &p);
   if (p.result == 0) {
      *bo_handle = p.bo_handle;
      *res_handle = p.res_handle;
   }
   return p.result;
}

static void *unixlib_virtgpu_map(int fd, uint32_t gem_handle, uint64_t size)
{
   struct npt_unix_virtgpu_map_params p = {
      .fd = fd, .gem_handle = gem_handle, .size = size,
   };
   npt_wine_unix_call(npt_unix_virtgpu_map, &p);
   return p.result_ptr;
}

static int unixlib_virtgpu_execbuffer(int fd, uint32_t flags, uint32_t size,
                                      const void *cmd, uint32_t ring_idx,
                                      int *fence_fd)
{
   struct npt_unix_virtgpu_execbuffer_params p = {
      .fd = fd, .flags = flags, .size = size,
      .command = cmd, .ring_idx = ring_idx,
   };
   npt_wine_unix_call(npt_unix_virtgpu_execbuffer, &p);
   if (fence_fd)
      *fence_fd = p.fence_fd;
   return p.result;
}

static void unixlib_virtgpu_gem_close(int fd, uint32_t gem_handle)
{
   struct npt_unix_virtgpu_gem_close_params p = {
      .fd = fd, .gem_handle = gem_handle,
   };
   npt_wine_unix_call(npt_unix_virtgpu_gem_close, &p);
}

static int unixlib_virtgpu_counter_wait(int fd, uint32_t bo_handle,
                                        uint32_t counter_offset,
                                        uint32_t waiters_offset,
                                        uint32_t abort_offset,
                                        uint64_t value, int64_t timeout_ns)
{
   struct npt_unix_virtgpu_counter_wait_params p = {
      .fd = fd, .bo_handle = bo_handle,
      .counter_offset = counter_offset,
      .waiters_offset = waiters_offset,
      .abort_offset = abort_offset,
      .value = value, .timeout_ns = timeout_ns,
   };
   npt_wine_unix_call(npt_unix_virtgpu_counter_wait, &p);
   return p.result;
}

static void *unixlib_virtgpu_doorbell_create(int fd)
{
   struct npt_unix_virtgpu_doorbell_params p = { .fd = fd };
   npt_wine_unix_call(npt_unix_virtgpu_doorbell_create, &p);
   return p.result_ptr;
}

static int unixlib_virtgpu_prime_handle_to_fd(int fd, uint32_t gem_handle)
{
   struct npt_unix_virtgpu_prime_handle_to_fd_params p = {
      .fd = fd, .gem_handle = gem_handle,
      .flags = DRM_CLOEXEC | DRM_RDWR,
   };
   npt_wine_unix_call(npt_unix_virtgpu_prime_handle_to_fd, &p);
   return p.result_fd;
}

static struct npt_renderer_shmem *
npt_vgw_shmem_create(struct npt_renderer *r, size_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   size_t alloc_size = (size_t)align64(size, gpu->blob_alignment);

   uint32_t bo_handle, res_handle;
   if (unixlib_virtgpu_create_blob(gpu->drm_fd,
                                   VIRTGPU_BLOB_MEM_HOST3D,
                                   VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
                                   alloc_size, 0,
                                   &bo_handle, &res_handle)) {
      npt_log("virtgpu shmem_create: create_blob failed");
      return NULL;
   }

   void *ptr = unixlib_virtgpu_map(gpu->drm_fd, bo_handle, alloc_size);
   if (!ptr) {
      npt_log("virtgpu shmem_create: map failed");
      unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);
      return NULL;
   }

   struct npt_virtgpu_wine_shmem *s = npt_alloc(sizeof(*s));
   if (!s) {
      npt_wine_unixlib_munmap(ptr, alloc_size);
      unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);
      return NULL;
   }
   npt_refcount_init(&s->base.refcount);
   s->base.res_id = res_handle;
   s->base.size = alloc_size;
   s->base.mmap_ptr = ptr;
   s->gem_handle = bo_handle;

   return &s->base;
}

static void
npt_vgw_shmem_destroy(struct npt_renderer *r, struct npt_renderer_shmem *_s)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;
   struct npt_virtgpu_wine_shmem *s = (struct npt_virtgpu_wine_shmem *)_s;
   if (!s)
      return;
   npt_wine_unixlib_munmap(s->base.mmap_ptr, s->base.size);
   unixlib_virtgpu_gem_close(gpu->drm_fd, s->gem_handle);
   free(s);
}

static bool
npt_vgw_submit_cmd(struct npt_renderer *r, const void *data, size_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   int ret = unixlib_virtgpu_execbuffer(gpu->drm_fd,
                                        VIRTGPU_EXECBUF_RING_IDX,
                                        (uint32_t)size, data, 0, NULL);

   if (ret) {
      npt_log("virtgpu submit_cmd failed");
      return false;
   }
   return true;
}

/* EXECBUF_FENCE_FD_OUT + unixlib poll(2) on the fence. */
static bool
npt_vgw_submit_cmd_sync(struct npt_renderer *r, const void *data, size_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   struct npt_unix_virtgpu_execbuffer_params p = {
      .fd = gpu->drm_fd,
      .flags = VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT,
      .size = (uint32_t)size,
      .command = data,
      .ring_idx = 0,
      .wait_for_fence = 1,
   };

   npt_wine_unix_call(npt_unix_virtgpu_execbuffer, &p);

   if (p.result) {
      npt_log("virtgpu submit_cmd_sync failed");
      return false;
   }
   return true;
}

static int
npt_vgw_create_host_blob(struct npt_renderer *r,
                         uint64_t blob_id, uint64_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   size = align64(size, gpu->blob_alignment);

   uint32_t bo_handle, res_handle;
   if (unixlib_virtgpu_create_blob(gpu->drm_fd,
                                   VIRTGPU_BLOB_MEM_HOST3D,
                                   VIRTGPU_BLOB_FLAG_USE_MAPPABLE |
                                   VIRTGPU_BLOB_FLAG_USE_SHAREABLE,
                                   size, blob_id,
                                   &bo_handle, &res_handle)) {
      npt_log("virtgpu create_host_blob: create_blob failed for blob_id=%llu",
              (unsigned long long)blob_id);
      return -1;
   }

   int fd = unixlib_virtgpu_prime_handle_to_fd(gpu->drm_fd, bo_handle);
   unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);

   if (fd < 0)
      npt_log("virtgpu create_host_blob: prime failed for blob_id=%llu",
              (unsigned long long)blob_id);
   else
      npt_debug("virtgpu create_host_blob: blob_id=%llu -> fd %d",
                (unsigned long long)blob_id, fd);

   return fd;
}

static int
npt_vgw_submit_present_fence(struct npt_renderer *r, uint32_t ring_idx)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   int fence_fd = -1;
   int ret = unixlib_virtgpu_execbuffer(
      gpu->drm_fd,
      VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT,
      0, NULL, ring_idx, &fence_fd);

   if (ret) {
      npt_log("virtgpu submit_present_fence failed: ring_idx=%u", ring_idx);
      return -1;
   }
   return fence_fd;
}

static int
npt_vgw_counter_wait(struct npt_renderer *r, uint32_t counter_offset,
                     uint32_t waiters_offset, uint32_t abort_offset,
                     uint64_t value, int64_t timeout_ns)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;
   if (!gpu->status_gem)
      return -NPT_COUNTER_WAIT_ENODEV;
   return unixlib_virtgpu_counter_wait(gpu->drm_fd, gpu->status_gem,
                                       counter_offset, waiters_offset,
                                       abort_offset, value, timeout_ns);
}

static void
npt_vgw_status_init(struct npt_virtgpu_wine *gpu)
{
   uint32_t bo_handle, res_handle;
   if (unixlib_virtgpu_create_blob(gpu->drm_fd,
                                   VIRTGPU_BLOB_MEM_HOST3D,
                                   VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
                                   NPT_CONTEXT_STATUS_PAGE_SIZE,
                                   NPT_CONTEXT_STATUS_BLOB_ID,
                                   &bo_handle, &res_handle)) {
      npt_log("virtgpu: context status page unavailable, host rejected blob");
      return;
   }

   void *ptr = unixlib_virtgpu_map(gpu->drm_fd, bo_handle,
                                   NPT_CONTEXT_STATUS_PAGE_SIZE);
   const struct npt_context_status_page *page = ptr;
   if (!page || page->magic != NPT_CONTEXT_STATUS_MAGIC) {
      npt_log("virtgpu: context status page invalid");
      if (ptr)
         npt_wine_unixlib_munmap(ptr, NPT_CONTEXT_STATUS_PAGE_SIZE);
      unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);
      return;
   }

   if (page->version != 1 && page->version != NPT_CONTEXT_STATUS_VERSION) {
      npt_log("virtgpu: context status page version %u unsupported",
              page->version);
      npt_wine_unixlib_munmap(ptr, NPT_CONTEXT_STATUS_PAGE_SIZE);
      unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);
      return;
   }

   gpu->status_gem = bo_handle;
   gpu->base.status = ptr;
}

static void
npt_vgw_doorbell_init(struct npt_virtgpu_wine *gpu)
{
   void *ptr = unixlib_virtgpu_doorbell_create(gpu->drm_fd);
   if (!ptr) {
      npt_log("virtgpu: no MMIO doorbell");
      return;
   }
   gpu->base.doorbell = ptr;
   npt_log("virtgpu doorbell mapped");
}

static void
npt_vgw_destroy(struct npt_renderer *r)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   if (gpu->base.status) {
      npt_wine_unixlib_munmap(gpu->base.status, NPT_CONTEXT_STATUS_PAGE_SIZE);
      unixlib_virtgpu_gem_close(gpu->drm_fd, gpu->status_gem);
   }
   if (gpu->base.doorbell)
      npt_wine_unixlib_munmap((void *)gpu->base.doorbell, 4096);
   if (gpu->drm_fd >= 0)
      npt_wine_unixlib_close(gpu->drm_fd);

   free(gpu);
}

struct npt_renderer *
npt_renderer_create_virtgpu(void)
{
   if (npt_unixlib_ensure_init() < 0)
      return NULL;

   int drm_fd = unixlib_virtgpu_open();
   if (drm_fd < 0) {
      npt_debug("virtgpu: no virtio_gpu DRM device found");
      return NULL;
   }

   static const uint64_t required[] = {
      VIRTGPU_PARAM_3D_FEATURES,
      VIRTGPU_PARAM_CAPSET_QUERY_FIX,
      VIRTGPU_PARAM_RESOURCE_BLOB,
      VIRTGPU_PARAM_CONTEXT_INIT,
      VIRTGPU_PARAM_HOST_VISIBLE,
   };
   for (uint32_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
      if (!unixlib_virtgpu_getparam(drm_fd, required[i])) {
         npt_debug("virtgpu: missing param %llu", (unsigned long long)required[i]);
         npt_wine_unixlib_close(drm_fd);
         return NULL;
      }
   }

   /* Older kernels do not expose the alignment query and use 4 KiB. */
   uint64_t blob_alignment =
      unixlib_virtgpu_getparam(drm_fd, VIRTGPU_PARAM_BLOB_ALIGNMENT);
   if (!blob_alignment)
      blob_alignment = 4096;
   if (blob_alignment < 4096 ||
       (blob_alignment & (blob_alignment - 1))) {
      npt_log("virtgpu: invalid blob alignment %llu",
              (unsigned long long)blob_alignment);
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   struct npt_capset capset;
   memset(&capset, 0, sizeof(capset));
   if (unixlib_virtgpu_get_caps(drm_fd, NPT_CAPSET_ID, 0,
                                &capset, sizeof(capset))) {
      npt_debug("virtgpu: failed to get Neptune capset");
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }
   if (capset.wire_format_version != NPT_PROTOCOL_WIRE_VERSION) {
      npt_debug("virtgpu: wire format mismatch: host 0x%08x, guest 0x%08x",
                capset.wire_format_version,
                (unsigned)NPT_PROTOCOL_WIRE_VERSION);
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   if (unixlib_virtgpu_context_init(drm_fd, NPT_CAPSET_ID)) {
      npt_debug("virtgpu: context init failed");
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   struct npt_virtgpu_wine *gpu = npt_alloc(sizeof(*gpu));
   if (!gpu) {
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   gpu->drm_fd = drm_fd;
   gpu->blob_alignment = blob_alignment;

   gpu->base.info.wire_format_version = capset.wire_format_version;
   gpu->base.info.caps_flags = capset.caps_flags;
   gpu->base.info.max_timeline_count = NPT_MAX_TIMELINES;

   gpu->base.ops.destroy = npt_vgw_destroy;
   gpu->base.ops.submit_cmd = npt_vgw_submit_cmd;
   gpu->base.ops.submit_cmd_sync = npt_vgw_submit_cmd_sync;
   gpu->base.ops.create_host_blob = npt_vgw_create_host_blob;
   gpu->base.ops.submit_present_fence = npt_vgw_submit_present_fence;
   gpu->base.ops.counter_wait = npt_vgw_counter_wait;
   npt_wine_install_wsi_ops(&gpu->base.ops);
   gpu->base.shmem_ops.create = npt_vgw_shmem_create;
   gpu->base.shmem_ops.destroy = npt_vgw_shmem_destroy;
   npt_vgw_status_init(gpu);
   npt_vgw_doorbell_init(gpu);

   npt_log("virtgpu renderer created (drm_fd=%d, wire_format=0x%08x, "
           "blob_alignment=%llu)", gpu->drm_fd, capset.wire_format_version,
           (unsigned long long)gpu->blob_alignment);

   return &gpu->base;
}
