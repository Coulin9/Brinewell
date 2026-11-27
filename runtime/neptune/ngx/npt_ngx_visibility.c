/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Process-side validity check for the NGX SR deployment; see the
 * header for the contract.  The verification runs once per process
 * (InitOnceExecuteOnce / pthread_once) so no consumer hashes per call,
 * and all three consumers (DXGI identity, nvapi Initialize, bridge QI)
 * observe the same latched answer regardless of call order.  Nothing
 * here loads a module: GetModuleHandleW only inspects what the loader
 * already mapped, and no I/O happens at DllMain time.
 */

#include "npt_ngx_visibility.h"
#include "npt_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <wchar.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#define VIS_ACTIVE_VAR    "BRINEWELL_NGX_SR_ACTIVE"
#define VIS_CANDIDATE_VAR "BRINEWELL_NGX_SR_CANDIDATE"
#define VIS_BACKEND_VAR   "BRINEWELL_NEPTUNE_BACKEND"
#define VIS_CORE_HASH_VAR "BRINEWELL_NGX_CORE_SHA256"
#define VIS_NVAPI_HASH_VAR "BRINEWELL_NGX_NVAPI_SHA256"

#if defined(__x86_64__) || defined(_M_X64) || defined(NPT_NGX_TEST_X64)
#define VIS_ARCH_X64 1
#else
#define VIS_ARCH_X64 0
#endif

static void
vis_log(const char *stage, const char *detail)
{
#if defined(_WIN32)
   char buf[256];
   _snprintf(buf, sizeof(buf), "neptune: ngx-sr: %s %s",
             stage, detail);
   buf[sizeof(buf) - 1] = 0;
   OutputDebugStringA(buf);
#else
   fprintf(stderr, "neptune: ngx-sr inactive: %s %s\n", stage, detail);
#endif
}

#if defined(_WIN32) || defined(NPT_NGX_TEST_HOOKS)
struct vis_registry_ops {
   int (*read)(void *ctx, unsigned index, const char *name,
               uint32_t *type, uint8_t **data, uint32_t *size);
   int (*write)(void *ctx, unsigned index, const char *name,
                uint32_t type, const void *data, uint32_t size);
   int (*remove)(void *ctx, unsigned index, const char *name);
};

static int
vis_read_u32(const struct vis_registry_ops *ops, void *ctx, unsigned i,
             const char *name, uint32_t *value)
{
   uint8_t *data = NULL;
   uint32_t type = 0, size = 0;
   int rc = ops->read(ctx, i, name, &type, &data, &size);
   if (rc == 1) {
      if (type != 4 || size != sizeof(*value))
         rc = -1;
      else
         memcpy(value, data, sizeof(*value));
   }
   free(data);
   return rc;
}

static bool
vis_write_u32(const struct vis_registry_ops *ops, void *ctx, unsigned i,
              const char *name, uint32_t value)
{
   return ops->write(ctx, i, name, 4, &value, sizeof(value)) == 1;
}

static void
vis_manage_value(const struct vis_registry_ops *ops, void *ctx, unsigned i,
                 bool enable, uint32_t wanted_type, const void *wanted,
                 uint32_t wanted_size)
{
   uint32_t managed = 0, type = 0, size = 0;
   uint8_t *data = NULL;
   int rc = vis_read_u32(ops, ctx, i, "Managed", &managed);
   if (rc < 0 || (rc == 1 && managed != 0 && managed != 1))
      return;
   if (enable) {
      if (managed != 1) {
         rc = ops->read(ctx, i, NULL, &type, &data, &size);
         if (rc < 0)
            return;
         if (rc == 1 && type == wanted_type && size == wanted_size &&
             !memcmp(data, wanted, size)) {
            free(data);
            return;
         }
         if (!vis_write_u32(ops, ctx, i, "HadOrig", rc == 1) ||
             (rc == 1 && (!vis_write_u32(ops, ctx, i, "OrigType", type) ||
              ops->write(ctx, i, "OrigData", 3, data, size) != 1))) {
            free(data);
            return;
         }
         free(data);
         if (!vis_write_u32(ops, ctx, i, "Managed", 1))
            return;
      }
      ops->write(ctx, i, NULL, wanted_type, wanted, wanted_size);
      return;
   }
   if (managed != 1)
      return;
   rc = ops->read(ctx, i, NULL, &type, &data, &size);
   if (rc < 0)
      return;
   bool matches = rc == 1 && type == wanted_type && size == wanted_size &&
                  !memcmp(data, wanted, size);
   free(data);
   if (matches) {
      uint32_t had_orig = 0;
      if (vis_read_u32(ops, ctx, i, "HadOrig", &had_orig) != 1 || had_orig > 1)
         return;
      if (had_orig) {
         uint32_t orig_type, backup_type, backup_size;
         uint8_t *backup = NULL;
         if (vis_read_u32(ops, ctx, i, "OrigType", &orig_type) != 1 ||
             ops->read(ctx, i, "OrigData", &backup_type, &backup, &backup_size) != 1)
            return;
         if (backup_type != 3 ||
             ops->write(ctx, i, NULL, orig_type, backup, backup_size) != 1) {
            free(backup);
            return;
         }
         free(backup);
      } else if (ops->remove(ctx, i, NULL) < 0) {
         return;
      }
   }
   if (ops->remove(ctx, i, "Managed") < 0)
      return;
   ops->remove(ctx, i, "HadOrig");
   ops->remove(ctx, i, "OrigType");
   ops->remove(ctx, i, "OrigData");
}

#if defined(NPT_NGX_TEST_HOOKS)
void
npt_ngx_vis_test_registry(const struct vis_registry_ops *ops, void *ctx,
                          unsigned i, bool enable, uint32_t type,
                          const void *data, uint32_t size)
{
   vis_manage_value(ops, ctx, i, enable, type, data, size);
}
#endif
#endif

#if defined(_WIN32)
struct vis_win_registry {
   LSTATUS (WINAPI *open)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
   LSTATUS (WINAPI *create)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                            const SECURITY_ATTRIBUTES *, PHKEY, LPDWORD);
   LSTATUS (WINAPI *query)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
   LSTATUS (WINAPI *set)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
   LSTATUS (WINAPI *del_value)(HKEY, LPCWSTR);
   LSTATUS (WINAPI *query_info)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD,
                                 LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD,
                                 LPDWORD, PFILETIME);
   LSTATUS (WINAPI *del_key)(HKEY, LPCWSTR);
   LSTATUS (WINAPI *close)(HKEY);
};

static const WCHAR *const vis_reg_paths[] = {
   L"SOFTWARE\\NVIDIA Corporation\\Global",
   L"SYSTEM\\ControlSet001\\Services\\nvlddmkm",
   L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore"
};
static const WCHAR vis_reg_backup[] = L"SOFTWARE\\Brinewell\\NGX";
static const WCHAR *const vis_reg_prefix[] = {L"Global", L"Nvlddmkm", L"FullPath"};
static const WCHAR vis_reg_signature[] = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";

static void
vis_reg_status(const char *operation, LSTATUS status)
{
   char detail[128];
   snprintf(detail, sizeof(detail), "%s status %ld", operation, (long)status);
   vis_log("registry", detail);
}

static LPCWSTR
vis_reg_name(unsigned i, const char *name, WCHAR buffer[64])
{
   if (!name)
      return i == 2 ? L"FullPath" : vis_reg_signature;
   size_t n = wcslen(vis_reg_prefix[i]);
   memcpy(buffer, vis_reg_prefix[i], n * sizeof(WCHAR));
   while (*name && n < 63)
      buffer[n++] = (WCHAR)(unsigned char)*name++;
   buffer[n] = 0;
   return buffer;
}

static int
vis_reg_open(struct vis_win_registry *r, unsigned i, const char *name,
             bool create, HKEY *key)
{
   const WCHAR *path = name ? vis_reg_backup : vis_reg_paths[i];
   LSTATUS status = create ? r->create(HKEY_LOCAL_MACHINE, path, 0, NULL, 0,
                                       KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, key, NULL) :
                             r->open(HKEY_LOCAL_MACHINE, path, 0,
                                     KEY_QUERY_VALUE | KEY_SET_VALUE, key);
   vis_reg_status(create ? "RegCreateKeyExW" : "RegOpenKeyExW", status);
   if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
      return 0;
   return status == ERROR_SUCCESS ? 1 : -1;
}

static void
vis_reg_close(struct vis_win_registry *r, HKEY key)
{
   LSTATUS status = r->close(key);
   vis_reg_status("RegCloseKey", status);
}

static int
vis_reg_read(void *ctx, unsigned i, const char *name, uint32_t *type,
             uint8_t **data, uint32_t *size)
{
   struct vis_win_registry *r = ctx;
   HKEY key;
   int rc = vis_reg_open(r, i, name, false, &key);
   if (rc != 1)
      return rc;
   WCHAR buffer[64];
   LPCWSTR value = vis_reg_name(i, name, buffer);
   DWORD t = 0, n = 0;
   LSTATUS status = r->query(key, value, NULL, &t, NULL, &n);
   vis_reg_status("RegQueryValueExW size", status);
   if (status == ERROR_SUCCESS) {
      uint8_t *bytes = malloc(n ? n : 1);
      if (!bytes) {
         vis_log("registry", "allocation failed");
         rc = -1;
      } else {
         DWORD actual = n;
         status = r->query(key, value, NULL, &t, bytes, &actual);
         vis_reg_status("RegQueryValueExW data", status);
         if (status == ERROR_SUCCESS) {
            *type = t; *size = actual; *data = bytes;
            rc = 1;
         } else {
            free(bytes);
            rc = -1;
         }
      }
   } else {
      rc = status == ERROR_FILE_NOT_FOUND ? 0 : -1;
   }
   vis_reg_close(r, key);
   return rc;
}

static int
vis_reg_write(void *ctx, unsigned i, const char *name, uint32_t type,
              const void *data, uint32_t size)
{
   struct vis_win_registry *r = ctx;
   HKEY key;
   int rc = vis_reg_open(r, i, name, true, &key);
   if (rc != 1)
      return -1;
   WCHAR buffer[64];
   LSTATUS status = r->set(key, vis_reg_name(i, name, buffer), 0, type, data, size);
   vis_reg_status("RegSetValueExW", status);
   vis_reg_close(r, key);
   return status == ERROR_SUCCESS ? 1 : -1;
}

static int
vis_reg_remove(void *ctx, unsigned i, const char *name)
{
   struct vis_win_registry *r = ctx;
   HKEY key;
   int rc = vis_reg_open(r, i, name, false, &key);
   if (rc != 1)
      return rc;
   WCHAR buffer[64];
   LSTATUS status = r->del_value(key, vis_reg_name(i, name, buffer));
   vis_reg_status("RegDeleteValueW", status);
   vis_reg_close(r, key);
   return status == ERROR_SUCCESS ? 1 : status == ERROR_FILE_NOT_FOUND ? 0 : -1;
}

static void
vis_registry_overrides(bool enable)
{
   HMODULE lib = LoadLibraryA("advapi32.dll");
   if (!lib) {
      vis_log("registry", "LoadLibraryA failed");
      return;
   }
   struct vis_win_registry r;
#define VIS_REG_PROC(member, name) r.member = (void *)GetProcAddress(lib, name)
   VIS_REG_PROC(open, "RegOpenKeyExW");
   VIS_REG_PROC(create, "RegCreateKeyExW");
   VIS_REG_PROC(query, "RegQueryValueExW");
   VIS_REG_PROC(set, "RegSetValueExW");
   VIS_REG_PROC(del_value, "RegDeleteValueW");
   VIS_REG_PROC(query_info, "RegQueryInfoKeyW");
   VIS_REG_PROC(del_key, "RegDeleteKeyW");
   VIS_REG_PROC(close, "RegCloseKey");
#undef VIS_REG_PROC
   if (!r.open || !r.create || !r.query || !r.set || !r.del_value ||
       !r.query_info || !r.del_key || !r.close) {
      vis_log("registry", "advapi32 entry point unavailable");
      FreeLibrary(lib);
      return;
   }
   HANDLE mutex = CreateMutexW(NULL, FALSE, L"Global\\BrinewellNGXRegistry");
   if (!mutex) {
      vis_log("registry", "CreateMutexW failed");
      FreeLibrary(lib);
      return;
   }
   DWORD wait = WaitForSingleObject(mutex, INFINITE);
   if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
      vis_log("registry", "WaitForSingleObject failed");
      CloseHandle(mutex);
      FreeLibrary(lib);
      return;
   }
   static const struct vis_registry_ops ops = {vis_reg_read, vis_reg_write, vis_reg_remove};
   static const uint32_t one = 1;
   static const WCHAR full_path[] = L"C:\\Windows\\System32";
   for (unsigned i = 0; i < 3; i++)
      vis_manage_value(&ops, &r, i, enable, i == 2 ? REG_SZ : REG_DWORD,
                       i == 2 ? (const void *)full_path : (const void *)&one,
                       i == 2 ? sizeof(full_path) : sizeof(one));
   if (!enable) {
      HKEY key;
      int opened = vis_reg_open(&r, 0, "Managed", false, &key);
      if (opened == 1) {
         DWORD subkeys = 0, values = 0;
         LSTATUS status = r.query_info(key, NULL, NULL, NULL, &subkeys, NULL,
                                       NULL, &values, NULL, NULL, NULL, NULL);
         vis_reg_status("RegQueryInfoKeyW", status);
         vis_reg_close(&r, key);
         if (status == ERROR_SUCCESS && !subkeys && !values) {
            status = r.del_key(HKEY_LOCAL_MACHINE, vis_reg_backup);
            vis_reg_status("RegDeleteKeyW", status);
         }
      }
   }
   if (!ReleaseMutex(mutex))
      vis_log("registry", "ReleaseMutex failed");
   if (!CloseHandle(mutex))
      vis_log("registry", "CloseHandle failed");
   if (!FreeLibrary(lib))
      vis_log("registry", "FreeLibrary failed");
}
#endif

static bool
vis_env_is(const char *name, const char *want)
{
#if defined(_WIN32)
   char buf[64];
   DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
   return n && n < sizeof(buf) && !strcmp(buf, want);
#else
   const char *s = getenv(name);
   return s && !strcmp(s, want);
#endif
}

static bool
vis_env_get(const char *name, char *out, size_t size)
{
#if defined(_WIN32)
   DWORD n = GetEnvironmentVariableA(name, out, (DWORD)size);
   return n && n < size;
#else
   const char *s = getenv(name);
   if (!s || strlen(s) >= size)
      return false;
   memcpy(out, s, strlen(s) + 1);
   return true;
#endif
}

static int
vis_hex_digit(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

static bool
vis_parse_hex32(const char *s, uint8_t out[32])
{
   for (unsigned i = 0; i < 32; i++) {
      int hi = vis_hex_digit(s[i * 2]);
      int lo = vis_hex_digit(s[i * 2 + 1]);
      if (hi < 0 || lo < 0)
         return false;
      out[i] = (uint8_t)((hi << 4) | lo);
   }
   return s[64] == 0;
}

#if defined(NPT_NGX_TEST_HOOKS)

static const char *t_dir;
static const char *t_mod_core;
static const char *t_mod_nvapi;
static npt_ngx_vis_hash_fn t_hash;

static bool
vis_hash_file(const char *path, uint8_t out[32])
{
   if (t_hash)
      return t_hash(path, out);
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   npt_sha256_ctx c;
   npt_sha256_init(&c);
   uint8_t buf[16384];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      npt_sha256_update(&c, buf, n);
   bool ok = ferror(f) == 0;
   fclose(f);
   if (!ok)
      return false;
   npt_sha256_final(&c, out);
   return true;
}

static bool
vis_module_path_ok(const char *path)
{
   if (!path)
      return true;
   size_t dl = strlen(t_dir);
   return strncmp(path, t_dir, dl) == 0 &&
          (path[dl] == '/' || path[dl] == '\\');
}

#else /* real lookup */

#if defined(_WIN32)
static bool
vis_hash_file(const WCHAR *path, uint8_t out[32])
{
   HANDLE f = CreateFileW(path, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE |
                          FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                          NULL);
   if (f == INVALID_HANDLE_VALUE)
      return false;
   npt_sha256_ctx c;
   npt_sha256_init(&c);
   uint8_t buf[65536];
   DWORD n;
   bool ok = true;
   while (ok) {
      if (!ReadFile(f, buf, sizeof(buf), &n, NULL)) {
         ok = false;
      } else if (n == 0) {
         break;
      } else {
         npt_sha256_update(&c, buf, n);
      }
   }
   CloseHandle(f);
   if (!ok)
      return false;
   npt_sha256_final(&c, out);
   return true;
}

static bool
vis_module_path_ok(const WCHAR *name, const WCHAR *dir)
{
   HMODULE m = GetModuleHandleW(name);
   if (!m)
      return true;
   WCHAR mp[4096];
   DWORD n = GetModuleFileNameW(m, mp, (DWORD)(sizeof(mp) / sizeof(mp[0])));
   if (!n || n >= sizeof(mp) / sizeof(mp[0]))
      return false;
   size_t dl = wcslen(dir);
   return _wcsnicmp(mp, dir, dl) == 0 &&
          (mp[dl] == L'\\' || mp[dl] == L'/');
}
#endif

#endif /* NPT_NGX_TEST_HOOKS */

static int g_vis_result;

static void
vis_verify(void)
{
   int result = 0;
#if defined(_WIN32)
   if (!vis_env_is(VIS_CANDIDATE_VAR, "1"))
      vis_registry_overrides(false);
#endif
#if !VIS_ARCH_X64
   vis_log("arch", "not-x64");
#else
   if (!vis_env_is(VIS_CANDIDATE_VAR, "1")) {
      vis_log("candidate", VIS_CANDIDATE_VAR);
      goto done;
   }
   if (!vis_env_is(VIS_BACKEND_VAR, "d3dmetal")) {
      vis_log("backend", VIS_BACKEND_VAR);
      goto done;
   }
   char hex[80];
   uint8_t want[2][32];
   if (!vis_env_get(VIS_CORE_HASH_VAR, hex, sizeof(hex)) ||
       !vis_parse_hex32(hex, want[0])) {
      vis_log("env", VIS_CORE_HASH_VAR);
      goto done;
   }
   if (!vis_env_get(VIS_NVAPI_HASH_VAR, hex, sizeof(hex)) ||
       !vis_parse_hex32(hex, want[1])) {
      vis_log("env", VIS_NVAPI_HASH_VAR);
      goto done;
   }

#if defined(NPT_NGX_TEST_HOOKS)
   static const char *const names[2] = {"nvngx.dll", "nvapi64.dll"};
   const char *mods[2] = {t_mod_core, t_mod_nvapi};
   if (!t_dir) {
      vis_log("systemdir", "unset");
      goto done;
   }
   for (unsigned i = 0; i < 2; i++) {
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", t_dir, names[i]);
      uint8_t got[32];
      if (!vis_hash_file(path, got)) {
         vis_log("read", names[i]);
         goto done;
      }
      if (memcmp(got, want[i], sizeof(got)) != 0) {
         vis_log("hash", names[i]);
         goto done;
      }
      if (!vis_module_path_ok(mods[i])) {
         vis_log("module", names[i]);
         goto done;
      }
   }
#elif defined(_WIN32)
   static const WCHAR *const names[2] = {L"nvngx.dll", L"nvapi64.dll"};
   WCHAR dir[MAX_PATH];
   UINT dl = GetSystemDirectoryW(dir, (UINT)(sizeof(dir) / sizeof(dir[0])));
   if (!dl || dl >= MAX_PATH) {
      vis_log("systemdir", "GetSystemDirectoryW");
      goto done;
   }
   for (unsigned i = 0; i < 2; i++) {
      WCHAR path[MAX_PATH];
      _snwprintf(path, sizeof(path) / sizeof(path[0]), L"%s\\%s",
                 dir, names[i]);
      uint8_t got[32];
      if (!vis_hash_file(path, got)) {
         vis_log("read", i == 0 ? "nvngx.dll" : "nvapi64.dll");
         goto done;
      }
      if (memcmp(got, want[i], sizeof(got)) != 0) {
         vis_log("hash", i == 0 ? "nvngx.dll" : "nvapi64.dll");
         goto done;
      }
      if (!vis_module_path_ok(names[i], dir)) {
         vis_log("module", i == 0 ? "nvngx.dll" : "nvapi64.dll");
         goto done;
      }
   }
#else
   vis_log("platform", "not-windows");
   goto done;
#endif

#if defined(_WIN32)
   vis_registry_overrides(true);
   SetEnvironmentVariableA(VIS_ACTIVE_VAR, "1");
#else
   setenv(VIS_ACTIVE_VAR, "1", 1);
#endif
   result = 1;
done:
#endif /* VIS_ARCH_X64 */
   g_vis_result = result;
}

#if defined(_WIN32)
static INIT_ONCE g_vis_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK
vis_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once; (void)param; (void)ctx;
   vis_verify();
   return TRUE;
}
#else
static pthread_once_t g_vis_once = PTHREAD_ONCE_INIT;
#endif

bool
npt_ngx_visible_for_process(void)
{
#if defined(_WIN32)
   InitOnceExecuteOnce(&g_vis_once, vis_once_cb, NULL, NULL);
#else
   pthread_once(&g_vis_once, vis_verify);
#endif
   return g_vis_result != 0;
}

#if defined(NPT_NGX_TEST_HOOKS)
void
npt_ngx_vis_test_reset(void)
{
   g_vis_result = 0;
#if defined(_WIN32)
   memset(&g_vis_once, 0, sizeof(g_vis_once));
#else
   pthread_once_t fresh = PTHREAD_ONCE_INIT;
   g_vis_once = fresh;
#endif
}

void
npt_ngx_vis_test_setup(const char *system32_dir,
                       const char *nvngx_module_path,
                       const char *nvapi_module_path,
                       npt_ngx_vis_hash_fn hash_fn)
{
   t_dir = system32_dir;
   t_mod_core = nvngx_module_path;
   t_mod_nvapi = nvapi_module_path;
   t_hash = hash_fn;
}
#endif
