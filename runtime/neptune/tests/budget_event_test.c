#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "../npt_com.h"
#undef NPT_REGISTER_OVERRIDE
#define NPT_REGISTER_OVERRIDE(iface, method, func) ((void)0)
#include "../npt_overrides_event_handles.c"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

struct npt_env npt_env;
void npt_env_init(void) {}
void npt_log_impl(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
   SetLastError(0x12345678);
}

int main(void)
{
   HANDLE first = CreateEventW(NULL, TRUE, FALSE, NULL);
   HANDLE second = CreateEventW(NULL, TRUE, FALSE, NULL);
   DWORD a = 0, b = 0;
   CHECK(first && second);
   npt_env.debug = NPT_DEBUG_API_TRACE;
   SetLastError(0x87654321);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, NULL, &a) == NPT_E_INVALIDARG);
   CHECK(GetLastError() == 0x87654321);
   SetLastError(0x87654321);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, first, NULL) == NPT_E_INVALIDARG);
   CHECK(GetLastError() == 0x87654321);
   DWORD invalid_cookie = 0;
   DWORD next = atomic_load(&budget_cookie_next);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, (HANDLE)(uintptr_t)0xdeadbeef, &invalid_cookie) == NPT_E_INVALIDARG);
   CHECK(invalid_cookie == 0);
   CHECK(atomic_load(&budget_cookie_next) == next);
   CHECK(WaitForSingleObject(first, 0) == WAIT_TIMEOUT);
   SetLastError(0x87654321);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, first, &a) == NPT_S_OK);
   CHECK(GetLastError() == 0x87654321);
   CHECK(WaitForSingleObject(first, 0) == WAIT_OBJECT_0);
   CHECK(WaitForSingleObject(second, 0) == WAIT_TIMEOUT);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, second, &b) == NPT_S_OK);
   CHECK(WaitForSingleObject(second, 0) == WAIT_OBJECT_0);
   CHECK(a != 0 && b != 0 && a != b);
   SetLastError(0x87654321);
   adp3_UnregisterVideoMemoryBudgetChangeNotification_override(NULL, a);
   CHECK(GetLastError() == 0x87654321);
   adp3_UnregisterVideoMemoryBudgetChangeNotification_override(NULL, b);
   atomic_store(&budget_cookie_next, UINT32_MAX);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, first, &a) == NPT_S_OK);
   CHECK(a == UINT32_MAX);
   CHECK(adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(NULL, second, &b) == NPT_S_OK);
   CHECK(b == 1);
   CHECK(CloseHandle(first));
   CHECK(CloseHandle(second));
   puts("PASS budget event notification");
   return 0;
}
