// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A read-only search engine over one installed generation (docs/plans/mobile.md §5, §9): the
// desktop's query path, linked statically, behind a small host table that answers from the
// catalogue. A new generation is a new engine.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "mobile/catalogue.h"

namespace mv::mobile {

struct hit {
  std::int64_t asset = 0;
  std::int64_t pts_ms = -1;
  float score = 0;
  std::uint32_t kind = 0, match = 0, more_in_clip = 0;
  std::string path, snippet;
};

// True for the towers a phone can run (§9: L/14 and B/32). H/14 is not one.
[[nodiscard]] bool tower_supported(std::string_view spec);

// docs/plans/mobile.md §5's folder for the engine: index.db, settings.json and what else the
// generation carries, linked in, and the text tower where pack.cpp expects it. Atomic: a run
// killed part-way leaves nothing that a later run would believe.
[[nodiscard]] expected lay_out_work_dir(const catalogue& c, const std::string& work_dir);

class search_engine {
 public:
  ~search_engine();
  search_engine(const search_engine&) = delete;
  search_engine& operator=(const search_engine&) = delete;

  // status::unsupported_format for a tower a phone cannot run. Reads the
  // catalogue now; the model loads on the engine's own thread.
  [[nodiscard]] static result<std::unique_ptr<search_engine>> start(std::shared_ptr<const catalogue> c,
                                                                    const std::string& work_dir);
  enum class state : std::uint8_t { warming, ready, failed };
  [[nodiscard]] state current() const;
  // status::cancelled when a newer run or cancel() overtakes it. timeout_ms 0: no limit.
  [[nodiscard]] result<std::vector<hit>> run(const std::string& query, std::uint32_t kinds, std::uint32_t timeout_ms);
  void cancel() noexcept;

 private:
  search_engine() = default;
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::mobile
