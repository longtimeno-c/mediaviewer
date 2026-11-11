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

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

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

// ---- asking for the index (docs/plans/places-world-map.md) ----------------------
// Both hosts load the AI pack on demand (docs/design/17 "Starting on demand"),
// so an add-on can ask for the index before anything started it (the Places
// world map opened before Local search this session). The core cannot start
// the pack; the host can. So a call that finds no source asks the host through
// this listener (any thread; it must not block) and waits a bounded time for
// publish_index_locations, or for decline_index_request when the host has no
// pack to start (none installed, or it would not load).
using index_wanted_fn = std::function<void()>;
void set_index_wanted_listener(index_wanted_fn fn);
// The host will not publish a source for the requests made so far: waiters stop.
void decline_index_request() noexcept;
// each_index_location; when no source is published and a listener is set,
// asks for one and waits up to `max` for it (polling `keep_waiting`, which
// false stops with status::cancelled), then streams it. status::not_found when
// declined, timed out, or no listener.
[[nodiscard]] expected each_index_location_or_wait(const index_location_visit& visit,
                                                   const std::function<bool()>& keep_waiting,
                                                   std::chrono::milliseconds max);

// The AI pack's mv.ai.1 table as a source: its each_location row. Empty when
// the table is NULL or predates the row (an older pack).
[[nodiscard]] index_location_source ai_index_locations(const mv_ai_api* api);

// docs/plans/edit-adjust.md slice 6: the pack's eyes_json row, published and
// retracted with the locations source (the same lifetime: retract stops new
// calls, wait_index_locations_idle waits out a call in flight). A host's
// Open closed eyes reads it on a worker.
using eyes_source = std::function<result<std::string>(const std::string& path, const std::string& candidates)>;
void publish_ai_eyes(eyes_source source);
// The pack's answer for `path` and the candidate frames (one path a line).
// status::not_found when no pack is loaded or it predates the row.
[[nodiscard]] result<std::string> ai_eyes_json(const std::string& path, const std::string& candidates);
// The table's eyes_json row as a source; empty for an older pack.
[[nodiscard]] eyes_source ai_eyes(const mv_ai_api* api);

// docs/plans/edit-refresh.md S10: the pack's faces_json row (Restore faces'
// landmarks and its identity guard), published and retracted with the eyes.
// `other` may be null; both are `stride`-byte rows of 8-bit RGB.
using faces_source = std::function<result<std::string>(const std::uint8_t* rgb, std::uint32_t width,
                                                       std::uint32_t height, std::uint32_t stride,
                                                       const std::uint8_t* other)>;
void publish_ai_faces(faces_source source);
// The pack's answer. status::not_found when no pack is loaded or it predates
// the row; status::permission_denied while People is off.
[[nodiscard]] result<std::string> ai_faces_json(const std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                                                std::uint32_t stride, const std::uint8_t* other);
// The table's faces_json row as a source; empty for an older pack.
[[nodiscard]] faces_source ai_faces(const mv_ai_api* api);

// 2026-11-07: the pack's person_faces_json row (a person's faces in People,
// each with its photo), published and retracted with the eyes. The Faces
// tool's fixes ask it for the chosen faces' people's photos and offer them to
// eyes_json as candidates (media::people_candidates): the pack reads those at
// any pose, and the host judges the pose on points read again upright.
using person_faces_source = std::function<result<std::string>(std::uint64_t person)>;
void publish_ai_person_faces(person_faces_source source);
// status::not_found when no pack is loaded or it predates the row.
[[nodiscard]] result<std::string> ai_person_faces_json(std::uint64_t person);
[[nodiscard]] person_faces_source ai_person_faces(const mv_ai_api* api);

}  // namespace mv::addon
