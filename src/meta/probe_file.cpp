// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #345: the probe over a file (meta/probe.h). Every byte read from the file counts
// against kProbeMaxFileBytes, whichever reader asked for it.
#include "io/file_port.h"
#include "meta/probe.h"

namespace mv::meta {
namespace {

class capped_file final : public parse::byte_source {
 public:
  bool open(std::string_view utf8_path) { return f_.open(utf8_path, io::read_mode::sequential).has_value(); }
  [[nodiscard]] std::uint64_t size() const noexcept override { return f_.size(); }
  [[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept override {
    if (into.size() > kProbeMaxFileBytes - spent_) return 0;
    spent_ += into.size();
    auto n = f_.read_at(offset, into);
    return n ? *n : 0;
  }

 private:
  io::file_reader f_;
  std::uint64_t spent_ = 0;
};

}  // namespace

provenance probe_provenance(std::string_view utf8_path) noexcept {
  try {
    capped_file src;
    if (!src.open(utf8_path)) return {};
    return probe_provenance(src);
  } catch (...) {
    return {};
  }
}

void probe_evidence(std::string_view utf8_path, parse::evidence& ev, bool c2pa,
                    std::vector<property>* rows) noexcept {
  try {
    capped_file src;
    if (src.open(utf8_path)) probe_evidence(src, ev, c2pa, rows);
  } catch (...) {
  }
}

}  // namespace mv::meta
