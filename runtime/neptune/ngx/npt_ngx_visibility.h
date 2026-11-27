/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Process-side validity check for the NGX SR deployment.  The wrapper
 * can only declare a candidate (BRINEWELL_NGX_SR_CANDIDATE plus the two
 * expected payload hashes); whether the game process actually sees the
 * right files is decided here, once per process, by hashing the final
 * System32 nvngx.dll / nvapi64.dll and rejecting foreign same-name
 * modules.  BRINEWELL_NGX_SR_ACTIVE is written by us on success and is
 * never trusted as an input.
 */

#ifndef NPT_NGX_VISIBILITY_H
#define NPT_NGX_VISIBILITY_H

#include <stdbool.h>
#include <stdint.h>

bool npt_ngx_visible_for_process(void);

#if defined(NPT_NGX_TEST_HOOKS)
/* Test injection (test targets only): a fake System32 directory, fake
 * loaded-module paths (NULL = not loaded), and an optional replacement
 * hash function (NULL = real streaming SHA-256 over the file). */
typedef bool (*npt_ngx_vis_hash_fn)(const char *path, uint8_t out[32]);
void npt_ngx_vis_test_reset(void);
void npt_ngx_vis_test_setup(const char *system32_dir,
                            const char *nvngx_module_path,
                            const char *nvapi_module_path,
                            npt_ngx_vis_hash_fn hash_fn);
#endif

#endif /* NPT_NGX_VISIBILITY_H */
