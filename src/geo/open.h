// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Opening the place database from a file (docs/design/06 "Place").
//
// The host resolves the path: the Places add-on's installed package
// (`<add-on dir>/data/places.mvgeo`), or a dev/test blob. The core never goes
// looking for it. Worker threads only.
#pragma once

#include <memory>
#include <string_view>

#include "core/result.h"
#include "geo/geo.h"
#include "io/mapped_file.h"

namespace mv::geo {

// A database over a memory-mapped file. Immutable; share it freely between
// workers. The mapping lives as long as this object.
class mapped_database {
 public:
  [[nodiscard]] const database& db() const noexcept { return db_; }

 private:
  friend result<std::unique_ptr<mapped_database>> open_database(std::string_view utf8_path);
  io::mapped_file file_;
  database db_;
};

// Maps and validates the blob. Missing file: status::not_found. Damaged,
// truncated or foreign: status::corrupt. Newer format: unsupported_format.
[[nodiscard]] result<std::unique_ptr<mapped_database>> open_database(std::string_view utf8_path);

// ---- one per process --------------------------------------------------------------------
// The host sets the path at startup (cheap: no I/O). The first shared() call
// maps the file on whichever worker asks; later calls are a load of an atomic
// pointer. A path that fails to open gives nullptr for the life of the process
// (no retry storm, no error surfaced: the pane keeps showing coordinates).
// Setting a new path drops nothing already handed out; it applies to a
// process that has not loaded yet (tests, or the add-on being enabled).
void set_data_path(std::string_view utf8_path);
[[nodiscard]] const database* shared();

}  // namespace mv::geo
