/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * X11 DRI3/Present WSI for nptunix.so.
 *
 * Explicit DRI3/Present timeline synchronization is preferred; legacy
 * servers use a cancellable CPU wait for the per-Present sync_file.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include <xf86drm.h>

#include "npt_unix_internal.h"
#include "npt_unixlib.h"
#include "npt_wsi_protocol.h"
#include "brinewell_frame_stats.h"
#include "brinewell_x11_focus.h"

#ifndef NPT_WSI_DIAGNOSTIC_DEFAULT
#define NPT_WSI_DIAGNOSTIC_DEFAULT 0
#endif

/* One per swapchain: its own X connection, window, pixmaps and Present
 * event queue, so swapchains never see each other's images or events. */
struct npt_unix_wsi {
   xcb_connection_t *conn;
   struct npt_wayland_hdr *wayland;
   int conn_fd;
   xcb_window_t window;
   uint32_t width, height, num_images;
   int16_t x_off, y_off;
   int drm_fd;
   bool explicit_sync, diagnostics;
   int cancel_pipe[2], wake_pipe[2];
   pthread_mutex_t mtx, drain_mtx;
   _Atomic int cancelled, terminal_err, retired;
   uint32_t serial;
   uint64_t idle_count, complete_count, release_count;
   struct brinewell_frame_stats frame_stats;
   struct brinewell_x11_focus frame_focus;
   xcb_present_event_t present_eid;
   xcb_special_event_t *special_event;
   struct npt_wsi_image {
      int fd;
      xcb_pixmap_t pixmap;
      uint32_t acquire_handle, release_handle;
      uint32_t acquire_xid, release_xid;
      uint64_t acquire_point, release_point;
      uint32_t pending_serial, pending_seq;
      bool acq_done, rel_done;
      enum { NPT_WSI_IMG_IDLE, NPT_WSI_IMG_SUBMITTING,
             NPT_WSI_IMG_PENDING } state;
   } images[NPT_WSI_MAX_IMAGES];
};

#include "npt_unix_wayland_hdr.h"

static struct npt_unix_wsi *
wsi_from_handle(uint64_t handle)
{
   return (struct npt_unix_wsi *)(uintptr_t)handle;
}

/* Wine can promote an override-redirect fullscreen window to a managed
 * window without changing its geometry. No ConfigureNotify follows that
 * attribute change, so gamescope keeps the old override_redirect value and
 * omits the game from Steam's window list. Report the current server state
 * once when binding the presentation window; leave focus and stacking to
 * Steam, including when the user subsequently opens its menus. */
static void
refresh_gamescope_window(struct npt_unix_wsi *w)
{
   const char *name = "GAMESCOPE_XWAYLAND_SERVER_ID";
   xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(
      w->conn, xcb_intern_atom(w->conn, true, strlen(name), name), NULL);
   if (!atom)
      return;
   xcb_atom_t marker = atom->atom;
   free(atom);
   if (!marker)
      return;

   xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
      w->conn, xcb_get_geometry(w->conn, w->window), NULL);
   if (!geometry)
      return;
   xcb_get_property_reply_t *property = xcb_get_property_reply(
      w->conn, xcb_get_property(w->conn, false, geometry->root, marker,
                                XCB_ATOM_CARDINAL, 0, 1), NULL);
   bool gamescope = property && property->format == 32 &&
                    property->type == XCB_ATOM_CARDINAL &&
                    property->value_len == 1;
   free(property);
   if (!gamescope) {
      free(geometry);
      return;
   }

   xcb_get_window_attributes_reply_t *attributes = xcb_get_window_attributes_reply(
      w->conn, xcb_get_window_attributes(w->conn, w->window), NULL);
   if (!attributes || attributes->override_redirect ||
       attributes->map_state != XCB_MAP_STATE_VIEWABLE) {
      free(attributes);
      free(geometry);
      return;
   }
   free(attributes);

   xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
      w->conn, xcb_query_tree(w->conn, geometry->root), NULL);
   if (tree) {
      xcb_window_t *children = xcb_query_tree_children(tree);
      int count = xcb_query_tree_children_length(tree);
      for (int i = 0; i < count; i++) {
         if (children[i] != w->window)
            continue;
         /* QueryTree lists siblings bottom-to-top. ConfigureNotify names
          * the sibling immediately below this window, despite "above". */
         xcb_configure_notify_event_t event = {
            .response_type = XCB_CONFIGURE_NOTIFY,
            .event = geometry->root,
            .window = w->window,
            .above_sibling = i > 0 ? children[i - 1] : XCB_NONE,
            .x = geometry->x,
            .y = geometry->y,
            .width = geometry->width,
            .height = geometry->height,
            .border_width = geometry->border_width,
            .override_redirect = false,
         };
         /* SendEvent reads a full 32-byte wire event; this struct is 28. */
         char wire[32] = { 0 };
         memcpy(wire, &event, sizeof(event));
         xcb_send_event(w->conn, false, geometry->root,
                        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, wire);
         fprintf(stderr, "neptune unixlib wsi: refreshed gamescope window 0x%x "
                         "(%ux%u, managed)\n", w->window,
                 geometry->width, geometry->height);
         break;
      }
   }
   free(tree);
   free(geometry);
}

static void
free_syncobjs(struct npt_unix_wsi *w)
{
   for (uint32_t i = 0; i < w->num_images; i++) {
      struct npt_wsi_image *img = &w->images[i];
      if (w->conn && !xcb_connection_has_error(w->conn)) {
         if (img->acquire_xid)
            npt_xcb_dri3_free_syncobj(w->conn, img->acquire_xid);
         if (img->release_xid)
            npt_xcb_dri3_free_syncobj(w->conn, img->release_xid);
      }
      if (w->drm_fd >= 0) {
         if (img->acquire_handle)
            drmSyncobjDestroy(w->drm_fd, img->acquire_handle);
         if (img->release_handle)
            drmSyncobjDestroy(w->drm_fd, img->release_handle);
      }
      img->acquire_xid = img->release_xid = 0;
      img->acquire_handle = img->release_handle = 0;
   }
   if (w->drm_fd >= 0)
      close(w->drm_fd);
   w->drm_fd = -1;
   w->explicit_sync = false;
}

static void
wsi_free(struct npt_unix_wsi *w)
{
   npt_wl_free(w);
   brinewell_frame_stats_finish(&w->frame_stats);
   if (w->conn)
      brinewell_x11_focus_finish(w->conn, &w->frame_focus);
   free_syncobjs(w);
   if (w->conn) {
      if (!xcb_connection_has_error(w->conn)) {
         for (uint32_t i = 0; i < w->num_images; i++)
            if (w->images[i].pixmap)
               xcb_free_pixmap(w->conn, w->images[i].pixmap);
      }
      if (w->special_event)
         xcb_unregister_for_special_event(w->conn, w->special_event);
      xcb_disconnect(w->conn);
   }
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
      if (w->images[i].fd >= 0)
         close(w->images[i].fd);
   for (int i = 0; i < 2; i++) {
      if (w->cancel_pipe[i] >= 0) close(w->cancel_pipe[i]);
      if (w->wake_pipe[i] >= 0) close(w->wake_pipe[i]);
   }
   pthread_mutex_destroy(&w->drain_mtx);
   pthread_mutex_destroy(&w->mtx);
   free(w);
}

static bool
timeline_capable(int fd)
{
   uint64_t cap = 0;
   return fd >= 0 && drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap;
}

static int
open_timeline_device(struct npt_unix_wsi *w)
{
   xcb_dri3_open_reply_t *reply = xcb_dri3_open_reply(
      w->conn, xcb_dri3_open(w->conn, w->window, 0), NULL);
   int fd = -1;
   if (reply) {
      int *fds = xcb_dri3_open_reply_fds(w->conn, reply);
      for (unsigned int i = 0; i < reply->nfd; i++) {
         int candidate = fds[i];
         if (fd < 0 && fcntl(candidate, F_SETFD, FD_CLOEXEC) == 0 &&
             timeline_capable(candidate))
            fd = candidate;
         else
            close(candidate);
      }
      free(reply);
   }
   if (fd >= 0)
      return fd;
   fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (!timeline_capable(fd)) {
      if (fd >= 0) close(fd);
      return -1;
   }
   return fd;
}

static bool
import_syncobj(struct npt_unix_wsi *w, uint32_t *handle, uint32_t *xid)
{
   int fd = -1;
   if (drmSyncobjCreate(w->drm_fd, 0, handle) != 0 ||
       drmSyncobjHandleToFD(w->drm_fd, *handle, &fd) != 0)
      return false;
   uint32_t id = xcb_generate_id(w->conn);
   xcb_generic_error_t *error = xcb_request_check(
      w->conn, npt_xcb_dri3_import_syncobj_checked(w->conn, id, w->window, fd));
   if (error) {
      free(error);
      return false;
   }
   if (xcb_connection_has_error(w->conn))
      return false;
   *xid = id;
   return true;
}

NTSTATUS npt_do_wsi_init(void *args)
{
   struct npt_unix_wsi_init_params *p = args;
   p->result = -1;
   p->wsi = 0;
   p->frame_stats_enabled = 0;
   if (!p->num_images || p->num_images > NPT_WSI_MAX_IMAGES ||
       !p->x11_window || !p->width || p->width > UINT16_MAX ||
       !p->height || p->height > UINT16_MAX)
      return STATUS_SUCCESS;
   bool wayland = p->x_off == 0 && p->y_off == 0 &&
                  (p->virgl_format == 8 || p->virgl_format == 94 ||
                   p->colorspace == NPT_DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                   p->colorspace == NPT_DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
   for (uint32_t i = 0; i < p->num_images; i++)
      if (p->fds[i] < 0 || p->strides[i] < (uint64_t)p->width * (p->virgl_format == 94 ? 8 : 4) ||
          (!wayland && (p->strides[i] > UINT16_MAX ||
           (uint64_t)p->strides[i] * p->height > UINT32_MAX)))
         return STATUS_SUCCESS;

   struct npt_unix_wsi *w = calloc(1, sizeof(*w));
   if (!w)
      return STATUS_SUCCESS;
   w->frame_stats.fd = -1;
   w->conn_fd = w->drm_fd = -1;
   w->cancel_pipe[0] = w->cancel_pipe[1] = -1;
   w->wake_pipe[0] = w->wake_pipe[1] = -1;
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
      w->images[i].fd = -1;
   if (pthread_mutex_init(&w->mtx, NULL) != 0) {
      free(w);
      return STATUS_SUCCESS;
   }
   if (pthread_mutex_init(&w->drain_mtx, NULL) != 0) {
      pthread_mutex_destroy(&w->mtx);
      free(w);
      return STATUS_SUCCESS;
   }
   w->window = p->x11_window;
   w->width = p->width;
   w->height = p->height;
   w->num_images = p->num_images;
   w->x_off = p->x_off;
   w->y_off = p->y_off;
   const char *diagnostics = getenv("NPT_WSI_DIAGNOSTICS");
   w->diagnostics = diagnostics ? strcmp(diagnostics, "0") != 0
                                : NPT_WSI_DIAGNOSTIC_DEFAULT;
   if (pipe2(w->cancel_pipe, O_CLOEXEC | O_NONBLOCK) ||
       pipe2(w->wake_pipe, O_CLOEXEC | O_NONBLOCK))
      goto fail;
   w->conn = xcb_connect(NULL, NULL);
   if (!w->conn || xcb_connection_has_error(w->conn))
      goto fail;
   w->conn_fd = xcb_get_file_descriptor(w->conn);
   if (w->conn_fd < 0)
      goto fail;
   if (wayland) {
      if (!npt_wl_init(w, p))
         goto fail;
      brinewell_frame_stats_init(&w->frame_stats, "neptune", w->width, w->height);
      if (w->frame_stats.fd >= 0)
         brinewell_x11_focus_init(w->conn, &w->frame_focus, w->window);
      p->frame_stats_enabled = w->frame_stats.fd >= 0;
      p->wsi = (uint64_t)(uintptr_t)w;
      p->result = 0;
      return STATUS_SUCCESS;
   }
   xcb_dri3_query_version_reply_t *dri3 = xcb_dri3_query_version_reply(
      w->conn, xcb_dri3_query_version(w->conn, 1, 4), NULL);
   xcb_present_query_version_reply_t *pres = xcb_present_query_version_reply(
      w->conn, xcb_present_query_version(w->conn, 1, 4), NULL);
   if (!dri3 || !pres || (pres->major_version == 1 && pres->minor_version < 2) ||
       pres->major_version < 1) {
      free(dri3);
      free(pres);
      goto fail;
   }
   bool versions = (dri3->major_version > 1 ||
                    (dri3->major_version == 1 && dri3->minor_version >= 4)) &&
                   (pres->major_version > 1 ||
                    (pres->major_version == 1 && pres->minor_version >= 4));
   free(dri3);
   free(pres);
   xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(
      w->conn, xcb_get_geometry(w->conn, w->window), NULL);
   uint8_t pixmap_depth = (geom && geom->depth == 32) ? 32 : 24;
   free(geom);
   for (uint32_t i = 0; i < p->num_images; i++) {
      struct npt_wsi_image *img = &w->images[i];
      img->fd = dup(p->fds[i]);
      if (img->fd < 0)
         goto fail;
      int xcb_fd = dup(img->fd);
      if (xcb_fd < 0)
         goto fail;
      xcb_pixmap_t pixmap = xcb_generate_id(w->conn);
      xcb_generic_error_t *error = xcb_request_check(w->conn,
         xcb_dri3_pixmap_from_buffer_checked(w->conn, pixmap, w->window,
            p->strides[i] * p->height, p->width, p->height, p->strides[i],
            pixmap_depth, 32, xcb_fd));
      if (error) {
         free(error);
         goto fail;
      }
      if (xcb_connection_has_error(w->conn))
         goto fail;
      img->pixmap = pixmap;
   }

   const char *explicit_env = getenv("NPT_WSI_EXPLICIT_SYNC");
   bool caps_ok = false;
   uint32_t caps = 0;
   const char *reason = NULL;
   if (explicit_env && strcmp(explicit_env, "0") == 0)
      reason = "disabled by NPT_WSI_EXPLICIT_SYNC";
   else if (!versions)
      reason = "DRI3 or Present 1.4 unavailable";
   else {
      caps = npt_xcb_present_query_capabilities_sync(w->conn, w->window, &caps_ok);
      if (!caps_ok || !(caps & NPT_XCB_PRESENT_CAPABILITY_SYNCOBJ))
         reason = "Present syncobj capability unavailable";
   }
   if (!reason) {
      w->drm_fd = open_timeline_device(w);
      if (w->drm_fd < 0)
         reason = "DRM timeline syncobj unavailable";
   }
   if (!reason) {
      for (uint32_t i = 0; i < w->num_images; i++) {
         struct npt_wsi_image *img = &w->images[i];
         if (!import_syncobj(w, &img->acquire_handle, &img->acquire_xid) ||
             !import_syncobj(w, &img->release_handle, &img->release_xid)) {
            reason = "syncobj creation or import failed";
            break;
         }
      }
   }
   if (reason) {
      fprintf(stderr, "neptune unixlib wsi: %s; using CPU fence wait\n", reason);
      free_syncobjs(w);
   } else {
      w->explicit_sync = true;
   }
   w->present_eid = xcb_generate_id(w->conn);
   xcb_present_select_input(w->conn, w->present_eid, w->window,
      XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
      (w->explicit_sync ? 0 : XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY));
   w->special_event = xcb_register_for_special_xge(
      w->conn, &xcb_present_id, w->present_eid, NULL);
   if (!w->special_event)
      goto fail;
   refresh_gamescope_window(w);
   xcb_flush(w->conn);
   if (xcb_connection_has_error(w->conn))
      goto fail;
   brinewell_frame_stats_init(&w->frame_stats, "neptune", w->width, w->height);
   if (w->frame_stats.fd >= 0)
      brinewell_x11_focus_init(w->conn, &w->frame_focus, w->window);
   p->frame_stats_enabled = w->frame_stats.fd >= 0;
   p->wsi = (uint64_t)(uintptr_t)w;
   p->result = 0;
   return STATUS_SUCCESS;
fail:
   wsi_free(w);
   return STATUS_SUCCESS;
}

static uint64_t
monotonic_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void
harvest_x_events(struct npt_unix_wsi *w, uint32_t *counts)
{
   xcb_generic_event_t *ev;
   while ((ev = xcb_poll_for_special_event(w->conn, w->special_event))) {
      xcb_present_generic_event_t *pe = (void *)ev;
      if (pe->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY && !w->explicit_sync) {
         xcb_present_idle_notify_event_t *idle = (void *)ev;
         int image = -1;
         for (uint32_t i = 0; i < w->num_images; i++) {
            if (w->images[i].pixmap == idle->pixmap) {
               image = (int)i;
               if (counts)
                  counts[i]++;
            }
         }
         w->idle_count++;
         if (w->diagnostics && (w->idle_count <= 8 || w->idle_count % 120 == 0))
            fprintf(stderr, "neptune unixlib wsi: idle count=%llu serial=%u image=%d\n",
                    (unsigned long long)w->idle_count, idle->serial, image);
      } else if (pe->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
         xcb_present_complete_notify_event_t *complete = (void *)ev;
         w->complete_count++;
         if (w->diagnostics && (w->complete_count <= 8 || w->complete_count % 120 == 0))
            fprintf(stderr, "neptune unixlib wsi: complete count=%llu serial=%u mode=%u\n",
                    (unsigned long long)w->complete_count, complete->serial, complete->mode);
      }
      free(ev);
   }
   while ((ev = xcb_poll_for_event(w->conn))) {
      if (ev->response_type == 0) {
         xcb_generic_error_t *err = (void *)ev;
         fprintf(stderr, "neptune unixlib wsi: X error code=%u major=%u minor=%u "
                 "resource=0x%x sequence=%u\n", err->error_code, err->major_code,
                 err->minor_code, err->resource_id, err->full_sequence);
         atomic_store(&w->terminal_err, -2);
      }
      free(ev);
   }
   if (xcb_connection_has_error(w->conn))
      atomic_store(&w->terminal_err, -2);
}

static const uint32_t kPresentOptions =
   XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_COPY;

static void
wsi_record_frame(struct npt_unix_wsi *w, uint64_t present_time_ns)
{
   if (w->frame_stats.fd < 0 || !present_time_ns)
      return;
   bool focused = brinewell_x11_focus_update(w->conn, &w->frame_focus,
                                             present_time_ns);
   brinewell_frame_stats_record(&w->frame_stats, present_time_ns, focused);
}

NTSTATUS npt_do_wsi_present(void *args)
{
   struct npt_unix_wsi_present_params *p = args;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   p->result = -1;
   p->wait_fence_consumed = 0;
   if (!w || p->image_index >= w->num_images || atomic_load(&w->cancelled))
      return STATUS_SUCCESS;
   if (atomic_load(&w->terminal_err)) {
      p->result = -2;
      return STATUS_SUCCESS;
   }
   if (w->wayland) {
      npt_wl_present(w, p);
      if (p->result == 0)
         wsi_record_frame(w, p->present_time_ns);
      return STATUS_SUCCESS;
   }
   struct npt_wsi_image *img = &w->images[p->image_index];
   pthread_mutex_lock(&w->mtx);
   if (atomic_load(&w->cancelled) || atomic_load(&w->terminal_err) ||
       img->state != NPT_WSI_IMG_IDLE) {
      if (!atomic_load(&w->cancelled) && atomic_load(&w->terminal_err))
         p->result = -2;
      pthread_mutex_unlock(&w->mtx);
      return STATUS_SUCCESS;
   }
   if (w->explicit_sync &&
       (img->acquire_point == UINT64_MAX || img->release_point == UINT64_MAX)) {
      atomic_store(&w->terminal_err, -2);
      p->result = -2;
      pthread_mutex_unlock(&w->mtx);
      return STATUS_SUCCESS;
   }
   img->state = NPT_WSI_IMG_SUBMITTING;
   uint64_t acq_pt = img->acquire_point + 1;
   uint64_t rel_pt = img->release_point + 1;
   uint32_t serial = ++w->serial;
   pthread_mutex_unlock(&w->mtx);

   if (w->explicit_sync) {
      int rc;
      if (p->wait_fence_fd >= 0) {
         uint32_t tmp = 0;
         rc = drmSyncobjCreate(w->drm_fd, 0, &tmp);
         if (!rc)
            rc = drmSyncobjImportSyncFile(w->drm_fd, tmp, p->wait_fence_fd);
         close(p->wait_fence_fd);
         p->wait_fence_consumed = 1;
         if (!rc)
            rc = drmSyncobjTransfer(w->drm_fd, img->acquire_handle, acq_pt,
                                    tmp, 0, 0);
         if (tmp)
            drmSyncobjDestroy(w->drm_fd, tmp);
      } else {
         rc = drmSyncobjTimelineSignal(w->drm_fd, &img->acquire_handle, &acq_pt, 1);
      }
      if (rc)
         goto reject;
      if (atomic_load(&w->cancelled))
         goto reject;
      if (atomic_load(&w->terminal_err)) {
         p->result = -2;
         goto reject;
      }
      xcb_void_cookie_t cookie = npt_xcb_present_pixmap_synced(
         w->conn, w->window, img->pixmap, serial, 0, 0, w->x_off, w->y_off,
         XCB_NONE, img->acquire_xid, img->release_xid, acq_pt, rel_pt,
         kPresentOptions, 0, 0, 0);
      if (w->diagnostics && (serial <= 8 || serial % 120 == 0))
         fprintf(stderr, "neptune unixlib wsi: synced Present serial=%u image=%u acq=%llu rel=%llu fenced=%d\n",
                 serial, p->image_index, (unsigned long long)acq_pt,
                 (unsigned long long)rel_pt, p->wait_fence_fd >= 0);
      xcb_flush(w->conn);
      if (xcb_connection_has_error(w->conn)) {
         atomic_store(&w->terminal_err, -2);
         p->result = -2;
         goto reject;
      }
      pthread_mutex_lock(&w->mtx);
      img->acquire_point = acq_pt;
      img->release_point = rel_pt;
      img->acq_done = p->wait_fence_fd < 0;
      img->rel_done = false;
      img->pending_serial = serial;
      img->pending_seq = cookie.sequence;
      img->state = NPT_WSI_IMG_PENDING;
      pthread_mutex_unlock(&w->mtx);
      (void)write(w->wake_pipe[1], "w", 1);
      p->result = 0;
      wsi_record_frame(w, p->present_time_ns);
      return STATUS_SUCCESS;
   }

   int fence_poll = 0;
   int fence_revents = 0;
   bool poll_failed = false;
   if (p->wait_fence_fd >= 0) {
      struct pollfd fds[2] = {
         { .fd = p->wait_fence_fd, .events = POLLIN },
         { .fd = w->cancel_pipe[0], .events = POLLIN },
      };
      uint64_t deadline = monotonic_ns() + 30000000000ull;
      int rc;
      do {
         uint64_t now = monotonic_ns();
         int ms = now >= deadline ? 0 : (int)((deadline - now + 999999) / 1000000);
         rc = poll(fds, 2, ms);
      } while (rc < 0 && errno == EINTR);
      close(p->wait_fence_fd);
      p->wait_fence_consumed = 1;
      fence_poll = rc;
      fence_revents = fds[0].revents;
      poll_failed = atomic_load(&w->cancelled) || rc <= 0 || fds[1].revents ||
                    !(fds[0].revents & POLLIN) ||
                    (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL));
      if (poll_failed) {
         if (w->diagnostics)
            fprintf(stderr, "neptune unixlib wsi: legacy Present serial=%u image=%u fence_poll=%d revents=0x%x\n",
                    serial, p->image_index, fence_poll, fence_revents);
         goto reject;
      }
   }
   xcb_present_pixmap(w->conn, w->window, img->pixmap, serial, 0, 0,
                      w->x_off, w->y_off,
                      XCB_NONE, XCB_NONE, XCB_NONE, kPresentOptions,
                      0, 0, 0, 0, NULL);
   if (w->diagnostics && (serial <= 8 || serial % 120 == 0 || poll_failed))
      fprintf(stderr, "neptune unixlib wsi: legacy Present serial=%u image=%u fence_poll=%d revents=0x%x\n",
              serial, p->image_index, fence_poll, fence_revents);
   xcb_flush(w->conn);
   if (xcb_connection_has_error(w->conn)) {
      atomic_store(&w->terminal_err, -2);
      p->result = -2;
   } else {
      p->result = 0;
      wsi_record_frame(w, p->present_time_ns);
   }
reject:
   pthread_mutex_lock(&w->mtx);
   img->state = NPT_WSI_IMG_IDLE;
   pthread_mutex_unlock(&w->mtx);
   return STATUS_SUCCESS;
}

static int
check_point(struct npt_unix_wsi *w, uint32_t handle, uint64_t point)
{
   if (drmSyncobjTimelineWait(w->drm_fd, &handle, &point, 1, 0,
       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
       NULL) == 0)
      return 1;
   if (errno == ETIME || errno == EINTR)
      return 0;
   atomic_store(&w->terminal_err, -2);
   return -1;
}

NTSTATUS npt_do_wsi_drain(void *args)
{
   struct npt_unix_wsi_drain_params *p = args;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   memset(p->released_count, 0, sizeof(p->released_count));
   p->result = -1;
   if (!w)
      return STATUS_SUCCESS;
   if (w->wayland) {
      npt_wl_drain(w, p);
      return STATUS_SUCCESS;
   }
   pthread_mutex_lock(&w->drain_mtx);
   uint64_t deadline = monotonic_ns() + (uint64_t)p->timeout_ms * 1000000ull;
   for (;;) {
      if (atomic_load(&w->cancelled))
         break;
      harvest_x_events(w, w->explicit_sync ? NULL : p->released_count);
      if (atomic_load(&w->terminal_err)) {
         p->result = -2;
         break;
      }
      bool any = false;
      uint32_t handles[NPT_WSI_MAX_IMAGES * 2];
      uint64_t points[NPT_WSI_MAX_IMAGES * 2];
      unsigned int count = 0;
      if (w->explicit_sync) {
         for (uint32_t i = 0; i < w->num_images; i++) {
            struct npt_wsi_image *img = &w->images[i];
            pthread_mutex_lock(&w->mtx);
            bool pending = img->state == NPT_WSI_IMG_PENDING;
            uint32_t serial = img->pending_serial;
            uint64_t acq = img->acquire_point, rel = img->release_point;
            bool acq_done = img->acq_done, rel_done = img->rel_done;
            pthread_mutex_unlock(&w->mtx);
            if (!pending)
               continue;
            int a = acq_done ? 1 : check_point(w, img->acquire_handle, acq);
            int r = rel_done ? 1 : check_point(w, img->release_handle, rel);
            if (a < 0 || r < 0)
               break;
            pthread_mutex_lock(&w->mtx);
            if (img->state == NPT_WSI_IMG_PENDING && img->pending_serial == serial) {
               img->acq_done |= a == 1;
               img->rel_done |= r == 1;
               if (img->acq_done && img->rel_done) {
                  img->state = NPT_WSI_IMG_IDLE;
                  p->released_count[i]++;
                  any = true;
                  w->release_count++;
                  if (w->diagnostics &&
                      (w->release_count <= 8 || w->release_count % 120 == 0))
                     fprintf(stderr, "neptune unixlib wsi: timeline release count=%llu serial=%u image=%u acq=%llu rel=%llu\n",
                             (unsigned long long)w->release_count, serial, i,
                             (unsigned long long)acq, (unsigned long long)rel);
               } else {
                  if (!img->acq_done) {
                     handles[count] = img->acquire_handle;
                     points[count++] = acq;
                  }
                  if (!img->rel_done) {
                     handles[count] = img->release_handle;
                     points[count++] = rel;
                  }
               }
            }
            pthread_mutex_unlock(&w->mtx);
         }
         if (atomic_load(&w->terminal_err)) {
            p->result = -2;
            break;
         }
      } else {
         for (uint32_t i = 0; i < w->num_images; i++)
            any |= p->released_count[i] != 0;
      }
      if (any) {
         p->result = 0;
         break;
      }
      uint64_t now = monotonic_ns();
      if (now >= deadline) {
         p->result = 0;
         break;
      }
      uint64_t remaining = deadline - now;
      if (w->explicit_sync && count) {
         uint64_t slice = remaining < 10000000ull ? remaining : 10000000ull;
         if (drmSyncobjTimelineWait(w->drm_fd, handles, points, count, now + slice,
             DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) != 0 &&
             errno != ETIME && errno != EINTR) {
            atomic_store(&w->terminal_err, -2);
            p->result = -2;
            break;
         }
      } else {
         struct pollfd fds[3] = {
            { .fd = w->conn_fd, .events = POLLIN },
            { .fd = w->cancel_pipe[0], .events = POLLIN },
            { .fd = w->wake_pipe[0], .events = POLLIN },
         };
         uint64_t ms = (remaining + 999999) / 1000000;
         if (w->explicit_sync && ms > 25) ms = 25;
         if (ms > INT32_MAX) ms = INT32_MAX;
         int rc = poll(fds, w->explicit_sync ? 3 : 2, (int)ms);
         if (atomic_load(&w->cancelled) || fds[1].revents)
            break;
         if ((rc < 0 && errno != EINTR) ||
             (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            atomic_store(&w->terminal_err, -2);
            p->result = -2;
            break;
         }
         if (w->explicit_sync && (fds[2].revents & POLLIN)) {
            char buffer[128];
            while (read(w->wake_pipe[0], buffer, sizeof(buffer)) > 0) {}
         }
      }
   }
   pthread_mutex_unlock(&w->drain_mtx);
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_cancel(void *args)
{
   struct npt_unix_wsi_cancel_params *p = args;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   if (w) {
      atomic_store(&w->cancelled, 1);
      (void)write(w->cancel_pipe[1], "c", 1);
      if (w->conn_fd >= 0)
         shutdown(w->conn_fd, SHUT_RDWR);
   }
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_destroy(void *args)
{
   struct npt_unix_wsi_destroy_params *p = args;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   if (w) {
      if (w->wayland) {
         atomic_store(&w->cancelled, 1);
         (void)write(w->cancel_pipe[1], "c", 1);
         pthread_mutex_lock(&w->drain_mtx);
         pthread_mutex_unlock(&w->drain_mtx);
      }
      wsi_free(w);
   }
   return STATUS_SUCCESS;
}
