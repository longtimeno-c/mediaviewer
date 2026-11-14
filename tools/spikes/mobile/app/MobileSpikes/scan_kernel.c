// M1 spike S3 (throwaway). See scan_kernel.h.
#include "scan_kernel.h"

#include <arm_neon.h>
#include <sys/sysctl.h>

int mv_has_dotprod(void) {
  int value = 0;
  size_t size = sizeof(value);
  if (sysctlbyname("hw.optional.arm.FEAT_DotProd", &value, &size, NULL, 0) != 0) return 0;
  return value != 0;
}

void mv_fill_rows(int8_t* out, size_t bytes, uint64_t seed) {
  uint64_t state = seed;  // SplitMix64
  size_t i = 0;
  while (i < bytes) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    for (int b = 0; b < 8 && i < bytes; ++b, ++i, z >>= 8) {
      const int8_t v = (int8_t)(z & 0xFF);
      out[i] = v == -128 ? -127 : v;
    }
  }
}

// Verbatim from src/addons/ai/vectors.cpp at 1facf73a, so this is the code the desktop runs.
static int32_t dot_desktop(const int8_t* a, const int8_t* b, size_t n) {
  int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += (int32_t)a[i] * b[i];
    s1 += (int32_t)a[i + 1] * b[i + 1];
    s2 += (int32_t)a[i + 2] * b[i + 2];
    s3 += (int32_t)a[i + 3] * b[i + 3];
  }
  for (; i < n; ++i) s0 += (int32_t)a[i] * b[i];
  return s0 + s1 + s2 + s3;
}

static int32_t dot_neon(const int8_t* a, const int8_t* b, size_t n) {
  int32x4_t acc = vdupq_n_s32(0);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const int8x16_t x = vld1q_s8(a + i);
    const int8x16_t y = vld1q_s8(b + i);
    acc = vpadalq_s16(acc, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
    acc = vpadalq_s16(acc, vmull_high_s8(x, y));
  }
  int32_t s = vaddvq_s32(acc);
  for (; i < n; ++i) s += (int32_t)a[i] * b[i];
  return s;
}

__attribute__((target("dotprod")))
static int32_t dot_dotprod(const int8_t* a, const int8_t* b, size_t n) {
  int32x4_t acc = vdupq_n_s32(0);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    acc = vdotq_s32(acc, vld1q_s8(a + i), vld1q_s8(b + i));
  }
  int32_t s = vaddvq_s32(acc);
  for (; i < n; ++i) s += (int32_t)a[i] * b[i];
  return s;
}

int32_t mv_dot(mv_kernel kernel, const int8_t* a, const int8_t* b, size_t n) {
  switch (kernel) {
    case MV_KERNEL_NEON: return dot_neon(a, b, n);
    case MV_KERNEL_DOTPROD: return dot_dotprod(a, b, n);
    default: return dot_desktop(a, b, n);
  }
}

// A small unsorted buffer with a cached minimum: a phone scan is almost all rejects.
static inline size_t keep(mv_hit* top, size_t kept, size_t cap, int32_t* floor_score,
                          int32_t score, uint32_t row) {
  if (kept < cap) {
    top[kept].score = score;
    top[kept].row = row;
    ++kept;
    if (kept == cap) {
      int32_t m = top[0].score;
      for (size_t i = 1; i < cap; ++i) m = top[i].score < m ? top[i].score : m;
      *floor_score = m;
    }
    return kept;
  }
  size_t at = 0;
  for (size_t i = 1; i < cap; ++i) at = top[i].score < top[at].score ? i : at;
  top[at].score = score;
  top[at].row = row;
  int32_t m = top[0].score;
  for (size_t i = 1; i < cap; ++i) m = top[i].score < m ? top[i].score : m;
  *floor_score = m;
  return kept;
}

#define MV_SCAN_LOOP(DOT)                                                \
  for (size_t r = first; r < end; ++r) {                                 \
    const int32_t score = DOT(rows + r * dim, q, dim);                   \
    if (kept < cap || score > floor_score) {                             \
      kept = keep(top, kept, cap, &floor_score, score, (uint32_t)r);     \
    }                                                                    \
  }

// One loop per kernel, so the call is direct and the choice is not paid per row.
__attribute__((target("dotprod")))
static size_t scan_dotprod(const int8_t* rows, size_t first, size_t end, size_t dim,
                           const int8_t* q, mv_hit* top, size_t cap) {
  size_t kept = 0;
  int32_t floor_score = INT32_MIN;
  MV_SCAN_LOOP(dot_dotprod)
  return kept;
}

size_t mv_scan(const int8_t* rows, size_t first, size_t count, size_t dim, const int8_t* q,
               mv_kernel kernel, mv_hit* top, size_t cap) {
  if (cap == 0) return 0;
  const size_t end = first + count;
  size_t kept = 0;
  int32_t floor_score = INT32_MIN;
  switch (kernel) {
    case MV_KERNEL_NEON:
      MV_SCAN_LOOP(dot_neon)
      return kept;
    case MV_KERNEL_DOTPROD:
      return scan_dotprod(rows, first, end, dim, q, top, cap);
    default:
      MV_SCAN_LOOP(dot_desktop)
      return kept;
  }
}
