// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "Index these folders", from another process (docs/design/29 "analyse", owner
// 2026-10-18: the agent may ask the app to index the timeline's media and the
// folders the editor gave).
//
// `MediaViewer --mcp` cannot index: two indexers writing one index fight over
// it (docs/design/18 "One host process"). So it asks the window that hosts the
// add-ons, which hands each folder to the AI pack's index_folder, as the
// person's own "Index this folder" does. The request travels as JSON (a
// distributed notification on the Mac, the single-instance pipe on Windows)
// and is checked here on arrival: absolute paths only, never a `photos:` key,
// never a whole disk, at most kMaxFolders.
//
// The host publishes a sink while the pack is attached as the writer; a request
// that arrives before is kept (the newest kMaxPending) and handed over when it
// attaches. Portable (D9).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::addon {

inline constexpr std::size_t kMaxIndexFolders = 64;
inline constexpr std::size_t kMaxPendingIndexRequests = 8;

struct index_folder_request {
  std::string path;
  bool recursive = true;
};

struct index_request {
  std::string id;  // the requester's, echoed in an acknowledgement
  std::vector<index_folder_request> folders;
};

[[nodiscard]] std::string to_json(const index_request& r);
// nullopt when it is not a request, or any folder breaks the rules above.
[[nodiscard]] std::optional<index_request> index_request_from_json(std::string_view text);

// What became of a request.
enum class index_delivery : std::uint8_t {
  started,  // the pack has the folders
  queued,   // kept until the pack attaches
  refused,  // not a valid request
};
[[nodiscard]] const char* delivery_name(index_delivery d) noexcept;

using index_sink = std::function<expected(const index_folder_request& folder)>;

// The pack attached (as the writer): requests go to `sink`, the kept ones first.
void publish_index_requests(index_sink sink);
// It is going: requests are kept from now on.
void retract_index_requests() noexcept;
// The Mac's distributed notification name for a requester and the window
// (`bundle_id` + ".agent.index"). A developer build's (one that trusts a dev
// add-on key) is apart: ".agent.index.dev", so a test never reaches the
// installed app's index.
[[nodiscard]] std::string index_notification_name(std::string_view bundle_id);

// A request as it arrived (JSON).
[[nodiscard]] index_delivery deliver_index_request(std::string_view json);

}  // namespace mv::addon
