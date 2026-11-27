/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */
#include "swapchain_test_support.h"

#include <stdatomic.h>

struct test_object {
   struct test_object *next;
   void *ptr;
   uint32_t release_count;
   bool alive;
};

struct test_fixture {
   struct npt_device device;
   struct npt_renderer renderer;
   struct npt_ring ring;
   struct npt_guest_swapchain_common *common;
   _Atomic uint32_t counts[TEST_EVENT_COUNT];
   HANDLE events[TEST_EVENT_COUNT];
   _Atomic uint64_t completed[NPT_SC_PRESENT_IMAGES];
   _Atomic DWORD nonblocking_thread;
   CRITICAL_SECTION objects_lock;
   struct test_object *objects;
};

static _Atomic(struct test_fixture *) active_fixture;
static const char *current_test;

void
test_begin(const char *name)
{
   CHECK(name && !current_test);
   current_test = name;
   printf("BEGIN %s\n", name);
   fflush(stdout);
}

void
test_end(void)
{
   CHECK(current_test);
   printf("END %s\n", current_test);
   fflush(stdout);
   current_test = NULL;
}

struct test_fixture *
test_fixture_create(void)
{
   struct test_fixture *f = calloc(1, sizeof(*f));
   CHECK(f);
   InitializeCriticalSection(&f->objects_lock);
   for (unsigned i = 0; i < TEST_EVENT_COUNT; ++i) {
      atomic_init(&f->counts[i], 0);
      f->events[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
      CHECK(f->events[i]);
   }
   for (unsigned i = 0; i < NPT_SC_PRESENT_IMAGES; ++i)
      atomic_init(&f->completed[i], 0);
   atomic_init(&f->nonblocking_thread, 0);
   f->device.renderer = &f->renderer;
   f->device.ring = &f->ring;
   struct test_fixture *expected = NULL;
   CHECK(atomic_compare_exchange_strong(&active_fixture, &expected, f));
   return f;
}

void
test_fixture_destroy(struct test_fixture *f)
{
   CHECK(f && atomic_load(&active_fixture) == f);
   CHECK(!atomic_load(&f->nonblocking_thread));
   atomic_store(&active_fixture, NULL);
   for (unsigned i = 0; i < TEST_EVENT_COUNT; ++i)
      CHECK(CloseHandle(f->events[i]));
   struct test_object *object = f->objects;
   while (object) {
      struct test_object *next = object->next;
      free(object->ptr);
      free(object);
      object = next;
   }
   DeleteCriticalSection(&f->objects_lock);
   free(f);
}

void
test_fixture_bind(struct test_fixture *f, struct npt_guest_swapchain_common *c,
                  struct npt_com_base *com)
{
   CHECK(f && c && com);
   f->common = c;
   c->com = com;
   com->base.device = &f->device;
   com->base.instance_ring = &f->ring;
   com->aux = c;
}

struct test_fixture *
test_active_fixture(void)
{
   return atomic_load(&active_fixture);
}

struct npt_device *test_device(struct test_fixture *f) { return &f->device; }
struct npt_renderer *test_renderer(struct test_fixture *f) { return &f->renderer; }
struct npt_ring *test_ring(struct test_fixture *f) { return &f->ring; }
struct npt_guest_swapchain_common *test_bound_common(struct test_fixture *f) { return f->common; }

void
test_observe(struct test_fixture *f, enum test_event event)
{
   CHECK(f && event >= 0 && event < TEST_EVENT_COUNT);
   atomic_fetch_add(&f->counts[event], 1);
   CHECK(SetEvent(f->events[event]));
}

uint32_t
test_count(struct test_fixture *f, enum test_event event)
{
   CHECK(f && event >= 0 && event < TEST_EVENT_COUNT);
   return atomic_load(&f->counts[event]);
}

void
test_wait(struct test_fixture *f, enum test_event event, uint32_t count)
{
   ULONGLONG deadline = GetTickCount64() + 5000;
   CHECK(f && event >= 0 && event < TEST_EVENT_COUNT);
   while (test_count(f, event) < count) {
      CHECK(ResetEvent(f->events[event]));
      if (test_count(f, event) >= count)
         break;
      ULONGLONG now = GetTickCount64();
      CHECK(now < deadline);
      CHECK(WaitForSingleObject(f->events[event], (DWORD)(deadline - now)) ==
            WAIT_OBJECT_0);
   }
}

void
test_set_gpu_completed(struct test_fixture *f, uint32_t image, uint64_t value)
{
   CHECK(f && image < NPT_SC_PRESENT_IMAGES);
   atomic_store(&f->completed[image], value);
}

uint64_t
test_gpu_completed(struct test_fixture *f, uint32_t image)
{
   CHECK(f && image < NPT_SC_PRESENT_IMAGES);
   return atomic_load(&f->completed[image]);
}

HRESULT
test_gpu_poll(void *ctx, uint32_t image, uint64_t value)
{
   struct test_fixture *f = ctx;
   CHECK(f && image < NPT_SC_PRESENT_IMAGES);
   test_observe(f, TEST_GPU_POLL);
   return atomic_load(&f->completed[image]) >= value ? S_OK : S_FALSE;
}

void
test_enter_nonblocking(struct test_fixture *f)
{
   DWORD expected = 0;
   CHECK(f && atomic_compare_exchange_strong(&f->nonblocking_thread, &expected,
                                               GetCurrentThreadId()));
}

void
test_leave_nonblocking(struct test_fixture *f)
{
   DWORD expected = GetCurrentThreadId();
   CHECK(f && atomic_compare_exchange_strong(&f->nonblocking_thread, &expected, 0));
}

static bool
on_nonblocking_thread(void)
{
   struct test_fixture *f = test_active_fixture();
   return f && atomic_load(&f->nonblocking_thread) == GetCurrentThreadId();
}

static struct test_object *
find_object(struct test_fixture *f, const void *ptr)
{
   for (struct test_object *object = f->objects; object; object = object->next) {
      if (object->ptr == ptr)
         return object;
   }
   CHECK(false);
   return NULL;
}

void *
test_object_create(struct test_fixture *f, size_t size, const void **vtbl,
                   uint64_t id)
{
   CHECK(f && size >= sizeof(struct npt_com_base) && id);
   struct test_object *object = calloc(1, sizeof(*object));
   CHECK(object);
   object->ptr = calloc(1, size);
   CHECK(object->ptr);
   object->alive = true;
   npt_com_base_init(object->ptr, vtbl, &f->device, id);
   ((struct npt_com_base *)object->ptr)->base.instance_ring = &f->ring;
   EnterCriticalSection(&f->objects_lock);
   object->next = f->objects;
   f->objects = object;
   LeaveCriticalSection(&f->objects_lock);
   return object->ptr;
}

bool
test_object_alive(struct test_fixture *f, const void *ptr)
{
   CHECK(f && ptr);
   EnterCriticalSection(&f->objects_lock);
   bool alive = find_object(f, ptr)->alive;
   LeaveCriticalSection(&f->objects_lock);
   return alive;
}

uint32_t
test_object_release_count(struct test_fixture *f, const void *ptr)
{
   CHECK(f && ptr);
   EnterCriticalSection(&f->objects_lock);
   uint32_t count = find_object(f, ptr)->release_count;
   LeaveCriticalSection(&f->objects_lock);
   return count;
}

void
test_object_release(struct test_fixture *f, void *ptr)
{
   CHECK(f && ptr);
   EnterCriticalSection(&f->objects_lock);
   struct test_object *object = find_object(f, ptr);
   CHECK(object->alive);
   ++object->release_count;
   object->alive = false;
   LeaveCriticalSection(&f->objects_lock);
}

void WINAPI
test_Sleep(DWORD milliseconds)
{
   CHECK(!milliseconds || !on_nonblocking_thread());
   Sleep(milliseconds);
}

DWORD WINAPI
test_WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
   CHECK(!milliseconds || !on_nonblocking_thread());
   return WaitForSingleObject(handle, milliseconds);
}

HANDLE WINAPI
test_CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size,
                  LPTHREAD_START_ROUTINE start, LPVOID parameter,
                  DWORD flags, LPDWORD thread_id)
{
   return CreateThread(attributes, stack_size, start, parameter, flags, thread_id);
}

BOOL WINAPI test_CloseHandle(HANDLE handle) { return CloseHandle(handle); }
BOOL WINAPI
test_ReleaseSemaphore(HANDLE semaphore, LONG release_count, LPLONG previous_count)
{
   return ReleaseSemaphore(semaphore, release_count, previous_count);
}
ULONGLONG WINAPI test_GetTickCount64(void) { return GetTickCount64(); }

void WINAPI
test_EnterCriticalSection(LPCRITICAL_SECTION section)
{
   if (on_nonblocking_thread()) {
      CHECK(TryEnterCriticalSection(section));
      return;
   }
   EnterCriticalSection(section);
}

BOOL WINAPI
test_SleepConditionVariableCS(PCONDITION_VARIABLE condition,
                              PCRITICAL_SECTION section, DWORD milliseconds)
{
   CHECK(!milliseconds || !on_nonblocking_thread());
   return SleepConditionVariableCS(condition, section, milliseconds);
}
