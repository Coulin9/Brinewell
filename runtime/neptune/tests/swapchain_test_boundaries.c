/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#include "swapchain_test_support.h"
#include <stdarg.h>
#include "../npt_com.h"
#include "../npt_event.h"
#include "../npt_env.h"
#include "../npt_profile.h"
#include "../npt_resource.h"
#include "../npt_shared_texture.h"
#include "../npt_renderer_wine_common.h"
#include "../nptunix/npt_unixlib.h"
#include "../neptune-protocol/npt_protocol_defs.h"

#define MAX_RECORDS 4096
struct fd_record { bool allocated, open, ready; unsigned closes, dups; int root; };
struct token_record { uint64_t token; unsigned releases; };
struct command_record { uint32_t type; uint64_t object; size_t size; unsigned char *bytes; };
static struct {
   struct test_fixture *fixture;
   CRITICAL_SECTION lock;
   CONDITION_VARIABLE changed;
   HANDLE token_gate;
   bool initialized, hold, arm_failure, dup_failure, canceled, live;
   unsigned active;
   uint64_t generation;
   int present_result, drain_result;
   uint32_t releases[NPT_WSI_MAX_IMAGES];
   struct fd_record fds[MAX_RECORDS];
   struct token_record tokens[MAX_RECORDS];
   struct command_record commands[MAX_RECORDS];
   unsigned fd_count, token_count, command_count;
} state;

static void lock_state(void) { EnterCriticalSection(&state.lock); }
static void unlock_state(void) { LeaveCriticalSection(&state.lock); }
static void check_fixture(struct test_fixture *f) { CHECK(f && f == state.fixture); }
static struct fd_record *fd_record(int fd)
{
   CHECK(fd > 0 && (unsigned)fd <= state.fd_count);
   return &state.fds[fd - 1];
}
static int make_fd(bool ready, int root)
{
   CHECK(state.fd_count < MAX_RECORDS);
   int fd = ++state.fd_count;
   state.fds[fd - 1] = (struct fd_record){ true, true, ready, 0, 0, root ? root : fd };
   return fd;
}
int test_fd_create(struct test_fixture *f, bool ready)
{
   check_fixture(f); lock_state(); int fd = make_fd(ready, 0); unlock_state(); return fd;
}
void npt_wine_unixlib_close(int fd)
{
   lock_state(); struct fd_record *r = fd_record(fd); CHECK(r->open); r->open = false; ++r->closes; unlock_state();
}
int npt_wine_unixlib_dup(int fd)
{
   lock_state(); struct fd_record *r = fd_record(fd); CHECK(r->open);
   if (state.dup_failure) { unlock_state(); return -1; }
   ++r->dups; int dup = make_fd(r->ready, r->root); unlock_state(); return dup;
}
unsigned test_fd_close_count(int fd) { lock_state(); unsigned n = fd_record(fd)->closes; unlock_state(); return n; }
unsigned test_fd_dup_count(int fd) { lock_state(); unsigned n = fd_record(fd)->dups; unlock_state(); return n; }
void test_set_dup_failure(struct test_fixture *f, bool fail) { check_fixture(f); lock_state(); state.dup_failure = fail; unlock_state(); }
void test_set_arm_failure(struct test_fixture *f, bool fail) { check_fixture(f); lock_state(); state.arm_failure = fail; unlock_state(); }
static struct token_record *find_token(uint64_t token, bool create)
{
   for (unsigned i = 0; i < state.token_count; ++i)
      if (state.tokens[i].token == token) return &state.tokens[i];
   if (!create) return NULL;
   CHECK(token && state.token_count < MAX_RECORDS);
   struct token_record *t = &state.tokens[state.token_count++]; t->token = token; return t;
}
static void release_token(uint64_t token)
{
   lock_state(); struct token_record *t = find_token(token, true); CHECK(!t->releases); ++t->releases;
   bool hold = state.hold; unlock_state();
   test_observe(state.fixture, TEST_TOKEN_RELEASE);
   if (hold) CHECK(WaitForSingleObject(state.token_gate, INFINITE) == WAIT_OBJECT_0);
}
int npt_event_arm_token_fd(struct npt_device *dev, uint64_t token)
{
   CHECK(dev == test_device(state.fixture) && token);
   lock_state(); CHECK(!find_token(token, false)); find_token(token, true); bool fail = state.arm_failure;
   int fd = fail ? -1 : make_fd(false, 0); unlock_state();
   if (fail) release_token(token);
   return fd;
}
void npt_event_release_token(struct npt_device *dev, uint64_t token)
{
   CHECK(dev == test_device(state.fixture)); if (token) release_token(token);
}
unsigned test_token_release_count(uint64_t token)
{
   lock_state(); struct token_record *t = find_token(token, false); unsigned n = t ? t->releases : 0; unlock_state(); return n;
}
void test_hold_token_release(bool hold)
{
   lock_state(); state.hold = hold; CHECK(hold ? ResetEvent(state.token_gate) : SetEvent(state.token_gate)); unlock_state();
}
void test_unblock_token_release(void) { test_hold_token_release(false); }
int npt_unixlib_ensure_init(void) { CHECK(state.fixture); return 0; }
struct npt_unix_wsi_hdr_caps_args test_hdr_caps;
struct npt_unix_hdr10_metadata test_hdr_metadata;
int test_hdr_metadata_result;
unsigned test_hdr_metadata_calls;
NTSTATUS npt_wine_unix_call(unsigned int code, void *args)
{
   if (code == NPT_UNIX_WSI_QUERY_HDR_CAPS) {
      *(struct npt_unix_wsi_hdr_caps_args *)args = test_hdr_caps;
      return 0;
   }
   if (code == NPT_UNIX_WSI_SET_HDR_METADATA) {
      struct npt_unix_wsi_set_hdr_metadata_args *p = args;
      test_hdr_metadata = p->meta;
      test_hdr_metadata_calls++;
      p->result = test_hdr_metadata_result;
      return 0;
   }
   CHECK(code == npt_unix_wait_fd && args);
   struct npt_unix_wait_fd_params *p = args;
   lock_state(); struct fd_record *r = fd_record(p->fd); CHECK(r->open);
   p->result = state.fds[r->root - 1].ready ? 1 : 0; unlock_state(); return 0;
}

static bool wsi_init(struct npt_renderer *r, uint32_t n, const int *fds, const uint32_t *strides,
                     uint32_t format, uint32_t width, uint32_t height, uint64_t hwnd, uint64_t *out,
                     bool *frame_stats_enabled, uint32_t colorspace,
                     const struct npt_unix_hdr10_metadata *hdr_meta)
{
   (void)colorspace; (void)hdr_meta;
   *frame_stats_enabled = false;
   CHECK(r == test_renderer(state.fixture) && n && n <= NPT_WSI_MAX_IMAGES && fds && strides && format && width && height && hwnd && out);
   lock_state(); CHECK(!state.live && !state.active);
   for (unsigned i = 0; i < n; ++i) { CHECK(fd_record(fds[i])->open && strides[i]); }
   state.live = true; state.canceled = false; *out = ++state.generation; unlock_state(); return true;
}
static void check_wsi(struct npt_renderer *r, uint64_t wsi)
{
   CHECK(r == test_renderer(state.fixture) && state.live && wsi == state.generation);
}
static int wsi_present(struct npt_renderer *r, uint64_t wsi, uint32_t image, int fd,
                       uint64_t present_time_ns)
{
   CHECK(present_time_ns == 0);
   lock_state(); check_wsi(r, wsi); CHECK(image < NPT_SC_PRESENT_IMAGES); ++state.active;
   int result = state.present_result; unlock_state();
   if (fd >= 0) npt_wine_unixlib_close(fd);
   else {
      struct npt_guest_swapchain_common *c = test_bound_common(state.fixture);
      CHECK(c && atomic_load(&c->slot_fence_val[image]) <= atomic_load(&c->slot_completed_val[image]));
   }
   test_observe(state.fixture, TEST_PRESENT);
   lock_state(); --state.active; unlock_state(); return result;
}
static int wsi_drain(struct npt_renderer *r, uint64_t wsi, uint32_t timeout, uint32_t *out)
{
   test_observe(state.fixture, TEST_DRAIN_ENTER);
   lock_state(); check_wsi(r, wsi); ++state.active;
   bool any = false;
   for (unsigned i = 0; i < NPT_WSI_MAX_IMAGES; ++i) any |= state.releases[i] != 0;
   if (!any && !state.drain_result && !state.canceled)
      SleepConditionVariableCS(&state.changed, &state.lock, timeout);
   int result = state.canceled ? -1 : state.drain_result;
   if (!result) { memcpy(out, state.releases, sizeof(state.releases)); memset(state.releases, 0, sizeof(state.releases)); }
   --state.active; unlock_state(); test_observe(state.fixture, TEST_DRAIN); return result;
}
static void wsi_cancel(struct npt_renderer *r, uint64_t wsi)
{
   lock_state(); check_wsi(r, wsi); state.canceled = true; WakeAllConditionVariable(&state.changed); unlock_state(); test_observe(state.fixture, TEST_CANCEL);
}
static void wsi_destroy(struct npt_renderer *r, uint64_t wsi)
{
   lock_state(); check_wsi(r, wsi); CHECK(!state.active); state.live = false; unlock_state();
}
static int create_host_blob(struct npt_renderer *r, uint64_t id, uint64_t size)
{
   CHECK(r == test_renderer(state.fixture) && id && size); return test_fd_create(state.fixture, true);
}
void test_set_present_result(struct test_fixture *f, int result) { check_fixture(f); lock_state(); state.present_result = result; unlock_state(); }
void test_set_drain_result(struct test_fixture *f, int result) { check_fixture(f); lock_state(); state.drain_result = result; WakeAllConditionVariable(&state.changed); unlock_state(); }
void test_release_image(struct test_fixture *f, uint32_t image, uint32_t count)
{
   check_fixture(f); CHECK(image < NPT_WSI_MAX_IMAGES && count); lock_state(); state.releases[image] += count; WakeAllConditionVariable(&state.changed); unlock_state();
}
void test_install_renderer(struct test_fixture *f)
{
   CHECK(f && f == test_active_fixture());
   if (state.initialized) {
      CHECK(!state.live && !state.active);
      for (unsigned i = 0; i < state.command_count; ++i) free(state.commands[i].bytes);
      CHECK(CloseHandle(state.token_gate)); DeleteCriticalSection(&state.lock);
   }
   memset(&state, 0, sizeof(state)); state.fixture = f; InitializeCriticalSection(&state.lock);
   InitializeConditionVariable(&state.changed); state.token_gate = CreateEventW(NULL, TRUE, TRUE, NULL); CHECK(state.token_gate); state.initialized = true;
   struct npt_renderer *r = test_renderer(f);
   r->ops.create_host_blob = create_host_blob; r->ops.wsi_init = wsi_init; r->ops.wsi_present = wsi_present;
   r->ops.wsi_drain = wsi_drain; r->ops.wsi_cancel = wsi_cancel; r->ops.wsi_destroy = wsi_destroy;
}

struct npt_cs_encoder *npt_ring_submit_command_init(struct npt_ring *ring, struct npt_ring_submit_command *submit, void *data, size_t size, size_t reply)
{
   CHECK(ring == test_ring(state.fixture) && data && !reply); memset(submit, 0, sizeof(*submit));
   submit->cmd_data = data; submit->cmd_size = size; submit->enc.cur = data; submit->enc.end = (uint8_t *)data + size; return &submit->enc;
}
void npt_ring_submit_command(struct npt_ring *ring, struct npt_ring_submit_command *submit)
{
   CHECK(ring == test_ring(state.fixture) && submit->cmd_size >= sizeof(struct npt_command_header));
   struct npt_command_header header; memcpy(&header, submit->cmd_data, sizeof(header));
   CHECK(header.cmd_size == submit->cmd_size && submit->enc.cur == submit->enc.end);
   lock_state(); CHECK(state.command_count < MAX_RECORDS); struct command_record *c = &state.commands[state.command_count++];
   c->type = header.cmd_type; c->object = header.object_id; c->size = submit->cmd_size; c->bytes = malloc(c->size); CHECK(c->bytes); memcpy(c->bytes, submit->cmd_data, c->size); unlock_state();
}
uint32_t test_command_count(struct test_fixture *f, uint32_t command)
{
   check_fixture(f); lock_state(); uint32_t n = 0; for (unsigned i = 0; i < state.command_count; ++i) n += state.commands[i].type == command; unlock_state(); return n;
}
uint64_t test_last_command(struct test_fixture *f, uint32_t command)
{
   check_fixture(f); lock_state(); uint64_t id = 0; for (unsigned i = 0; i < state.command_count; ++i) if (state.commands[i].type == command) id = state.commands[i].object; unlock_state(); return id;
}
struct npt_ring *npt_device_method_ring(struct npt_device *dev) { CHECK(dev == test_device(state.fixture)); return test_ring(state.fixture); }
struct npt_ring *npt_tls_get_ring(struct npt_device *dev) { return npt_device_method_ring(dev); }
struct npt_ring *npt_device_get_pipeline_library_ring(struct npt_device *dev)
{ (void)dev; CHECK(false); return NULL; }
uint32_t npt_ring_stage_self_index(struct npt_ring *ring) { (void)ring; CHECK(false); return 0; }
void npt_ring_stage_flush_index(struct npt_ring *ring, uint32_t index) { (void)ring; (void)index; CHECK(false); }
bool npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t id, uint32_t *tail) { (void)dev; (void)id; (void)tail; CHECK(false); return false; }
void npt_ring_order_after(struct npt_ring *ring, uint64_t id, uint32_t tail) { (void)ring; (void)id; (void)tail; CHECK(false); }
uint32_t NPT_STDMETHODCALLTYPE npt_com_default_release(void *self) { test_object_release(state.fixture, self); return 0; }
bool npt_device_wrapper_cache_is_live(struct npt_device *dev, uint64_t id, const void *wrapper) { CHECK(dev == test_device(state.fixture) && id); return test_object_alive(state.fixture, wrapper); }
void npt_private_data_init(struct npt_private_data *pd) { pd->head = NULL; mtx_init(&pd->lock, mtx_plain); }
void npt_private_data_fini(struct npt_private_data *pd) { CHECK(!pd->head); mtx_destroy(&pd->lock); }
struct npt_env npt_env;
/* The fixture owns environment state; no environment parsing is performed. */
void npt_env_init(void) { }
struct npt_profile npt_profile;
uint64_t npt_profile_now_ns(void) { LARGE_INTEGER value, freq; CHECK(QueryPerformanceCounter(&value) && QueryPerformanceFrequency(&freq)); return (uint64_t)(value.QuadPart * 1000000000ull / freq.QuadPart); }
/* Profiling is disabled for this fixture. */
void npt_profile_present_marker(void) { CHECK(false); }
void npt_profile_log_present_timing(unsigned sync, uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3) { (void)sync; (void)t0; (void)t1; (void)t2; (void)t3; CHECK(false); }

void npt_log_impl(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   vfprintf(stderr, fmt, args);
   va_end(args);
}

/* Resource export and creation are outside the swapchain fixture contract. */
bool npt_shared_texture_export_blob(void *texture_wrapper,
                                    struct npt_shared_texture_desc *desc)
{
   (void)texture_wrapper; (void)desc; CHECK(false); return false;
}
uint32_t npt_shared_texture_virgl_format(uint32_t dxgi_format)
{
   (void)dxgi_format; CHECK(false); return 0;
}
void *npt_shared_texture_create_exportable(void *device_wrapper,
                                           uint32_t width, uint32_t height,
                                           uint32_t dxgi_format)
{
   (void)device_wrapper; (void)width; (void)height; (void)dxgi_format;
   CHECK(false); return NULL;
}
HRESULT npt_d3d11_device_create_backbuffer(void *device_wrapper,
                                           const D3D11_TEXTURE2D_DESC *desc,
                                           DXGI_FORMAT wire_format,
                                           ID3D11Texture2D **out)
{
   (void)device_wrapper; (void)desc; (void)wire_format; (void)out;
   CHECK(false); return 0;
}

/* The fixture constructs its objects directly, without COM factory or private-data paths. */
HRESULT npt_com_default_query_interface_chain(void *self, const GUID *riid,
                                              void **out, const GUID *const *chain)
{
   (void)self; (void)riid; (void)out; (void)chain; CHECK(false); return 0;
}
uint32_t NPT_STDMETHODCALLTYPE npt_com_default_addref(void *self)
{
   (void)self; CHECK(false); return 0;
}
HRESULT npt_private_data_set(struct npt_private_data *pd, const GUID *name,
                             UINT size, const void *data)
{
   (void)pd; (void)name; (void)size; (void)data; CHECK(false); return 0;
}
HRESULT npt_private_data_set_interface(struct npt_private_data *pd,
                                       const GUID *name, const void *unknown)
{
   (void)pd; (void)name; (void)unknown; CHECK(false); return 0;
}
HRESULT npt_private_data_get(struct npt_private_data *pd, const GUID *name,
                             UINT *size, void *data)
{
   (void)pd; (void)name; (void)size; (void)data; CHECK(false); return 0;
}
uint64_t npt_com_allocate_next_id(void)
{
   CHECK(false); return 0;
}
void *npt_com_get_or_wrap(struct npt_device *dev, const GUID *iid,
                          uint64_t host_id, struct npt_com_base *parent_wrapper)
{
   (void)dev; (void)iid; (void)host_id; (void)parent_wrapper; CHECK(false); return NULL;
}

/* Device acquisition and shutdown are outside the directly initialized fixture. */
struct npt_device *npt_device_acquire(void)
{
   CHECK(false); return NULL;
}
bool npt_device_is_shutting_down(const struct npt_device *dev)
{
   (void)dev; CHECK(false); return false;
}
