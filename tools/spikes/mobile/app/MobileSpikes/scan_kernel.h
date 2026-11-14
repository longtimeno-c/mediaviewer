// M1 spike S3 (throwaway). The int8 dot-product scan, in three kernels, so the phone can say
// what the desktop's autovectorised loop costs against hand-written NEON.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
  MV_KERNEL_DESKTOP_COPY = 0,  // src/addons/ai/vectors.cpp dot_i8, verbatim
  MV_KERNEL_NEON = 1,          // vmull_s8 + vpadalq_s16, any arm64
  MV_KERNEL_DOTPROD = 2,       // vdotq_s32, only where mv_has_dotprod()
} mv_kernel;

typedef struct {
  int32_t score;
  uint32_t row;
} mv_hit;

// 1 when this CPU reports FEAT_DotProd.
int mv_has_dotprod(void);

// Deterministic pseudo-random bytes in [-127, 127], so the pages do not compress and every run
// scans the same rows.
void mv_fill_rows(int8_t* out, size_t bytes, uint64_t seed);

// The kernel on its own, for a sanity check that all three agree.
int32_t mv_dot(mv_kernel kernel, const int8_t* a, const int8_t* b, size_t n);

// Scans rows[first .. first + count) of `dim` bytes each. Keeps the best `cap` in `top`, unsorted;
// returns how many it kept.
size_t mv_scan(const int8_t* rows, size_t first, size_t count, size_t dim, const int8_t* q,
               mv_kernel kernel, mv_hit* top, size_t cap);
