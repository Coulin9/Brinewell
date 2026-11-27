/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#define TEST_INTERCEPT_WIN32
#include "swapchain_test_support.h"
#include "../npt_swapchain_common.c"

struct common_fixture {
   struct test_fixture *test;
   struct npt_com_base com;
   struct npt_guest_swapchain_common common;
};

static void
common_init(struct common_fixture *f, bool waitable)
{
   memset(f, 0, sizeof(*f));
   f->test = test_fixture_create();
   test_install_renderer(f->test);
   DXGI_SWAP_CHAIN_DESC1 desc = {
      .Width = 64, .Height = 64,
      .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .SampleDesc = { .Count = 1 },
      .BufferCount = 2,
      .Flags = waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0,
   };
   npt_gsc_init(&f->common, &f->com, &desc, NULL, NULL, "common-test");
   test_fixture_bind(f->test, &f->common, &f->com);
   f->common.gpu_poll = test_gpu_poll;
   f->common.gpu_poll_ctx = f->test;
}

static void
common_renderer_init(struct common_fixture *f)
{
   int fds[NPT_SC_PRESENT_IMAGES];
   uint32_t strides[NPT_SC_PRESENT_IMAGES];
   for (unsigned i = 0; i < NPT_SC_PRESENT_IMAGES; ++i) {
      fds[i] = test_fd_create(f->test, true);
      strides[i] = 256;
   }
   struct npt_guest_swapchain_common *c = &f->common;
   CHECK(npt_renderer_wsi_init(test_renderer(f->test), NPT_SC_PRESENT_IMAGES,
                               fds, strides, 1, 64, 64, 1, &c->wsi,
                               &c->frame_stats_enabled, 0, NULL));
   c->wsi_initialized = true;
   for (unsigned i = 0; i < NPT_SC_PRESENT_IMAGES; ++i)
      npt_wine_unixlib_close(fds[i]);
}

static void
common_fini(struct common_fixture *f)
{
   npt_gsc_wsi_stop_begin(&f->common, false);
   CHECK(npt_gsc_wsi_join(&f->common, false) == S_OK);
   npt_gsc_wsi_finish(&f->common);
   npt_gsc_fini(&f->common);
   test_fixture_destroy(f->test);
}

static void
latency_marker_not_release(void)
{
   struct common_fixture f;
   test_begin("common latency tracks producer completion");
   common_init(&f, true);
   struct npt_guest_swapchain_common *c = &f.common;
   CHECK(c->max_frame_latency == 1);
   CHECK(WaitForSingleObject(c->frame_latency_sem, 0) == WAIT_OBJECT_0);
   atomic_store(&c->slot_state[0], NPT_GSC_SLOT_SUBMITTED);
   atomic_store(&c->slot_fence_val[0], 7);
   c->latency[0] = (struct npt_gsc_latency_entry){ .fd = -1, .image = 0, .value = 7 };
   c->latency_count = 1;
   CHECK(npt_gsc_present_would_block(c, 1));
   CHECK(c->latency_count == 1);
   atomic_store(&c->slot_completed_val[0], 7);
   CHECK(!npt_gsc_present_would_block(c, 1));
   CHECK(c->latency_count == 0);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_SUBMITTED);
   CHECK(WaitForSingleObject(c->frame_latency_sem, 0) == WAIT_TIMEOUT);
   common_fini(&f);
   test_end();
}

struct fail_call {
   struct npt_guest_swapchain_common *common;
   HANDLE start;
   HRESULT error;
};

static DWORD WINAPI
fail_thread(void *arg)
{
   struct fail_call *call = arg;
   CHECK(WaitForSingleObject(call->start, 5000) == WAIT_OBJECT_0);
   npt_gsc_fail_generation(call->common, call->error);
   return 0;
}

static void
concurrent_failure_latch(void)
{
   struct common_fixture f;
   test_begin("common concurrent failure latch");
   common_init(&f, false);
   HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL);
   CHECK(start);
   struct fail_call calls[2] = {
      { &f.common, start, (HRESULT)0x887a0005u },
      { &f.common, start, (HRESULT)0x887a0006u },
   };
   HANDLE threads[2];
   for (unsigned i = 0; i < 2; ++i) {
      threads[i] = CreateThread(NULL, 0, fail_thread, &calls[i], 0, NULL);
      CHECK(threads[i]);
   }
   CHECK(SetEvent(start));
   for (unsigned i = 0; i < 2; ++i) {
      CHECK(WaitForSingleObject(threads[i], 5000) == WAIT_OBJECT_0);
      CHECK(CloseHandle(threads[i]));
   }
   CHECK(CloseHandle(start));
   HRESULT latched = (HRESULT)atomic_load(&f.common.gen_error_hr);
   CHECK(latched == calls[0].error || latched == calls[1].error);
   CHECK(atomic_load(&f.common.gen_state) == NPT_GSC_GEN_FAILED);
   npt_gsc_fail_generation(&f.common, (HRESULT)0x887a0007u);
   CHECK((HRESULT)atomic_load(&f.common.gen_error_hr) == latched);
   CHECK(npt_gsc_reserve_slot(&f.common, 0, true) == latched);
   common_fini(&f);
   test_end();
}

static DWORD WINAPI
stop_thread(void *arg)
{
   npt_gsc_wsi_stop_begin(arg, false);
   return 0;
}

static void
stop_during_queued_token_cleanup(void)
{
   struct common_fixture f;
   test_begin("common stop while queued token cleanup is held");
   common_init(&f, false);
   struct npt_guest_swapchain_common *c = &f.common;
   test_hold_token_release(true);
   uint64_t token = npt_gsc_make_token();
   int fd = npt_event_arm_token_fd(test_device(f.test), token);
   CHECK(fd >= 0);
   CHECK(npt_gsc_wsi_push(c, 0, fd, token, 1, false));
   atomic_store(&c->wsi_thread_exit, 1);
   c->wsi_thread = CreateThread(NULL, 0, npt_gsc_wsi_thread, c, 0, NULL);
   CHECK(c->wsi_thread);
   c->wsi_thread_started = true;
   test_wait(f.test, TEST_TOKEN_RELEASE, 1);

   HANDLE stopper = CreateThread(NULL, 0, stop_thread, c, 0, NULL);
   CHECK(stopper);
   DWORD stopped = WaitForSingleObject(stopper, 5000);
   test_unblock_token_release();
   CHECK(stopped == WAIT_OBJECT_0);
   CHECK(CloseHandle(stopper));
   CHECK(npt_gsc_wsi_join(c, false) == S_OK);
   CHECK(test_count(f.test, TEST_TOKEN_RELEASE) == 1);
   CHECK(test_token_release_count(token) == 1);
   CHECK(test_fd_close_count(fd) == 1);
   CHECK(test_fd_dup_count(fd) == 1);
   common_fini(&f);
   test_end();
}

static void
cancel_pending_producer(void)
{
   struct common_fixture f;
   test_begin("common cancel pending producer");
   common_init(&f, true);
   struct npt_guest_swapchain_common *c = &f.common;
   CHECK(WaitForSingleObject(c->frame_latency_sem, 0) == WAIT_OBJECT_0);
   CHECK(npt_gsc_wsi_start(c));
   uint64_t token = npt_gsc_make_token();
   int fd = npt_event_arm_token_fd(test_device(f.test), token);
   CHECK(fd >= 0);
   CHECK(npt_gsc_wsi_push(c, 0, fd, token, 1, true));
   int latency_fd = c->latency[0].fd;
   CHECK(latency_fd >= 0);
   test_wait(f.test, TEST_GPU_POLL, 1);
   CHECK(test_count(f.test, TEST_PRESENT) == 0);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_QUEUED);
   CHECK(atomic_load(&c->slot_fence_val[0]) == 1);
   npt_gsc_wsi_stop_begin(c, false);
   CHECK(npt_gsc_wsi_join(c, false) == S_OK);
   CHECK(test_count(f.test, TEST_PRESENT) == 0);
   CHECK(test_count(f.test, TEST_TOKEN_RELEASE) == 1);
   CHECK(test_token_release_count(token) == 1);
   CHECK(test_fd_close_count(fd) == 1);
   CHECK(test_fd_dup_count(fd) == 1);
   CHECK(test_fd_close_count(latency_fd) == 0);
   npt_gsc_backpressure_flush(c);
   CHECK(test_fd_close_count(latency_fd) == 1);
   CHECK(atomic_load(&c->slot_state[0]) != NPT_GSC_SLOT_FREE);
   CHECK(WaitForSingleObject(c->frame_latency_sem, 0) == WAIT_OBJECT_0);
   CHECK(WaitForSingleObject(c->frame_latency_sem, 0) == WAIT_TIMEOUT);
   common_fini(&f);
   test_end();
}

static void
pending_producer_progress(void)
{
   struct common_fixture f;
   test_begin("common worker waits for producer completion");
   common_init(&f, true);
   common_renderer_init(&f);
   struct npt_guest_swapchain_common *c = &f.common;
   CHECK(npt_gsc_wsi_start(c));
   CHECK(npt_gsc_wsi_push(c, 0, -1, 0, 7, true));
   test_wait(f.test, TEST_GPU_POLL, 1);
   CHECK(test_count(f.test, TEST_PRESENT) == 0);
   CHECK(npt_gsc_present_would_block(c, 1));
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_QUEUED);
   test_set_gpu_completed(f.test, 0, 7);
   test_wait(f.test, TEST_PRESENT, 1);
   CHECK(atomic_load(&c->slot_completed_val[0]) == 7);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_SUBMITTED);
   CHECK(!npt_gsc_present_would_block(c, 1));
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_SUBMITTED);
   CHECK(npt_gsc_drain_start(c));
   test_release_image(f.test, 0, 1);
   CHECK(WaitForSingleObject(c->image_release_evt, 5000) == WAIT_OBJECT_0);
   for (uint64_t value = 8; value <= 13; ++value) {
      CHECK(npt_gsc_reserve_slot(c, 0, true) == S_OK);
      test_set_gpu_completed(f.test, 0, value);
      CHECK(npt_gsc_wsi_push(c, 0, -1, 0, value, true));
      test_wait(f.test, TEST_PRESENT, (uint32_t)(value - 6));
      CHECK(!npt_gsc_present_would_block(c, 1));
      test_release_image(f.test, 0, 1);
      CHECK(WaitForSingleObject(c->image_release_evt, 5000) == WAIT_OBJECT_0);
   }
   CHECK(test_count(f.test, TEST_PRESENT) == 7);
   common_fini(&f);
   test_end();
}

static void
terminal_drain(int result)
{
   struct common_fixture f;
   test_begin(result == -2 ? "common drain terminal -2" : "common drain unexpected -1");
   common_init(&f, false);
   common_renderer_init(&f);
   struct npt_guest_swapchain_common *c = &f.common;
   CHECK(npt_gsc_wsi_start(c));
   test_set_gpu_completed(f.test, 0, 9);
   CHECK(npt_gsc_wsi_push(c, 0, -1, 0, 9, true));
   test_wait(f.test, TEST_PRESENT, 1);
   CHECK(npt_gsc_drain_start(c));
   test_set_drain_result(f.test, result);
   test_wait(f.test, TEST_CANCEL, 1);
   CHECK(atomic_load(&c->gen_state) == NPT_GSC_GEN_WINDOW_LOST);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_SUBMITTED);
   CHECK(atomic_load(&c->slot_fence_val[0]) == 9);
   CHECK(npt_gsc_reserve_slot(c, 0, true) == NPT_DXGI_STATUS_OCCLUDED);
   common_fini(&f);
   test_end();
}

static void
terminal_worker(void)
{
   struct common_fixture f;
   test_begin("common worker terminal result drops queued work");
   common_init(&f, false);
   common_renderer_init(&f);
   struct npt_guest_swapchain_common *c = &f.common;
   test_set_present_result(f.test, -2);
   test_set_gpu_completed(f.test, 0, 1);
   test_set_gpu_completed(f.test, 1, 2);
   CHECK(npt_gsc_wsi_push(c, 0, -1, 0, 1, true));
   CHECK(npt_gsc_wsi_push(c, 1, -1, 0, 2, true));
   c->wsi_thread = CreateThread(NULL, 0, npt_gsc_wsi_thread, c, 0, NULL);
   CHECK(c->wsi_thread);
   c->wsi_thread_started = true;
   test_wait(f.test, TEST_CANCEL, 1);
   CHECK(npt_gsc_wsi_join(c, false) == S_OK);
   CHECK(test_count(f.test, TEST_PRESENT) == 1);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_SUBMITTED);
   CHECK(atomic_load(&c->slot_fence_val[0]) == 1);
   CHECK(npt_gsc_reserve_slot(c, 0, true) == NPT_DXGI_STATUS_OCCLUDED);
   common_fini(&f);
   test_end();
}

static void
release_state_discipline(void)
{
   struct common_fixture f;
   test_begin("common release preserves reserved and queued slots");
   common_init(&f, false);
   common_renderer_init(&f);
   struct npt_guest_swapchain_common *c = &f.common;
   atomic_store(&c->slot_state[0], NPT_GSC_SLOT_RESERVED);
   atomic_store(&c->slot_state[1], NPT_GSC_SLOT_QUEUED);
   atomic_store(&c->slot_state[2], NPT_GSC_SLOT_SUBMITTED);
   CHECK(npt_gsc_drain_start(c));
   test_release_image(f.test, 0, 1);
   test_release_image(f.test, 1, 1);
   test_release_image(f.test, 2, 1);
   CHECK(WaitForSingleObject(c->image_release_evt, 5000) == WAIT_OBJECT_0);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_RESERVED);
   CHECK(atomic_load(&c->slot_state[1]) == NPT_GSC_SLOT_QUEUED);
   CHECK(atomic_load(&c->slot_state[2]) == NPT_GSC_SLOT_FREE);
   common_fini(&f);
   test_end();
}

static void
expected_drain_cancel(void)
{
   struct common_fixture f;
   test_begin("common expected drain cancellation preserves failure");
   common_init(&f, false);
   common_renderer_init(&f);
   struct npt_guest_swapchain_common *c = &f.common;
   atomic_store(&c->slot_state[0], NPT_GSC_SLOT_SUBMITTED);
   CHECK(npt_gsc_drain_start(c));
   test_wait(f.test, TEST_DRAIN_ENTER, 1);
   HRESULT error = (HRESULT)0x887a0005u;
   atomic_store(&c->gen_error_hr, (int)error);
   atomic_store(&c->gen_state, NPT_GSC_GEN_FAILED);
   npt_gsc_wsi_stop_begin(c, false);
   CHECK(npt_gsc_wsi_join(c, false) == S_OK);
   CHECK(test_count(f.test, TEST_DRAIN) == 1);
   CHECK(atomic_load(&c->gen_state) == NPT_GSC_GEN_FAILED);
   CHECK((HRESULT)atomic_load(&c->gen_error_hr) == error);
   CHECK(atomic_load(&c->slot_state[0]) == NPT_GSC_SLOT_SUBMITTED);
   common_fini(&f);
   test_end();
}

static void
source_format_preserved(void)
{
   test_begin("common preserves presentation byte order and sRGB format");
   const uint32_t formats[][2] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM},
      {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM},
      {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
      {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM},
      {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
      {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB},
   };
   for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      struct npt_gsc_format_plan plan;
      CHECK(npt_gsc_get_format_plan(formats[i][0],
                                    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &plan));
      CHECK(plan.pool_format == formats[i][1]);
      CHECK(!plan.needs_convert);
   }
   test_end();
}

extern struct npt_unix_wsi_hdr_caps_args test_hdr_caps;
extern struct npt_unix_hdr10_metadata test_hdr_metadata;
extern int test_hdr_metadata_result;
extern unsigned test_hdr_metadata_calls;

static void
resize_color_spaces(void)
{
   test_begin("resize format and color space transitions");
   const struct {
      uint32_t old_format, new_format;
      DXGI_COLOR_SPACE_TYPE old_cs, expected_cs;
   } cases[] = {
      {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
       DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
       DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
       DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709},
      {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
       DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709},
      {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
       DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
       DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709},
      {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_TYPELESS,
       DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709},
   };
   test_hdr_caps.hdr_supported = 1;
   test_hdr_caps.format_flags = NPT_HDR_FMT_R10G10B10A2 | NPT_HDR_FMT_R16G16B16A16F;
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      struct npt_gsc_format_plan plan;
      DXGI_COLOR_SPACE_TYPE cs = npt_gsc_color_space_for_resize(
         cases[i].old_format, cases[i].new_format, cases[i].old_cs);
      CHECK(cs == cases[i].expected_cs);
      CHECK(npt_gsc_get_format_plan(cases[i].new_format, cs, &plan));
      if (cases[i].new_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
          cases[i].new_format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
         CHECK(plan.pool_format == DXGI_FORMAT_R16G16B16A16_FLOAT && !plan.needs_convert);
   }
   test_hdr_caps = (struct npt_unix_wsi_hdr_caps_args){0};
   CHECK(npt_gsc_color_space_for_resize(DXGI_FORMAT_R10G10B10A2_UNORM,
                                      DXGI_FORMAT_R10G10B10A2_UNORM,
                                      DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ==
         DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
   test_hdr_caps.hdr_supported = 1;
   CHECK(npt_gsc_color_space_for_resize(DXGI_FORMAT_R10G10B10A2_UNORM,
                                      DXGI_FORMAT_R10G10B10A2_UNORM,
                                      DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ==
         DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
   test_hdr_caps = (struct npt_unix_wsi_hdr_caps_args){0};
   test_end();
}

static void
hdr_behavior(void)
{
   struct common_fixture f;
   struct npt_gsc_format_plan plan;
   test_begin("HDR format capabilities and metadata");
   common_init(&f, false);
   CHECK(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 == 1);
   CHECK(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 == 12);
   CHECK(NPT_DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 == 1);
   CHECK(NPT_DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 == 12);
   test_hdr_caps.hdr_supported = 1;
   test_hdr_caps.format_flags = NPT_HDR_FMT_R10G10B10A2;
   CHECK(npt_gsc_get_format_plan(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &plan));
   CHECK(plan.pool_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && plan.needs_convert);
   CHECK(npt_gsc_get_format_plan(DXGI_FORMAT_R10G10B10A2_UNORM,
                                DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &plan));
   CHECK(plan.pool_format == DXGI_FORMAT_R10G10B10A2_UNORM && !plan.needs_convert);
   test_hdr_caps.format_flags = NPT_HDR_FMT_R16G16B16A16F;
   CHECK(!npt_gsc_get_format_plan(DXGI_FORMAT_R10G10B10A2_UNORM,
                                  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &plan));
   test_hdr_caps.hdr_supported = 0;
   test_hdr_caps.format_flags = NPT_HDR_FMT_R10G10B10A2;
   CHECK(!npt_gsc_get_format_plan(DXGI_FORMAT_R10G10B10A2_TYPELESS,
                                  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &plan));
   test_hdr_caps.hdr_supported = 1;
   test_hdr_caps.format_flags = NPT_HDR_FMT_R16G16B16A16F;
   CHECK(npt_gsc_get_format_plan(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &plan));
   CHECK(plan.pool_format == DXGI_FORMAT_R16G16B16A16_FLOAT && !plan.needs_convert);
   common_renderer_init(&f);
   f.common.wsi_hdr = true;
   f.common.color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
   test_hdr_metadata_calls = 0;
   CHECK(npt_gsc_SetHDRMetaData(&f.com, DXGI_HDR_METADATA_TYPE_NONE, 0, NULL) == NPT_S_OK);
   CHECK(test_hdr_metadata_calls == 1 && test_hdr_metadata.valid &&
         test_hdr_metadata.max_mastering_luminance == 1000);
   DXGI_HDR_METADATA_HDR10 metadata = {0};
   metadata.MaxMasteringLuminance = 700;
   CHECK(npt_gsc_SetHDRMetaData(&f.com, DXGI_HDR_METADATA_TYPE_HDR10,
                                sizeof(metadata), &metadata) == NPT_S_OK);
   CHECK(test_hdr_metadata_calls == 2 && test_hdr_metadata.max_mastering_luminance == 700);
   CHECK(npt_gsc_SetHDRMetaData(&f.com, DXGI_HDR_METADATA_TYPE_HDR10,
                                sizeof(metadata) - 1, &metadata) == NPT_DXGI_ERROR_INVALID_CALL);
   CHECK(test_hdr_metadata_calls == 2);
   test_hdr_metadata_result = -1;
   CHECK(npt_gsc_SetHDRMetaData(&f.com, DXGI_HDR_METADATA_TYPE_NONE, 0, NULL) == NPT_E_FAIL);
   CHECK(f.common.hdr_meta.valid && f.common.hdr_meta.max_mastering_luminance == 700);
   test_hdr_metadata_result = 0;
   test_hdr_caps = (struct npt_unix_wsi_hdr_caps_args){0};
   common_fini(&f);
   test_end();
}

void
test_common_group(void)
{
   hdr_behavior();
   resize_color_spaces();
   source_format_preserved();
   latency_marker_not_release();
   concurrent_failure_latch();
   stop_during_queued_token_cleanup();
   cancel_pending_producer();
   pending_producer_progress();
   terminal_drain(-2);
   terminal_drain(-1);
   terminal_worker();
   release_state_discipline();
   expected_drain_cancel();
}
