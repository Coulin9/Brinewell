/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_API_TRACE_H
#define NPT_API_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#include "npt_env.h"

static inline bool
npt_api_trace_enabled(void)
{
   npt_env_init();
   return NPT_DEBUG(API_TRACE);
}

/* The condition also prevents evaluation of potentially expensive arguments. */
#define NPT_API_TRACE(fmt, ...)                                             \
   do {                                                                     \
      if (npt_api_trace_enabled())                                          \
         npt_log("NPT_API_TRACE tid=%lu " fmt,                              \
                 (unsigned long)GetCurrentThreadId(), ##__VA_ARGS__);       \
   } while (0)

static inline void
npt_api_trace_guid(const char *api, const char *field, const GUID *iid)
{
   if (!npt_api_trace_enabled())
      return;

   if (!iid) {
      NPT_API_TRACE("api=%s %s=NULL", api, field);
      return;
   }

   GUID local;
   SIZE_T bytes = 0;
   DWORD last_error = GetLastError();
   BOOL readable = ReadProcessMemory(GetCurrentProcess(), iid, &local,
                                     sizeof(local), &bytes);
   if (!readable || bytes != sizeof(local)) {
      NPT_API_TRACE("api=%s %s iid_ptr=%p unreadable", api, field,
                    (const void *)iid);
      SetLastError(last_error);
      return;
   }

   char text[37];
   snprintf(text, sizeof(text),
            "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            (unsigned)local.Data1, (unsigned)local.Data2, (unsigned)local.Data3,
            (unsigned)local.Data4[0], (unsigned)local.Data4[1],
            (unsigned)local.Data4[2], (unsigned)local.Data4[3],
            (unsigned)local.Data4[4], (unsigned)local.Data4[5],
            (unsigned)local.Data4[6], (unsigned)local.Data4[7]);
   NPT_API_TRACE("api=%s %s=%s", api, field, text);
   SetLastError(last_error);
}

static inline void
npt_api_trace_bytes(const char *api, const char *phase,
                    const void *data, size_t size)
{
   if (!npt_api_trace_enabled())
      return;

   if (!data) {
      NPT_API_TRACE("api=%s phase=%s data=NULL total=%zu", api, phase, size);
      return;
   }

   const uint8_t *bytes = data;
   static const char hex[] = "0123456789abcdef";
   for (size_t offset = 0; offset < size;) {
      size_t count = size - offset;
      if (count > 32)
         count = 32;
      char text[65];
      for (size_t i = 0; i < count; i++) {
         text[i * 2] = hex[bytes[offset + i] >> 4];
         text[i * 2 + 1] = hex[bytes[offset + i] & 0xf];
      }
      text[count * 2] = '\0';
      NPT_API_TRACE("api=%s phase=%s offset=%zu total=%zu hex=%s",
                    api, phase, offset, size, text);
      offset += count;
   }
}

#endif /* NPT_API_TRACE_H */
