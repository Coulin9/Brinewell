/* SPDX-License-Identifier: MIT */

/* llvm-mingw advertises x86-64 for the Windows ABI of Arm64EC.
 * Let Mesa select the native Arm instruction set while retaining those
 * ABI macros for Windows SDK headers and all other source files. */
#if defined(__arm64ec__)
#include <limits.h>
#pragma push_macro("_M_ARM64EC")
#define _M_ARM64EC 1
#pragma push_macro("__x86_64__")
#pragma push_macro("__x86_64")
#undef __x86_64__
#undef __x86_64
#include "util/detect_arch.h"
#pragma pop_macro("__x86_64")
#pragma pop_macro("__x86_64__")
#include "util/blake3/blake3_impl.h"
#pragma pop_macro("_M_ARM64EC")
#endif
