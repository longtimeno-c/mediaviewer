// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal MSVC AddressSanitizer runtime smoke test.
//
// The hosted Windows image currently hangs before main in ASan executables
// linked with vcpkg's prebuilt Catch2 entry point. Keep this target standalone:
// it verifies that an instrumented project executable starts, exercises the
// result and SPSC ring allocation paths, and exits without sanitizer findings.

#include <cstdint>
#include <memory>

#include "core/result.h"
#include "core/spsc_ring.h"

int main() {
  mv::result<std::uint32_t> value{41u};
  if (!value || value.value() != 41u) return 1;

  mv::spsc_ring<std::uint32_t, 8> ring;
  for (std::uint32_t i = 0; i < 7; ++i) {
    if (!ring.try_push(i)) return 2;
  }
  if (ring.try_push(7u)) return 3;

  auto heap = std::make_unique<std::uint32_t[]>(64);
  std::uint64_t sum = 0;
  for (std::uint32_t i = 0; i < 64; ++i) {
    heap[i] = i;
    sum += heap[i];
  }
  if (sum != 2016u) return 4;

  for (std::uint32_t expected = 0; expected < 7; ++expected) {
    std::uint32_t actual = 0;
    if (!ring.try_pop(actual) || actual != expected) return 5;
  }
  std::uint32_t extra = 0;
  if (ring.try_pop(extra) || !ring.empty()) return 6;

  return 0;
}
