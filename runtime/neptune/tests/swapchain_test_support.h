/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#ifndef SWAPCHAIN_TEST_SUPPORT_H
#define SWAPCHAIN_TEST_SUPPORT_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../npt_swapchain_common.h"
#include "../npt_device.h"
#include "../npt_renderer.h"

#define CHECK(expr) do { \
   if (!(expr)) { \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
      abort(); \
   } \
} while (0)

struct test_fixture;

enum test_event {
   TEST_GPU_POLL,
   TEST_PRESENT,
   TEST_DRAIN,
   TEST_CANCEL,
   TEST_TOKEN_RELEASE,
   TEST_DRAIN_ENTER,
   TEST_EVENT_COUNT
};

void test_common_group(void);
void test_d3d11_group(void);
void test_d3d12_group(void);

void test_begin(const char *name);
void test_end(void);
struct test_fixture *test_fixture_create(void);
void test_fixture_destroy(struct test_fixture *f);
void test_fixture_bind(struct test_fixture *f, struct npt_guest_swapchain_common *c,
                       struct npt_com_base *com);
struct test_fixture *test_active_fixture(void);
struct npt_guest_swapchain_common *test_bound_common(struct test_fixture *f);
struct npt_device *test_device(struct test_fixture *f);
struct npt_renderer *test_renderer(struct test_fixture *f);
struct npt_ring *test_ring(struct test_fixture *f);
void test_observe(struct test_fixture *f, enum test_event event);
void test_wait(struct test_fixture *f, enum test_event event, uint32_t count);
uint32_t test_count(struct test_fixture *f, enum test_event event);
void test_set_gpu_completed(struct test_fixture *f, uint32_t image, uint64_t value);
uint64_t test_gpu_completed(struct test_fixture *f, uint32_t image);
HRESULT test_gpu_poll(void *ctx, uint32_t image, uint64_t value);
void test_enter_nonblocking(struct test_fixture *f);
void test_leave_nonblocking(struct test_fixture *f);

void *test_object_create(struct test_fixture *f, size_t size, const void **vtbl,
                         uint64_t id);
bool test_object_alive(struct test_fixture *f, const void *ptr);
uint32_t test_object_release_count(struct test_fixture *f, const void *ptr);
void test_object_release(struct test_fixture *f, void *ptr);

/* Implemented by the transport/renderer boundary module. */
void test_set_present_result(struct test_fixture *f, int result);
void test_set_drain_result(struct test_fixture *f, int result);
void test_release_image(struct test_fixture *f, uint32_t image, uint32_t count);
void test_set_arm_failure(struct test_fixture *f, bool fail);
void test_set_dup_failure(struct test_fixture *f, bool fail);
int test_fd_create(struct test_fixture *f, bool ready);
uint32_t test_command_count(struct test_fixture *f, uint32_t command);
uint64_t test_last_command(struct test_fixture *f, uint32_t command);
void test_install_renderer(struct test_fixture *f);
void test_hold_token_release(bool hold);
void test_unblock_token_release(void);
unsigned test_fd_close_count(int fd);
unsigned test_fd_dup_count(int fd);
unsigned test_token_release_count(uint64_t token);

void WINAPI test_Sleep(DWORD milliseconds);
DWORD WINAPI test_WaitForSingleObject(HANDLE handle, DWORD milliseconds);
HANDLE WINAPI test_CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size,
                                LPTHREAD_START_ROUTINE start, LPVOID parameter,
                                DWORD flags, LPDWORD thread_id);
BOOL WINAPI test_CloseHandle(HANDLE handle);
BOOL WINAPI test_ReleaseSemaphore(HANDLE semaphore, LONG release_count,
                                  LPLONG previous_count);
ULONGLONG WINAPI test_GetTickCount64(void);
void WINAPI test_EnterCriticalSection(LPCRITICAL_SECTION section);
BOOL WINAPI test_SleepConditionVariableCS(PCONDITION_VARIABLE condition,
                                          PCRITICAL_SECTION section,
                                          DWORD milliseconds);

#ifdef TEST_INTERCEPT_WIN32
#define Sleep test_Sleep
#define WaitForSingleObject test_WaitForSingleObject
#define CreateThread test_CreateThread
#define CloseHandle test_CloseHandle
#define ReleaseSemaphore test_ReleaseSemaphore
#define GetTickCount64 test_GetTickCount64
#define EnterCriticalSection test_EnterCriticalSection
#define SleepConditionVariableCS test_SleepConditionVariableCS
#endif
#endif
