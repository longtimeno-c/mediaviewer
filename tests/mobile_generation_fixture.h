// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A small generation the publisher really builds (catalog::build, deliver), for the src/mobile
// tests on macOS and MobileCore's tests in the Simulator. Two variants share asset ids but differ
// in every row a person sees, so a test can tell which generation answered.
#pragma once

#include <stdint.h>  /* plain C: MobileCore's Swift tests import this header */

#ifdef __cplusplus
extern "C" {
#endif
/* Builds a generation under `root` (made, emptied first) and writes its folder to out_gen and the
 * library folder its shared paths are relative to to out_library. variant 0 or 1. 0 on success. */
int mv_mobile_test_make_generation(const char* root, int variant, uint64_t epoch, uint64_t generation,
                                   const char* spec, const char* extra_rel, char* out_gen, uint32_t gen_capacity,
                                   char* out_library, uint32_t library_capacity);
#ifdef __cplusplus
}
#endif
