/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared interface between Wine PE DLLs and the nptunix.so unixlib
 * bridge.  enum npt_unix_funcs enumerates every operation the .so
 * exports.
 */

#ifndef NPT_UNIXLIB_H
#define NPT_UNIXLIB_H

#include <stdint.h>

#include "../npt_display.h"

#define NPT_WSI_MAX_IMAGES 4

/* NPT_MAX_TIMELINES for the unix side, which cannot include the transport
 * header (it pulls in the DirectX types); the PE side asserts they agree. */
#define NPT_UNIX_MAX_TIMELINES 64u

struct npt_unix_connect_params {
   const char *path;
   int result_fd;
};

struct npt_unix_rw_params {
   int fd;
   void *buf;
   uint64_t size;
   int64_t result;
};

struct npt_unix_close_params {
   int fd;
};

struct npt_unix_dup_params {
   int fd;
   /* >=0 the duplicated fd, -1 on error. */
   int result;
};

struct npt_unix_wait_fd_params {
   int fd;
   int timeout_ms;
   /* >0 signaled, 0 timeout, <0 error. */
   int result;
};

struct npt_unix_receive_fd_params {
   int sock_fd;
   int result_fd;
};

struct npt_unix_mmap_params {
   uint64_t size;
   int fd;
   void *result_ptr;
};

struct npt_unix_munmap_params {
   void *addr;
   uint64_t size;
};

struct npt_unix_hdr10_metadata {
   uint32_t valid;
   uint16_t red_primary[2], green_primary[2], blue_primary[2], white_point[2];
   uint32_t max_mastering_luminance, min_mastering_luminance;
   uint16_t max_cll, max_fall;
};

#define NPT_DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 0u
#define NPT_DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 1u
#define NPT_DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 12u
#define NPT_HDR_FMT_R10G10B10A2 (1u << 0)
#define NPT_HDR_FMT_R16G16B16A16F (1u << 1)

struct npt_unix_wsi_hdr_caps_args {
   uint32_t hdr_supported;
   uint32_t format_flags;
   float max_luminance, max_full_frame_luminance, min_luminance;
};

struct npt_unix_wsi_set_hdr_metadata_args {
   uint64_t handle;
   struct npt_unix_hdr10_metadata meta;
   int32_t result;
};

struct npt_unix_wsi_init_params {
   uint32_t x11_window;
   uint32_t num_images;
   int fds[NPT_WSI_MAX_IMAGES];
   uint32_t strides[NPT_WSI_MAX_IMAGES];
   uint32_t virgl_format;
   uint32_t width;
   uint32_t height;
   int result;
   uint64_t wsi;        /* out: handle for the calls below */
   uint32_t frame_stats_enabled;
   uint32_t colorspace;
   struct npt_unix_hdr10_metadata hdr_meta;
   int16_t x_off;
   int16_t y_off;
};

/*
 * wait_fence_fd: sync_file (or -1) signalling host GPU done.  The
 * unixlib must close it after passing to xcb_dri3_fence_from_fd
 * (DRI3 >= 1.2) or after a poll(2) fallback; sets wait_fence_consumed
 * if it did, otherwise the PE side closes via the bridge.
 *
 * result: 0 = accepted, -1 = rejected before X submission,
 * -2 = terminal X/window error.
 *
 * Per-image release tracking goes through npt_unix_wsi_drain instead.
 */
struct npt_unix_wsi_present_params {
   uint64_t wsi;
   uint32_t image_index;
   int result;
   int wait_fence_fd;
   uint32_t wait_fence_consumed;
   uint64_t present_time_ns;
};

/*
 * Per-image release COUNTS (NOT a bitmask): a fast client can fire
 * multiple idles for the same image between drain polls; a bitmask
 * would collapse them and stall the host's pending_presents.
 * timeout_ms caps the wait so the forwarder can recheck its exit flag.
 * result: 0 = healthy, -1 = cancelled,
 * -2 = terminal X/window/syncobj error.
 */
struct npt_unix_wsi_drain_params {
   uint64_t wsi;
   uint32_t timeout_ms;
   uint32_t released_count[NPT_WSI_MAX_IMAGES];
   int result;
   uint32_t pad;
};

struct npt_unix_wsi_destroy_params {
   uint64_t wsi;
};

struct npt_unix_wsi_cancel_params {
   uint64_t wsi;
};

struct npt_unix_virtgpu_open_params {
   int result_fd;
};

struct npt_unix_virtgpu_getparam_params {
   int fd;
   uint64_t param;
   uint64_t value;
   int result;
};

struct npt_unix_virtgpu_get_caps_params {
   int fd;
   uint32_t cap_set_id;
   uint32_t cap_set_ver;
   void *capset;
   uint32_t size;
   int result;
};

struct npt_unix_virtgpu_context_init_params {
   int fd;
   uint32_t capset_id;
   /* VIRTGPU_CONTEXT_PARAM_DEBUG_NAME when non-empty (the kernel keeps
    * 63 bytes). */
   char debug_name[64];
   int result;
};

struct npt_unix_virtgpu_create_blob_params {
   int fd;
   uint32_t blob_mem;
   uint32_t blob_flags;
   uint64_t size;
   uint64_t blob_id;
   uint32_t bo_handle;
   uint32_t res_handle;
   int result;
};

struct npt_unix_virtgpu_map_params {
   int fd;
   uint32_t gem_handle;
   uint64_t size;
   void *result_ptr;
};

struct npt_unix_virtgpu_execbuffer_params {
   int fd;
   uint32_t flags;
   uint32_t size;
   const void *command;
   uint32_t ring_idx;
   /* Out if FENCE_FD_OUT, else -1. */
   int fence_fd;
   int result;
   /* With FENCE_FD_OUT: unixlib poll(2)s + closes the fence; PE has
    * no poll(). */
   uint32_t wait_for_fence;
};

struct npt_unix_virtgpu_gem_close_params {
   int fd;
   uint32_t gem_handle;
};

struct npt_unix_virtgpu_prime_handle_to_fd_params {
   int fd;
   uint32_t gem_handle;
   uint32_t flags;
   int result_fd;
};

struct npt_unix_virtgpu_counter_wait_params {
   int fd;
   uint32_t bo_handle;
   uint32_t counter_offset;
   uint32_t waiters_offset;
   uint32_t abort_offset;
   uint64_t value;
   int64_t timeout_ns;
   int result;
};

struct npt_unix_virtgpu_doorbell_params {
   int fd;
   void *result_ptr;
};

/* Walks every connected connector on the picked DRM device (viogpu
 * preferred, KMS fallback).  list.num_outputs == 0 if no DRM device
 * was usable; caller fabricates a synthetic output in that case. */
struct npt_unix_enumerate_outputs_params {
   struct npt_output_list list;
   /* When zero, the mode list is capped to the active CRTC scanout; the
    * PE side sets it from NPT_DEBUG=expose_all_modes. */
   uint32_t expose_all_modes;
};

/*
 * Process-wide npt_device singleton bridge (see npt_device.c).
 *
 * acquire: caller obtains the current device.  If `won_creation=1`,
 *   the caller has been elected to create the device and must call
 *   publish afterwards (success or failure).  Concurrent acquirers
 *   sleep on a cond var while creation is in flight.
 * publish: caller (won_creation==1) reports the result of npt_device_create.
 * release: dec refcount; if `device_out != NULL`, caller must destroy it.
 */
struct npt_unix_singleton_acquire_params {
   void *device_out;
   uint32_t won_creation;
   uint32_t pad;
};

struct npt_unix_singleton_publish_params {
   void *device;
   uint32_t success;
   uint32_t pad;
};

struct npt_unix_singleton_release_params {
   void *device_out;
   uint32_t pad0, pad1;
};

/*
 * Process-wide guest object id allocator.  npt_com_allocate_next_id
 * generates monotonically-increasing 64-bit ids that the encoder
 * stashes into output COM-handle slots before submit.  These ids
 * must be unique across PE DLLs in the same Wine process because
 * they share one host context (see the singleton bridge above) and
 * therefore one host-side object table.
 */
struct npt_unix_alloc_obj_id_params {
   uint64_t result_id;
};

enum npt_unix_funcs {
   npt_unix_connect,
   npt_unix_read,
   npt_unix_write,
   npt_unix_close,
   npt_unix_receive_fd,
   npt_unix_mmap,
   npt_unix_munmap,
   npt_unix_wait_fd,
   npt_unix_wsi_init,
   npt_unix_wsi_present,
   npt_unix_wsi_destroy,
   npt_unix_wsi_drain,
   npt_unix_virtgpu_open,
   npt_unix_virtgpu_getparam,
   npt_unix_virtgpu_get_caps,
   npt_unix_virtgpu_context_init,
   npt_unix_virtgpu_create_blob,
   npt_unix_virtgpu_map,
   npt_unix_virtgpu_execbuffer,
   npt_unix_virtgpu_gem_close,
   npt_unix_virtgpu_prime_handle_to_fd,
   npt_unix_virtgpu_counter_wait,
   npt_unix_enumerate_outputs,
   npt_unix_dup,
   npt_unix_singleton_acquire,
   npt_unix_singleton_publish,
   npt_unix_singleton_release,
   npt_unix_alloc_obj_id,
   npt_unix_wsi_cancel,
   npt_unix_ring_yield,
   npt_unix_virtgpu_doorbell_create,
   NPT_UNIX_WSI_QUERY_HDR_CAPS,
   NPT_UNIX_WSI_SET_HDR_METADATA,
   npt_unix_funcs_count,
};

#endif /* NPT_UNIXLIB_H */
