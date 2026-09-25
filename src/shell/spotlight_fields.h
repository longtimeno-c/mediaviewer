// SPDX-License-Identifier: GPL-2.0-or-later
// PR 15 (plan/12 2026-09-25): what the Spotlight importer tells Spotlight
// about a clip, decided here from the shared read model (meta::read) so the
// rule is tested without mdworker. The importer (spotlight_importer_mac.mm)
// only turns these into CF values.
//
// Keys are the kMDItem* constants' own names, which is what those constants
// hold. Only what the file says: nothing about the machine, nothing guessed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meta/meta.h"

namespace mv::shell {

struct spotlight_field {
  enum class kind : std::uint8_t { number, text, texts, date };
  std::string key;
  kind type = kind::number;
  double number = 0;
  std::string text;
  std::vector<std::string> texts;
  std::int64_t unix_seconds = 0;  // kind::date
};

[[nodiscard]] std::vector<spotlight_field> spotlight_fields(const meta::metadata& m);

}  // namespace mv::shell
