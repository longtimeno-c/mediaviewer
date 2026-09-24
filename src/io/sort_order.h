// SPDX-License-Identifier: GPL-2.0-or-later
// Folder sort orders (plan/16 "Status, sort, filter, typeahead"): name, mtime,
// size, type — and, from PR 9, EXIF date taken, which PR 4 could not offer
// without parsing every file.
//
// Pure: no I/O, no platform header. `date_taken` never reads a file here; it
// consults a lookup the host fills on a worker (meta_store), and an entry with
// no date yet sorts by its mtime so the order is total and stable while the
// keys arrive.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "io/dir.h"

namespace mv::io {

enum class sort_key : std::uint8_t { name = 0, modified, size, type, date_taken, count };

struct sort_order {
  sort_key key = sort_key::name;
  bool descending = false;
};

[[nodiscard]] const char* sort_key_label(sort_key k) noexcept;

// The stamp the date-taken sort orders an entry by, if the host has one.
using date_lookup_fn = std::function<std::optional<std::int64_t>(const io::dir_entry&)>;

// Stable, total order. Ties fall through to case-insensitive name, then path.
void sort_entries(std::vector<io::dir_entry>& entries, sort_order order,
                  const date_lookup_fn& dates = {});

// Settings persistence: one small int (key in the low 3 bits, descending in bit 3).
[[nodiscard]] std::int32_t pack_sort(sort_order o) noexcept;
[[nodiscard]] sort_order unpack_sort(std::int32_t v) noexcept;

}  // namespace mv::io
