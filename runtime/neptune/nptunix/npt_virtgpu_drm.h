/* SPDX-License-Identifier: MIT */
#ifndef NPT_VIRTGPU_DRM_H
#define NPT_VIRTGPU_DRM_H

#include "drm-uapi/virtgpu_drm.h"

/* Brinewell virtio-gpu guest kernel extensions used by the Neptune unixlib. */
struct drm_virtgpu_counter_wait {
   __u32 bo_handle;
   __u32 counter_offset;
   __u32 waiters_offset;
   __u32 abort_offset;
   __u64 value;
   __s64 timeout_ns;
};

struct drm_virtgpu_doorbell_create {
   __s32 fd;
   __u32 pad;
};

#define DRM_IOCTL_VIRTGPU_COUNTER_WAIT \
   DRM_IOWR(DRM_COMMAND_BASE + 0x0c, struct drm_virtgpu_counter_wait)
#define DRM_IOCTL_VIRTGPU_DOORBELL_CREATE \
   DRM_IOWR(DRM_COMMAND_BASE + 0x0d, struct drm_virtgpu_doorbell_create)

#endif
