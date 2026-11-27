/*
 * Run from this directory on Linux (requires libxcb, xcb-dri3, xcb-present
 * and libdrm development headers):
 * cc -std=gnu11 -Wall -Wextra -pthread $(pkg-config --cflags xcb xcb-dri3 xcb-present libdrm) wsi_state_test.c ../npt_unix_wsi.c -o wsi_state_test $(pkg-config --libs xcb xcb-dri3 xcb-present libdrm) -Wl,--wrap=poll -Wl,--wrap=xcb_connect -Wl,--wrap=xcb_disconnect -Wl,--wrap=xcb_connection_has_error -Wl,--wrap=xcb_get_file_descriptor -Wl,--wrap=xcb_generate_id -Wl,--wrap=xcb_dri3_query_version -Wl,--wrap=xcb_dri3_query_version_reply -Wl,--wrap=xcb_present_query_version -Wl,--wrap=xcb_present_query_version_reply -Wl,--wrap=xcb_dri3_pixmap_from_buffer_checked -Wl,--wrap=xcb_request_check -Wl,--wrap=xcb_free_pixmap -Wl,--wrap=xcb_present_select_input -Wl,--wrap=xcb_register_for_special_xge -Wl,--wrap=xcb_unregister_for_special_event -Wl,--wrap=xcb_flush -Wl,--wrap=xcb_intern_atom -Wl,--wrap=xcb_intern_atom_reply -Wl,--wrap=xcb_get_geometry -Wl,--wrap=xcb_get_geometry_reply -Wl,--wrap=xcb_get_property -Wl,--wrap=xcb_get_property_reply -Wl,--wrap=xcb_get_window_attributes -Wl,--wrap=xcb_get_window_attributes_reply -Wl,--wrap=xcb_query_tree -Wl,--wrap=xcb_query_tree_reply -Wl,--wrap=xcb_query_tree_children -Wl,--wrap=xcb_query_tree_children_length -Wl,--wrap=xcb_send_event -Wl,--wrap=xcb_dri3_open -Wl,--wrap=xcb_dri3_open_reply -Wl,--wrap=xcb_dri3_open_reply_fds -Wl,--wrap=xcb_send_request -Wl,--wrap=xcb_send_request_with_fds -Wl,--wrap=xcb_wait_for_reply -Wl,--wrap=xcb_poll_for_special_event -Wl,--wrap=xcb_poll_for_event -Wl,--wrap=xcb_present_pixmap -Wl,--wrap=drmGetCap -Wl,--wrap=drmSyncobjCreate -Wl,--wrap=drmSyncobjDestroy -Wl,--wrap=drmSyncobjHandleToFD -Wl,--wrap=drmSyncobjImportSyncFile -Wl,--wrap=drmSyncobjTransfer -Wl,--wrap=drmSyncobjTimelineSignal -Wl,--wrap=drmSyncobjTimelineWait
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include "../npt_unixlib.h"
#include "../npt_wsi_protocol.h"

extern unsigned int npt_do_wsi_init(void *), npt_do_wsi_present(void *),
 npt_do_wsi_drain(void *), npt_do_wsi_cancel(void *), npt_do_wsi_destroy(void *);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define UNUSED(x) (void)(x)

struct fence { ino_t ino; bool signaled; int writer; };
struct point { uint64_t pt; int fence; bool signaled; };
struct syncobj { bool alive; int fence; struct point points[32]; size_t n; };
struct mapping { ino_t ino; uint32_t handle; };
struct xid_mapping { uint32_t xid, handle; };
static struct fence fences[64];
static struct syncobj objects[64];
static struct mapping exports[64];
static struct xid_mapping xids[64];
static size_t nfences, nexports, nxids;
static pthread_mutex_t mock_mtx = PTHREAD_MUTEX_INITIALIZER;
static int sockets[2], open_reply_fd, timeline_cap, present_cap, fail_pixmap;
static int pixmap_calls, free_pixmaps, fail_import, fail_transfer;
static int fail_create_at;
static uint32_t next_id, next_handle, select_mask, pixmaps[4];
static unsigned sequence, synced_count, import_count, free_count, query_count, legacy_count;
static uint8_t mock_geom_depth, last_pixmap_depth;
static int16_t last_legacy_x_off, last_legacy_y_off;
static struct npt_xcb_present_pixmap_synced_req last_synced;
static xcb_generic_event_t *special_event, *normal_event;
static pthread_mutex_t poll_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poll_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int track_polls, polls_entered;

static uint64_t now_ns(void) { struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t)); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int count_fds(void) { DIR *d = opendir("/proc/self/fd"); CHECK(d); int n = 0; struct dirent *e; while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++; closedir(d); return n - 1; }
static int fence_create(bool signaled, int *fd) { int p[2]; struct stat st; CHECK(nfences < 64 && !pipe(p) && !fstat(p[0], &st)); int i = (int)nfences++; fences[i] = (struct fence){ st.st_ino, signaled, p[1] }; if (signaled) { close(p[1]); fences[i].writer = -1; } *fd = p[0]; return i; }
static void fence_signal(int i) { pthread_mutex_lock(&mock_mtx); fences[i].signaled = true; pthread_mutex_unlock(&mock_mtx); }
static void fence_cleanup(void) { for (size_t i = 0; i < nfences; i++) if (fences[i].writer >= 0) CHECK(!close(fences[i].writer)); }
static void reset_mock(void) { memset(objects, 0, sizeof(objects)); memset(fences, 0, sizeof(fences)); nfences = nexports = nxids = 0; sockets[0] = sockets[1] = open_reply_fd = -1; timeline_cap = present_cap = 1; fail_pixmap = -1; fail_create_at = 0; pixmap_calls = free_pixmaps = fail_import = fail_transfer = 0; next_id = 100; next_handle = 1; select_mask = 0; sequence = synced_count = import_count = free_count = query_count = legacy_count = 0; mock_geom_depth = last_pixmap_depth = 0; last_legacy_x_off = last_legacy_y_off = 0; memset(&last_synced, 0, sizeof(last_synced)); special_event = normal_event = NULL; atomic_store(&track_polls, 0); atomic_store(&polls_entered, 0); }
static void end_test(int before) { fence_cleanup(); CHECK(count_fds() == before); }
static struct point *point_get(uint32_t h, uint64_t p) { struct syncobj *o = &objects[h]; CHECK(h && h < 64 && o->alive); for (size_t i = 0; i < o->n; i++) if (o->points[i].pt == p) return &o->points[i]; CHECK(o->n < 32); o->points[o->n] = (struct point){p, -1, false}; return &o->points[o->n++]; }
static uint32_t xid_handle(uint32_t id) { for (size_t i = 0; i < nxids; i++) if (xids[i].xid == id) return xids[i].handle; CHECK(false); return 0; }
static void release_signal(uint32_t xid, uint64_t point) { pthread_mutex_lock(&mock_mtx); point_get(xid_handle(xid), point)->signaled = true; pthread_mutex_unlock(&mock_mtx); }

int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout) { if (atomic_load(&track_polls) && timeout >= 1000) { pthread_mutex_lock(&poll_mtx); atomic_fetch_add(&polls_entered, 1); pthread_cond_broadcast(&poll_cv); pthread_mutex_unlock(&poll_mtx); } return __real_poll(fds, n, timeout); }

xcb_connection_t *__wrap_xcb_connect(const char *s, int *screen) { UNUSED(s); UNUSED(screen); CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)); return (xcb_connection_t *)(uintptr_t)1; }
void __wrap_xcb_disconnect(xcb_connection_t *c) { UNUSED(c); CHECK(!close(sockets[0])); CHECK(!close(sockets[1])); sockets[0] = sockets[1] = -1; }
int __wrap_xcb_connection_has_error(xcb_connection_t *c) { UNUSED(c); return 0; }
int __wrap_xcb_get_file_descriptor(xcb_connection_t *c) { UNUSED(c); return sockets[0]; }
uint32_t __wrap_xcb_generate_id(xcb_connection_t *c) { UNUSED(c); return next_id++; }
xcb_dri3_query_version_cookie_t __wrap_xcb_dri3_query_version(xcb_connection_t *c, uint32_t a, uint32_t b) { UNUSED(c); UNUSED(a); UNUSED(b); return (xcb_dri3_query_version_cookie_t){1}; }
xcb_dri3_query_version_reply_t *__wrap_xcb_dri3_query_version_reply(xcb_connection_t *c, xcb_dri3_query_version_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); xcb_dri3_query_version_reply_t *r = calloc(1, sizeof(*r)); CHECK(r); r->major_version = 1; r->minor_version = 4; return r; }
xcb_present_query_version_cookie_t __wrap_xcb_present_query_version(xcb_connection_t *c, uint32_t a, uint32_t b) { UNUSED(c); UNUSED(a); UNUSED(b); return (xcb_present_query_version_cookie_t){1}; }
xcb_present_query_version_reply_t *__wrap_xcb_present_query_version_reply(xcb_connection_t *c, xcb_present_query_version_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); xcb_present_query_version_reply_t *r = calloc(1, sizeof(*r)); CHECK(r); r->major_version = 1; r->minor_version = 4; return r; }
xcb_void_cookie_t __wrap_xcb_dri3_pixmap_from_buffer_checked(xcb_connection_t *c, xcb_pixmap_t p, xcb_drawable_t d, uint32_t size, uint16_t w, uint16_t h, uint16_t stride, uint8_t depth, uint8_t bpp, int fd) { UNUSED(c); UNUSED(d); UNUSED(size); UNUSED(w); UNUSED(h); UNUSED(stride); UNUSED(bpp); last_pixmap_depth = depth; CHECK(!close(fd)); pixmaps[pixmap_calls++] = p; return (xcb_void_cookie_t){(unsigned)pixmap_calls}; }
xcb_generic_error_t *__wrap_xcb_request_check(xcb_connection_t *c, xcb_void_cookie_t k) { UNUSED(c); if (fail_pixmap >= 0 && k.sequence == (unsigned)fail_pixmap + 1) return calloc(1, sizeof(xcb_generic_error_t)); return NULL; }
xcb_void_cookie_t __wrap_xcb_free_pixmap(xcb_connection_t *c, xcb_pixmap_t p) { UNUSED(c); CHECK(p == pixmaps[free_pixmaps]); free_pixmaps++; return (xcb_void_cookie_t){0}; }
xcb_void_cookie_t __wrap_xcb_present_select_input(xcb_connection_t *c, xcb_present_event_t e, xcb_window_t w, uint32_t mask) { UNUSED(c); UNUSED(e); UNUSED(w); select_mask = mask; return (xcb_void_cookie_t){0}; }
xcb_special_event_t *__wrap_xcb_register_for_special_xge(xcb_connection_t *c, xcb_extension_t *e, uint32_t id, uint32_t *stamp) { UNUSED(c); UNUSED(e); UNUSED(id); UNUSED(stamp); return (xcb_special_event_t *)(uintptr_t)1; }
void __wrap_xcb_unregister_for_special_event(xcb_connection_t *c, xcb_special_event_t *e) { UNUSED(c); UNUSED(e); }
int __wrap_xcb_flush(xcb_connection_t *c) { UNUSED(c); return 1; }
xcb_intern_atom_cookie_t __wrap_xcb_intern_atom(xcb_connection_t *c, uint8_t o, uint16_t n, const char *s) { UNUSED(c); UNUSED(o); UNUSED(n); UNUSED(s); return (xcb_intern_atom_cookie_t){0}; }
xcb_intern_atom_reply_t *__wrap_xcb_intern_atom_reply(xcb_connection_t *c, xcb_intern_atom_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); return NULL; }
xcb_get_geometry_cookie_t __wrap_xcb_get_geometry(xcb_connection_t *c, xcb_drawable_t d) { UNUSED(c); UNUSED(d); return (xcb_get_geometry_cookie_t){0}; }
xcb_get_geometry_reply_t *__wrap_xcb_get_geometry_reply(xcb_connection_t *c, xcb_get_geometry_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); if (!mock_geom_depth) return NULL; xcb_get_geometry_reply_t *r = calloc(1, sizeof(*r)); CHECK(r); r->depth = mock_geom_depth; return r; }
xcb_get_property_cookie_t __wrap_xcb_get_property(xcb_connection_t *c, uint8_t del, xcb_window_t w, xcb_atom_t p, xcb_atom_t t, uint32_t o, uint32_t n) { UNUSED(c); UNUSED(del); UNUSED(w); UNUSED(p); UNUSED(t); UNUSED(o); UNUSED(n); return (xcb_get_property_cookie_t){0}; }
xcb_get_property_reply_t *__wrap_xcb_get_property_reply(xcb_connection_t *c, xcb_get_property_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); return NULL; }
xcb_get_window_attributes_cookie_t __wrap_xcb_get_window_attributes(xcb_connection_t *c, xcb_window_t w) { UNUSED(c); UNUSED(w); return (xcb_get_window_attributes_cookie_t){0}; }
xcb_get_window_attributes_reply_t *__wrap_xcb_get_window_attributes_reply(xcb_connection_t *c, xcb_get_window_attributes_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); return NULL; }
xcb_query_tree_cookie_t __wrap_xcb_query_tree(xcb_connection_t *c, xcb_window_t w) { UNUSED(c); UNUSED(w); return (xcb_query_tree_cookie_t){0}; }
xcb_query_tree_reply_t *__wrap_xcb_query_tree_reply(xcb_connection_t *c, xcb_query_tree_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); return NULL; }
xcb_window_t *__wrap_xcb_query_tree_children(const xcb_query_tree_reply_t *r) { UNUSED(r); return NULL; }
int __wrap_xcb_query_tree_children_length(const xcb_query_tree_reply_t *r) { UNUSED(r); return 0; }
xcb_void_cookie_t __wrap_xcb_send_event(xcb_connection_t *c, uint8_t p, xcb_window_t d, uint32_t m, const char *e) { UNUSED(c); UNUSED(p); UNUSED(d); UNUSED(m); UNUSED(e); return (xcb_void_cookie_t){0}; }
xcb_dri3_open_cookie_t __wrap_xcb_dri3_open(xcb_connection_t *c, xcb_drawable_t d, uint32_t p) { UNUSED(c); UNUSED(d); UNUSED(p); return (xcb_dri3_open_cookie_t){0}; }
xcb_dri3_open_reply_t *__wrap_xcb_dri3_open_reply(xcb_connection_t *c, xcb_dri3_open_cookie_t k, xcb_generic_error_t **e) { UNUSED(c); UNUSED(k); UNUSED(e); xcb_dri3_open_reply_t *r = calloc(1, sizeof(*r)); CHECK(r); r->nfd = 1; int f[2]; CHECK(!pipe(f)); close(f[1]); open_reply_fd = f[0]; return r; }
int *__wrap_xcb_dri3_open_reply_fds(xcb_connection_t *c, xcb_dri3_open_reply_t *r) { UNUSED(c); UNUSED(r); return &open_reply_fd; }

static void request_bytes(struct iovec *v, const xcb_protocol_request_t *req, size_t len, uint8_t opcode, uint16_t words) { CHECK(req->count == 2 && v[0].iov_len == len && v[1].iov_len == 0); const unsigned char *b = v[0].iov_base; uint16_t n; memcpy(&n, b + 2, 2); CHECK(b[1] == opcode && n == words && req->opcode == opcode); }
unsigned int __wrap_xcb_send_request(xcb_connection_t *c, int flags, struct iovec *v, const xcb_protocol_request_t *req) { UNUSED(c); UNUSED(flags); memset(&v[-2], 0, 2 * sizeof(struct iovec)); if (req->ext == &xcb_present_id && req->opcode == 4) { request_bytes(v, req, 8, 4, 2); query_count++; } else if (req->ext == &xcb_dri3_id && req->opcode == 11) { request_bytes(v, req, 8, 11, 2); free_count++; } else if (req->ext == &xcb_present_id && req->opcode == 5) { request_bytes(v, req, 88, 5, 22); memcpy(&last_synced, v[0].iov_base, sizeof(last_synced)); synced_count++; } else CHECK(false); return ++sequence + 1000; }
unsigned int __wrap_xcb_send_request_with_fds(xcb_connection_t *c, int flags, struct iovec *v, const xcb_protocol_request_t *req, unsigned int n, int *fds) { UNUSED(c); UNUSED(flags); memset(&v[-2], 0, 2 * sizeof(struct iovec)); CHECK(req->ext == &xcb_dri3_id && n == 1); request_bytes(v, req, 12, 10, 3); struct stat st; CHECK(!fstat(fds[0], &st)); struct npt_xcb_dri3_import_syncobj_req r; memcpy(&r, v[0].iov_base, sizeof(r)); CHECK(r.drawable == 0x1234); size_t i; for (i = 0; i < nexports && exports[i].ino != st.st_ino; i++); CHECK(i < nexports && nxids < 64); xids[nxids++] = (struct xid_mapping){r.syncobj, exports[i].handle}; CHECK(!close(fds[0])); import_count++; return ++sequence + 1000; }
void *__wrap_xcb_wait_for_reply(xcb_connection_t *c, unsigned int s, xcb_generic_error_t **e) { UNUSED(c); UNUSED(s); UNUSED(e); struct npt_xcb_present_query_capabilities_reply *r = calloc(1, sizeof(*r)); CHECK(r); r->capabilities = present_cap ? NPT_XCB_PRESENT_CAPABILITY_SYNCOBJ : 0; return r; }
xcb_generic_event_t *__wrap_xcb_poll_for_special_event(xcb_connection_t *c, xcb_special_event_t *e) { UNUSED(c); UNUSED(e); xcb_generic_event_t *r = special_event; special_event = NULL; return r; }
xcb_generic_event_t *__wrap_xcb_poll_for_event(xcb_connection_t *c) { UNUSED(c); xcb_generic_event_t *r = normal_event; normal_event = NULL; return r; }
xcb_void_cookie_t __wrap_xcb_present_pixmap(xcb_connection_t *c, xcb_window_t w, xcb_pixmap_t p, uint32_t s, xcb_xfixes_region_t v, xcb_xfixes_region_t u, int16_t x, int16_t y, xcb_randr_crtc_t crtc, xcb_sync_fence_t wait, xcb_sync_fence_t idle, uint32_t options, uint64_t target, uint64_t divisor, uint64_t remainder, uint32_t n, const xcb_present_notify_t *notifies) { UNUSED(c); UNUSED(w); UNUSED(p); UNUSED(s); UNUSED(v); UNUSED(u); UNUSED(crtc); UNUSED(wait); UNUSED(idle); UNUSED(options); UNUSED(target); UNUSED(divisor); UNUSED(remainder); UNUSED(n); UNUSED(notifies); last_legacy_x_off = x; last_legacy_y_off = y; legacy_count++; return (xcb_void_cookie_t){0}; }

int __wrap_drmGetCap(int fd, uint64_t cap, uint64_t *value) { UNUSED(fd); CHECK(cap == DRM_CAP_SYNCOBJ_TIMELINE); *value = timeline_cap; return 0; }
int __wrap_drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) { UNUSED(fd); UNUSED(flags); if (fail_create_at && next_handle == (uint32_t)fail_create_at) { errno = ENOMEM; return -1; } CHECK(next_handle < 64); *handle = next_handle++; objects[*handle].alive = true; objects[*handle].fence = -1; return 0; }
int __wrap_drmSyncobjDestroy(int fd, uint32_t handle) { UNUSED(fd); CHECK(handle < 64 && objects[handle].alive); objects[handle].alive = false; return 0; }
int __wrap_drmSyncobjHandleToFD(int fd, uint32_t handle, int *out) { UNUSED(fd); int p[2]; struct stat st; CHECK(nexports < 64 && !pipe(p)); CHECK(!close(p[1]) && !fstat(p[0], &st)); exports[nexports++] = (struct mapping){st.st_ino, handle}; *out = p[0]; return 0; }
int __wrap_drmSyncobjImportSyncFile(int fd, uint32_t handle, int fence_fd) { UNUSED(fd); if (fail_import) { errno = EINVAL; return -1; } struct stat st; CHECK(!fstat(fence_fd, &st)); size_t i; for (i = 0; i < nfences && fences[i].ino != st.st_ino; i++); CHECK(i < nfences); objects[handle].fence = (int)i; return 0; }
int __wrap_drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dp, uint32_t src, uint64_t sp, uint32_t flags) { UNUSED(fd); UNUSED(sp); UNUSED(flags); if (fail_transfer) { errno = EINVAL; return -1; } pthread_mutex_lock(&mock_mtx); struct point *p = point_get(dst, dp); p->fence = objects[src].fence; p->signaled = false; pthread_mutex_unlock(&mock_mtx); return 0; }
int __wrap_drmSyncobjTimelineSignal(int fd, const uint32_t *handles, const uint64_t *points, uint32_t count) { UNUSED(fd); pthread_mutex_lock(&mock_mtx); for (uint32_t i = 0; i < count; i++) point_get(handles[i], points[i])->signaled = true; pthread_mutex_unlock(&mock_mtx); return 0; }
int __wrap_drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned int count, int64_t timeout, unsigned int flags, uint32_t *first) { UNUSED(fd); for (;;) { unsigned found = 0, first_index = 0; pthread_mutex_lock(&mock_mtx); for (unsigned i = 0; i < count; i++) { struct syncobj *o = &objects[handles[i]]; bool ok = false; for (size_t j = 0; j < o->n; j++) { struct point *p = &o->points[j]; if (p->pt >= points[i] && (p->signaled || (p->fence >= 0 && fences[p->fence].signaled))) { ok = true; break; } } if (ok) { if (!found) first_index = i; found++; } } pthread_mutex_unlock(&mock_mtx); if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? found == count : found > 0) { if (first) *first = first_index; return 0; } if (!timeout || now_ns() >= (uint64_t)timeout) { errno = ETIME; return -1; } usleep(1000); } }

static uint64_t init_offset(unsigned n, int16_t x_off, int16_t y_off) { struct npt_unix_wsi_init_params p = { .x11_window = 0x1234, .num_images = n, .width = 16, .height = 16, .x_off = x_off, .y_off = y_off }; for (unsigned i = 0; i < n; i++) { int f[2]; CHECK(!pipe(f)); close(f[1]); p.fds[i] = f[0]; p.strides[i] = 64; } npt_do_wsi_init(&p); for (unsigned i = 0; i < n; i++) CHECK(!close(p.fds[i])); if (fail_pixmap >= 0) { CHECK(p.result == -1 && p.wsi == 0); return 0; } CHECK(p.result == 0 && p.wsi); return p.wsi; }
static uint64_t init(unsigned n) { return init_offset(n, 0, 0); }
static void destroy(uint64_t w) { struct npt_unix_wsi_destroy_params p = {w}; npt_do_wsi_destroy(&p); }
static struct npt_unix_wsi_present_params present(uint64_t w, unsigned image, int fd) { struct npt_unix_wsi_present_params p = { .wsi = w, .image_index = image, .wait_fence_fd = fd }; npt_do_wsi_present(&p); return p; }
static struct npt_unix_wsi_drain_params drain(uint64_t w, unsigned timeout) { struct npt_unix_wsi_drain_params p = { .wsi = w, .timeout_ms = timeout }; npt_do_wsi_drain(&p); return p; }
static void assert_clean_objects(void) { for (unsigned i = 1; i < next_handle; i++) CHECK(!objects[i].alive); CHECK(import_count == free_count); }
static void explicit_env(void) { CHECK(!setenv("NPT_WSI_EXPLICIT_SYNC", "1", 1)); CHECK(!setenv("NPT_WSI_DIAGNOSTICS", "0", 1)); }

static void test_wire(void) { int before = count_fds(); reset_mock(); explicit_env(); uint64_t w = init(1); CHECK(query_count == 1 && import_count == 2); CHECK(!(select_mask & XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY)); struct npt_unix_wsi_present_params p = present(w, 0, -1); CHECK(!p.result && !p.wait_fence_consumed && synced_count == 1); const unsigned char *b = (const void *)&last_synced; uint32_t u32; uint64_t u64;
#define U32(off, val) do { memcpy(&u32, b + (off), 4); CHECK(u32 == (val)); } while (0)
#define U64(off, val) do { memcpy(&u64, b + (off), 8); CHECK(u64 == (val)); } while (0)
 U32(4, 0x1234); U32(8, pixmaps[0]); U32(12, 1); U32(32, xids[0].xid); U32(36, xids[1].xid); U64(40, 1); U64(48, 1); U32(56, XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_COPY);
#undef U32
#undef U64
 destroy(w); assert_clean_objects(); CHECK(free_count == 2); end_test(before); }
static void test_fallback(void) { for (int mode = 0; mode < 5; mode++) { int before = count_fds(); reset_mock(); explicit_env(); if (mode == 0) CHECK(!setenv("NPT_WSI_EXPLICIT_SYNC", "0", 1)); if (mode == 1) present_cap = 0; if (mode == 2) timeline_cap = 0; if (mode == 3) fail_pixmap = 1; if (mode == 4) fail_create_at = 3; uint64_t w = init(2); if (w) { CHECK(select_mask & XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY); if (mode == 4) { CHECK(import_count == 2 && free_count == 2 && next_handle == 3); assert_clean_objects(); CHECK(!present(w, 0, -1).result && synced_count == 0); } else CHECK(import_count == 0 && next_handle == 1); destroy(w); CHECK(free_pixmaps == 2); } else CHECK(free_pixmaps == 1); assert_clean_objects(); end_test(before); } }
static void test_sequence(void) { int before = count_fds(); reset_mock(); explicit_env(); uint64_t w = init(1); for (unsigned pt = 1; pt <= 3; pt++) { int fd = -1, fi = -1; if (pt != 2) fi = fence_create(false, &fd); struct npt_unix_wsi_present_params p = present(w, 0, fd); CHECK(!p.result && p.wait_fence_consumed == (fd >= 0)); if (fd >= 0) { CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF); fence_signal(fi); } CHECK(last_synced.acquire_point == pt && last_synced.release_point == pt); release_signal(last_synced.release_syncobj, pt); struct npt_unix_wsi_drain_params d = drain(w, 0); CHECK(!d.result && d.released_count[0] == 1); } for (int failure = 0; failure < 2; failure++) { int fd; fence_create(false, &fd); fail_import = failure == 0; fail_transfer = failure == 1; unsigned sent = synced_count; struct npt_unix_wsi_present_params p = present(w, 0, fd); CHECK(p.result == -1 && p.wait_fence_consumed == 1 && synced_count == sent); CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF); } fail_import = fail_transfer = 0; CHECK(!present(w, 0, -1).result); CHECK(last_synced.acquire_point == 4 && last_synced.release_point == 4); destroy(w); assert_clean_objects(); end_test(before); }
static void test_release_first(void) { int before = count_fds(); reset_mock(); explicit_env(); uint64_t w = init(1); int fd; int fi = fence_create(false, &fd); CHECK(!present(w, 0, fd).result); release_signal(last_synced.release_syncobj, 1); struct npt_unix_wsi_drain_params d = drain(w, 0); CHECK(!d.result && !d.released_count[0]); fence_signal(fi); d = drain(w, 0); CHECK(!d.result && d.released_count[0] == 1); d = drain(w, 0); CHECK(!d.result && !d.released_count[0]); destroy(w); assert_clean_objects(); end_test(before); }
static void test_independent(void) { int before = count_fds(); reset_mock(); explicit_env(); uint64_t w = init(2); CHECK(!present(w, 0, -1).result); CHECK(!present(w, 1, -1).result); release_signal(last_synced.release_syncobj, 1); struct npt_unix_wsi_drain_params d = drain(w, 0); CHECK(!d.result && !d.released_count[0] && d.released_count[1] == 1); destroy(w); assert_clean_objects(); end_test(before); }
static void test_idle(void) { int before = count_fds(); reset_mock(); explicit_env(); uint64_t w = init(1); int fd; fence_create(false, &fd); CHECK(!present(w, 0, fd).result); xcb_present_idle_notify_event_t *e = calloc(1, sizeof(*e)); CHECK(e); ((xcb_present_generic_event_t *)e)->evtype = XCB_PRESENT_EVENT_IDLE_NOTIFY; e->pixmap = pixmaps[0]; special_event = (xcb_generic_event_t *)e; struct npt_unix_wsi_drain_params d = drain(w, 0); CHECK(!d.result && !d.released_count[0]); destroy(w); assert_clean_objects(); end_test(before); }
static void test_error(void) { int before = count_fds(); reset_mock(); explicit_env(); uint64_t w = init(1); xcb_generic_error_t *e = calloc(1, sizeof(*e)); CHECK(e); e->error_code = XCB_WINDOW; normal_event = (xcb_generic_event_t *)e; CHECK(drain(w, 0).result == -2); CHECK(present(w, 0, -1).result == -2); destroy(w); assert_clean_objects(); end_test(before); }
static void *present_thread(void *arg) { npt_do_wsi_present(arg); return NULL; }
static void *drain_thread(void *arg) { npt_do_wsi_drain(arg); return NULL; }
static void test_cancel(void) { int before = count_fds(); reset_mock(); explicit_env(); CHECK(!setenv("NPT_WSI_EXPLICIT_SYNC", "0", 1)); uint64_t w = init(1); int fd; fence_create(false, &fd); struct npt_unix_wsi_present_params p = { .wsi = w, .image_index = 0, .wait_fence_fd = fd }; struct npt_unix_wsi_drain_params d = { .wsi = w, .timeout_ms = 5000 }; pthread_t a, b; atomic_store(&track_polls, 1); CHECK(!pthread_create(&a, NULL, present_thread, &p)); CHECK(!pthread_create(&b, NULL, drain_thread, &d)); pthread_mutex_lock(&poll_mtx); struct timespec deadline; CHECK(!clock_gettime(CLOCK_REALTIME, &deadline)); deadline.tv_sec += 5; while (atomic_load(&polls_entered) < 2) CHECK(!pthread_cond_timedwait(&poll_cv, &poll_mtx, &deadline)); pthread_mutex_unlock(&poll_mtx); usleep(5000); uint64_t start = now_ns(); struct npt_unix_wsi_cancel_params c = {w}; npt_do_wsi_cancel(&c); CHECK(!pthread_join(a, NULL) && !pthread_join(b, NULL)); CHECK(now_ns() - start < 200000000ull && p.result == -1 && d.result == -1 && p.wait_fence_consumed == 1); CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF); atomic_store(&track_polls, 0); destroy(w); assert_clean_objects(); end_test(before); }
static void test_child_offset_and_depth(void) {
   int before = count_fds();
   reset_mock();
   explicit_env();
   uint64_t w = init_offset(1, 32, 48);
   CHECK(last_pixmap_depth == 24);
   struct npt_unix_wsi_present_params p = present(w, 0, -1);
   CHECK(!p.result && synced_count == 1 && last_synced.x_off == 32 && last_synced.y_off == 48);
   int16_t wire_x, wire_y;
   memcpy(&wire_x, (const unsigned char *)&last_synced + 24, 2);
   memcpy(&wire_y, (const unsigned char *)&last_synced + 26, 2);
   CHECK(wire_x == 32 && wire_y == 48);
   destroy(w);
   assert_clean_objects();
   end_test(before);

   before = count_fds();
   reset_mock();
   explicit_env();
   CHECK(!setenv("NPT_WSI_EXPLICIT_SYNC", "0", 1));
   w = init_offset(1, 32, 48);
   CHECK(last_pixmap_depth == 24);
   p = present(w, 0, -1);
   CHECK(!p.result && synced_count == 0 && legacy_count == 1 &&
         last_legacy_x_off == 32 && last_legacy_y_off == 48);
   destroy(w);
   assert_clean_objects();
   end_test(before);

   before = count_fds();
   reset_mock();
   explicit_env();
   mock_geom_depth = 32;
   w = init_offset(1, 16, 24);
   CHECK(last_pixmap_depth == 32);
   p = present(w, 0, -1);
   CHECK(!p.result && last_synced.x_off == 16 && last_synced.y_off == 24);
   destroy(w);
   assert_clean_objects();
   end_test(before);
}
int main(void) { test_wire(); test_fallback(); test_sequence(); test_release_first(); test_independent(); test_idle(); test_error(); test_cancel(); test_child_offset_and_depth(); puts("NPT_WSI_STATE_TEST: PASS"); return 0; }
