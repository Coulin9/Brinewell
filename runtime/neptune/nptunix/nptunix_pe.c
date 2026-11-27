/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * PE carrier stub for nptunix.dll; the unixlib code lives in
 * nptunix.so (npt_unix*.c).  Wine's loader recognizes this as a
 * builtin via the post-link "Wine builtin DLL" DOS-stub marker.
 */

#include <windows.h>

/* The post-link Wine marker patch also rewrites the export-directory module
 * name. LLVM lld omits that directory for a DLL without exports. */
__declspec(dllexport) const int nptunix_anchor = 0;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved);

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
   (void)inst;
   (void)reason;
   (void)reserved;
   return TRUE;
}
