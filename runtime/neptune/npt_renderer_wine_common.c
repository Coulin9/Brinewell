/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifdef __WINE__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

#include <string.h>

#include "npt_common.h"
#include "npt_renderer.h"
#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include "virtio-gpu/virgl_hw.h"

void npt_wine_unixlib_close(int fd)
{
   struct npt_unix_close_params p = { .fd = fd };
   npt_wine_unix_call(npt_unix_close, &p);
}

int npt_wine_unixlib_dup(int fd)
{
   if (fd < 0)
      return -1;
   if (npt_unixlib_ensure_init() != 0)
      return -1;
   struct npt_unix_dup_params p = { .fd = fd, .result = -1 };
   if (npt_wine_unix_call(npt_unix_dup, &p) != 0)
      return -1;
   return p.result;
}

void npt_wine_unixlib_munmap(void *addr, size_t size)
{
   struct npt_unix_munmap_params p = { .addr = addr, .size = size };
   npt_wine_unix_call(npt_unix_munmap, &p);
}

static uint32_t
wine_resolve_x11_window(HWND hwnd, HWND *out_owner,
                        int16_t *out_x_off, int16_t *out_y_off)
{
   *out_owner = NULL;
   *out_x_off = 0;
   *out_y_off = 0;
   if (!hwnd)
      return 0;

   HWND desktop = GetDesktopWindow();
   HWND cur = hwnd;
   uint32_t x11_window = 0;
   for (unsigned hop = 0; hop < 64 && cur; hop++) {
      HANDLE prop = GetPropW(cur, L"__wine_x11_whole_window");
      uint32_t xid = (uint32_t)(uintptr_t)prop;
      if (xid) {
         x11_window = xid;
         *out_owner = cur;
         break;
      }
      if ((GetWindowLongW(cur, GWL_STYLE) & WS_CHILD) == 0)
         break;
      HWND parent = GetAncestor(cur, GA_PARENT);
      if (!parent || parent == cur || parent == desktop)
         break;
      cur = parent;
   }

   if (x11_window && *out_owner != hwnd) {
      POINT pt = { 0, 0 };
      MapWindowPoints(hwnd, *out_owner, &pt, 1);
      if (pt.x < INT16_MIN)
         pt.x = INT16_MIN;
      else if (pt.x > INT16_MAX)
         pt.x = INT16_MAX;
      if (pt.y < INT16_MIN)
         pt.y = INT16_MIN;
      else if (pt.y > INT16_MAX)
         pt.y = INT16_MAX;
      *out_x_off = (int16_t)pt.x;
      *out_y_off = (int16_t)pt.y;
   }

   return x11_window;
}

static int unixlib_wsi_init(uint32_t num_images, const int *fds,
                            const uint32_t *strides, uint32_t virgl_format,
                            uint32_t width, uint32_t height, HWND hwnd,
                            uint64_t *out_wsi, bool *frame_stats_enabled,
                            uint32_t colorspace,
                            const struct npt_unix_hdr10_metadata *hdr_meta)
{
   if (num_images > NPT_WSI_MAX_IMAGES) {
      npt_log("unixlib_wsi_init: num_images %u exceeds cap %u",
              num_images, (unsigned)NPT_WSI_MAX_IMAGES);
      return -1;
   }

   /* Wine's winex11.drv publishes the X11 Window XID for top-level Wine
    * HWNDs under this property (set in winex11 window.c: NtUserSetProp
    * with whole_window_prop).  WS_CHILD windows (including cross-process
    * child HWNDs such as CEF/ANGLE swapchains) have no X11 window of their
    * own; walk upward through WS_CHILD ancestors to the owning top-level
    * HWND and record the client-area offset. */
   HWND owner = NULL;
   int16_t x_off = 0, y_off = 0;
   uint32_t x11_window = wine_resolve_x11_window(hwnd, &owner, &x_off, &y_off);
   if (!x11_window) {
      npt_log("unixlib_wsi_init: no X11 window for hwnd %p", (void *)hwnd);
      return -1;
   }
   if (owner != hwnd) {
      npt_log("unixlib_wsi_init: child hwnd %p -> ancestor hwnd %p x11=0x%x off=(%d,%d)",
              (void *)hwnd, (void *)owner, x11_window, (int)x_off, (int)y_off);
   }

   struct npt_unix_wsi_init_params p;
   memset(&p, 0, sizeof(p));
   p.x11_window = x11_window;
   p.num_images = num_images;
   for (uint32_t i = 0; i < p.num_images; i++) {
      p.fds[i] = fds[i];
      p.strides[i] = strides[i];
   }
   p.virgl_format = virgl_format;
   p.width = width;
   p.height = height;
   p.colorspace = colorspace;
   if (hdr_meta)
      p.hdr_meta = *hdr_meta;
   p.x_off = x_off;
   p.y_off = y_off;
   npt_wine_unix_call(npt_unix_wsi_init, &p);
   *out_wsi = p.wsi;
   *frame_stats_enabled = p.frame_stats_enabled != 0;
   return p.result;
}

static int unixlib_wsi_present(uint64_t wsi, uint32_t image_index,
                               int wait_fence_fd, uint64_t present_time_ns)
{
   struct npt_unix_wsi_present_params p;
   memset(&p, 0, sizeof(p));
   p.wsi = wsi;
   p.image_index = image_index;
   p.wait_fence_fd = wait_fence_fd;
   p.present_time_ns = present_time_ns;
   npt_wine_unix_call(npt_unix_wsi_present, &p);
   /* Close the fd unless xcb took it via SCM_RIGHTS. */
   if (wait_fence_fd >= 0 && !p.wait_fence_consumed)
      npt_wine_unixlib_close(wait_fence_fd);
   return p.result;
}

static void unixlib_wsi_destroy(uint64_t wsi)
{
   struct npt_unix_wsi_destroy_params p = { .wsi = wsi };
   npt_wine_unix_call(npt_unix_wsi_destroy, &p);
}

static void unixlib_wsi_cancel(uint64_t wsi)
{
   struct npt_unix_wsi_cancel_params p = { .wsi = wsi };
   npt_wine_unix_call(npt_unix_wsi_cancel, &p);
}

static int unixlib_wsi_drain(uint64_t wsi, uint32_t timeout_ms,
                             uint32_t out_released_count[NPT_WSI_MAX_IMAGES])
{
   struct npt_unix_wsi_drain_params p;
   memset(&p, 0, sizeof(p));
   p.wsi = wsi;
   p.timeout_ms = timeout_ms;
   npt_wine_unix_call(npt_unix_wsi_drain, &p);
   if (out_released_count) {
      for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
         out_released_count[i] = p.released_count[i];
   }
   return p.result;
}

static bool
wine_wsi_init(struct npt_renderer *r, uint32_t num_images,
              const int *fds, const uint32_t *strides,
              uint32_t virgl_format, uint32_t width, uint32_t height,
              uint64_t hwnd, uint64_t *out_wsi, bool *frame_stats_enabled,
                            uint32_t colorspace,
                            const struct npt_unix_hdr10_metadata *hdr_meta)
{
   (void)r;
   return unixlib_wsi_init(num_images, fds, strides, virgl_format,
                           width, height, (HWND)(uintptr_t)hwnd,
                           out_wsi, frame_stats_enabled, colorspace, hdr_meta) == 0;
}

static int
wine_wsi_present(struct npt_renderer *r, uint64_t wsi, uint32_t image_index,
                 int wait_fence_fd, uint64_t present_time_ns)
{
   (void)r;
   return unixlib_wsi_present(wsi, image_index, wait_fence_fd, present_time_ns);
}

static void
wine_wsi_destroy(struct npt_renderer *r, uint64_t wsi)
{
   (void)r;
   unixlib_wsi_destroy(wsi);
}

static void
wine_wsi_cancel(struct npt_renderer *r, uint64_t wsi)
{
   (void)r;
   unixlib_wsi_cancel(wsi);
}

static int
wine_wsi_drain(struct npt_renderer *r, uint64_t wsi,
               uint32_t timeout_ms,
               uint32_t out_released_count[NPT_WSI_MAX_IMAGES])
{
   (void)r;
   return unixlib_wsi_drain(wsi, timeout_ms, out_released_count);
}

void npt_wine_install_wsi_ops(struct npt_renderer_ops *ops)
{
   ops->wsi_init = wine_wsi_init;
   ops->wsi_present = wine_wsi_present;
   ops->wsi_destroy = wine_wsi_destroy;
   ops->wsi_cancel = wine_wsi_cancel;
   ops->wsi_drain = wine_wsi_drain;
}

#endif /* __WINE__ */
