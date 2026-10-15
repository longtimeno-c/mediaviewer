// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #398: the Local search index's coordinates, served to other add-ons as the
// host table's locations_of_index (docs/design/17 "Places over the index").
//
// The index is the AI pack's index.db, and the AI pack is an add-on: it links
// nothing of the core, and the core links nothing of it. So the host holds
// one process-wide source here. Each host publishes the pack's mv.ai.1
// each_location row when the pack attaches (ai_index_locations) and retracts
// it before the pack goes; every host table's locations_of_index reads
// through it. Nothing here reads a file: the source serves what the index
// already holds.
//
// Lifetime: a call in flight holds no lock while the source runs. Retracting
// tells it to stop at its next row (the visitor returns false and the call
// returns status::cancelled) and makes later calls status::not_found;
// wait_index_locations_idle then returns once no call is inside the source,
// so the pack's library may go. A host that must not block its UI thread
// retracts there and waits on its add-on queue.
//
// Portable (D9).
#pragma once

#include <functional>

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_ai.h>

#include "core/result.h"

namespace mv::addon {

// One located asset; return false to stop.
using index_location_visit = std::function<bool(const mv_addon_location_entry& entry)>;
// Streams the index's located assets, in batches, from what it holds. A visit
// returning false stops it: status::cancelled.
using index_location_source = std::function<expected(const index_location_visit& visit)>;

// Replaces the process's source (a call in flight on the old one stops).
void publish_index_locations(index_location_source source);
// No source from now on; calls in flight stop at their next row. Never blocks.
void retract_index_locations() noexcept;
// Returns once no call is inside a source. [any thread but one a source's
// visitor runs on]
void wait_index_locations_idle() noexcept;

// Streams the published source. status::not_found when none is published,
// status::cancelled when `visit` stopped it or the source was retracted.
[[nodiscard]] expected each_index_location(const index_location_visit& visit);

// The AI pack's mv.ai.1 table as a source: its each_location row. Empty when
// the table is NULL or predates the row (an older pack).
[[nodiscard]] index_location_source ai_index_locations(const mv_ai_api* api);

}  // namespace mv::addon
