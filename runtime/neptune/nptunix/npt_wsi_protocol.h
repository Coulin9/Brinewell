#ifndef NPT_WSI_PROTOCOL_H
#define NPT_WSI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#define NPT_XCB_PRESENT_CAPABILITY_SYNCOBJ (1u << 4)

struct npt_xcb_dri3_import_syncobj_req {
   uint8_t major_opcode;
   uint8_t minor_opcode;
   uint16_t length;
   uint32_t syncobj;
   uint32_t drawable;
};

struct npt_xcb_dri3_free_syncobj_req {
   uint8_t major_opcode;
   uint8_t minor_opcode;
   uint16_t length;
   uint32_t syncobj;
};

struct npt_xcb_present_query_capabilities_req {
   uint8_t major_opcode;
   uint8_t minor_opcode;
   uint16_t length;
   uint32_t target;
};

struct npt_xcb_present_query_capabilities_reply {
   uint8_t response_type;
   uint8_t pad0;
   uint16_t sequence;
   uint32_t length;
   uint32_t capabilities;
   uint8_t pad1[20];
};

struct npt_xcb_present_pixmap_synced_req {
   uint8_t major_opcode;
   uint8_t minor_opcode;
   uint16_t length;
   uint32_t window;
   uint32_t pixmap;
   uint32_t serial;
   uint32_t valid;
   uint32_t update;
   int16_t x_off;
   int16_t y_off;
   uint32_t target_crtc;
   uint32_t acquire_syncobj;
   uint32_t release_syncobj;
   uint64_t acquire_point;
   uint64_t release_point;
   uint32_t options;
   uint32_t pad0;
   uint64_t target_msc;
   uint64_t divisor;
   uint64_t remainder;
};

_Static_assert(sizeof(struct npt_xcb_dri3_import_syncobj_req) == 12, "DRI3 import syncobj size");
_Static_assert(offsetof(struct npt_xcb_dri3_import_syncobj_req, syncobj) == 4, "DRI3 import syncobj offset");
_Static_assert(offsetof(struct npt_xcb_dri3_import_syncobj_req, drawable) == 8, "DRI3 import drawable offset");
_Static_assert(sizeof(struct npt_xcb_dri3_free_syncobj_req) == 8, "DRI3 free syncobj size");
_Static_assert(offsetof(struct npt_xcb_dri3_free_syncobj_req, syncobj) == 4, "DRI3 free syncobj offset");
_Static_assert(sizeof(struct npt_xcb_present_query_capabilities_req) == 8, "Present query capabilities size");
_Static_assert(offsetof(struct npt_xcb_present_query_capabilities_req, target) == 4, "Present query target offset");
_Static_assert(sizeof(struct npt_xcb_present_query_capabilities_reply) == 32, "Present capabilities reply size");
_Static_assert(offsetof(struct npt_xcb_present_query_capabilities_reply, capabilities) == 8, "Present capabilities offset");
_Static_assert(sizeof(struct npt_xcb_present_pixmap_synced_req) == 88, "Present pixmap synced size");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, major_opcode) == 0, "Present major opcode offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, minor_opcode) == 1, "Present minor opcode offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, length) == 2, "Present length offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, window) == 4, "Present window offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, pixmap) == 8, "Present pixmap offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, serial) == 12, "Present serial offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, valid) == 16, "Present valid offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, update) == 20, "Present update offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, x_off) == 24, "Present x offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, y_off) == 26, "Present y offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, target_crtc) == 28, "Present CRTC offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, acquire_syncobj) == 32, "Present acquire syncobj offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, release_syncobj) == 36, "Present release syncobj offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, acquire_point) == 40, "Present acquire point offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, release_point) == 48, "Present release point offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, options) == 56, "Present options offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, pad0) == 60, "Present padding offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, target_msc) == 64, "Present target MSC offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, divisor) == 72, "Present divisor offset");
_Static_assert(offsetof(struct npt_xcb_present_pixmap_synced_req, remainder) == 80, "Present remainder offset");

static inline xcb_void_cookie_t
npt_xcb_dri3_import_syncobj_checked(xcb_connection_t *c, uint32_t syncobj,
                                    xcb_drawable_t drawable, int syncobj_fd)
{
   static const xcb_protocol_request_t xcb_req = { 2, &xcb_dri3_id, 10, 1 };
   struct npt_xcb_dri3_import_syncobj_req req = {
      .minor_opcode = 10, .length = 3, .syncobj = syncobj, .drawable = drawable,
   };
   struct iovec xcb_parts[4];
   xcb_parts[2].iov_base = &req;
   xcb_parts[2].iov_len = sizeof(req);
   xcb_parts[3].iov_base = NULL;
   xcb_parts[3].iov_len = -sizeof(req) & 3;
   xcb_void_cookie_t cookie;
   cookie.sequence = xcb_send_request_with_fds(c, XCB_REQUEST_CHECKED,
                                                xcb_parts + 2, &xcb_req, 1,
                                                &syncobj_fd);
   return cookie;
}

static inline xcb_void_cookie_t
npt_xcb_dri3_free_syncobj(xcb_connection_t *c, uint32_t syncobj)
{
   static const xcb_protocol_request_t xcb_req = { 2, &xcb_dri3_id, 11, 1 };
   struct npt_xcb_dri3_free_syncobj_req req = {
      .minor_opcode = 11, .length = 2, .syncobj = syncobj,
   };
   struct iovec xcb_parts[4];
   xcb_parts[2].iov_base = &req;
   xcb_parts[2].iov_len = sizeof(req);
   xcb_parts[3].iov_base = NULL;
   xcb_parts[3].iov_len = -sizeof(req) & 3;
   xcb_void_cookie_t cookie;
   cookie.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
   return cookie;
}

static inline uint32_t
npt_xcb_present_query_capabilities_sync(xcb_connection_t *c, uint32_t target,
                                        bool *out_ok)
{
   static const xcb_protocol_request_t xcb_req = { 2, &xcb_present_id, 4, 0 };
   struct npt_xcb_present_query_capabilities_req req = {
      .minor_opcode = 4, .length = 2, .target = target,
   };
   struct iovec xcb_parts[4];
   xcb_parts[2].iov_base = &req;
   xcb_parts[2].iov_len = sizeof(req);
   xcb_parts[3].iov_base = NULL;
   xcb_parts[3].iov_len = -sizeof(req) & 3;
   unsigned int seq = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
   xcb_generic_error_t *err = NULL;
   struct npt_xcb_present_query_capabilities_reply *reply =
      xcb_wait_for_reply(c, seq, &err);
   uint32_t capabilities = reply ? reply->capabilities : 0;
   *out_ok = reply != NULL && err == NULL;
   free(err);
   free(reply);
   return capabilities;
}

static inline xcb_void_cookie_t
npt_xcb_present_pixmap_synced(xcb_connection_t *c, xcb_window_t window,
                              xcb_pixmap_t pixmap, uint32_t serial,
                              uint32_t valid, uint32_t update,
                              int16_t x_off, int16_t y_off, uint32_t target_crtc,
                              uint32_t acquire_syncobj, uint32_t release_syncobj,
                              uint64_t acquire_point, uint64_t release_point,
                              uint32_t options, uint64_t target_msc,
                              uint64_t divisor, uint64_t remainder)
{
   static const xcb_protocol_request_t xcb_req = { 2, &xcb_present_id, 5, 1 };
   struct npt_xcb_present_pixmap_synced_req req = {
      .minor_opcode = 5, .length = 22, .window = window, .pixmap = pixmap,
      .serial = serial, .valid = valid, .update = update,
      .x_off = x_off, .y_off = y_off, .target_crtc = target_crtc,
      .acquire_syncobj = acquire_syncobj, .release_syncobj = release_syncobj,
      .acquire_point = acquire_point, .release_point = release_point,
      .options = options, .target_msc = target_msc, .divisor = divisor,
      .remainder = remainder,
   };
   struct iovec xcb_parts[4];
   xcb_parts[2].iov_base = &req;
   xcb_parts[2].iov_len = sizeof(req);
   xcb_parts[3].iov_base = NULL;
   xcb_parts[3].iov_len = -sizeof(req) & 3;
   xcb_void_cookie_t cookie;
   cookie.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
   return cookie;
}

#endif /* NPT_WSI_PROTOCOL_H */
