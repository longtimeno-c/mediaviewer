// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Planning an import: every decision is made before the copy starts, so it
// never stops halfway to ask (plan/18 "What it is, honestly").
//
// The duplicate test is size first, then BLAKE3 only when a size matches,
// never by name. The card memory answers most hashes without reading the
// card; the library index answers the destination's without reading it.
// Same name with different bytes gets a safe name; overwrite is not offered.
// I/O threads only (make_plan, assign_names); the JSON and selection helpers
// are pure.
#pragma once

#include <atomic>
#include <set>
#include <string>

#include "addons/import/host.h"
#include "addons/import/library_index.h"
#include "addons/import/model.h"
#include "core/result.h"

namespace mv::import {

// Resolves the destination / backup roots (no trailing separator).
[[nodiscard]] std::string normalize_root(std::string root);

[[nodiscard]] result<plan_result> make_plan(const host& h, library_index& idx, scan_result& scan,
                                            const preset& settings,
                                            const std::set<std::string>& marked,
                                            const std::string& default_library,
                                            const std::atomic<bool>& cancel);

// Names and folders for every unit given the current selection: layout,
// rename template, {seq} per day continuing from import.db, and safe names
// for clashes with the plan itself or with files already at the destination.
// A destination file that exists but is not indexed is hashed once and
// indexed; if it holds the same bytes the unit becomes a duplicate.
void assign_names(const host& h, library_index& idx, scan_result& scan, plan_result& plan,
                  const std::atomic<bool>& cancel);

// Select or clear: one unit (unit >= 0), a day (unit < 0, day set), or all.
// Filtered units never become selected. Returns true if anything changed.
bool apply_selection(plan_result& plan, int unit, const std::string* day, bool selected);

// The plan for the chrome: totals, days, units, and the "Where files go"
// folder counts. `bytes_per_second` (0 = unknown) is the measured rate the
// ETA uses.
[[nodiscard]] std::string plan_to_json(const scan_result& scan, const plan_result& plan,
                                       double bytes_per_second);

// A preview of names for a preset with no card: three sample units.
[[nodiscard]] std::string preview_json(const preset& p, const std::string& default_library);

}  // namespace mv::import
