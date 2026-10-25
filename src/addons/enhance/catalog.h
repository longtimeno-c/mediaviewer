// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Enhance catalogue (#297, docs/plans/enhance.md "How a model is chosen"):
// per kind, the models offered from the largest to the smallest, and per
// model what it needs and the files it runs. Data, not code: a new model is a
// catalogue entry (tools/package/enhance-models.json), not a C++ change.
//
// What the signed version folder carries (tools/package/enhance-models.py stage):
//   models/enhance.json       {"schema":1,"kinds":{"still":{name, unit,
//                             cpu_budget_ms, models: [ids]}, "video", "audio"}}
//   models/<id>/model.json    one model block, as the catalogue wrote it
//   manifest.json             its `remote` list: url, path, size, sha256,
//                             licence; a model's files are the entries under
//                             "models/<id>/"
//
// Read leniently: the host verified the manifest's signature, but a model
// that breaks a rule of enhance-models.py check, or has no file to fetch, is
// left out rather than failing the page, and a kind drops an id that names
// no such model or one that does not serve it. Portable, no OS calls (D9).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "core/result.h"

namespace mv::enhance {

enum class kind : std::uint8_t { still = 0, video = 1, audio = 2 };
inline constexpr std::array<kind, 3> kKinds{kind::still, kind::video, kind::audio};

// "still", "video", "audio".
[[nodiscard]] const char* kind_id(kind k) noexcept;
[[nodiscard]] std::optional<kind> kind_from(std::string_view id) noexcept;
// A model folder's id: [a-z0-9-]{1,32} (it names a folder and node ids),
// in every version's catalogue.
[[nodiscard]] bool model_id_ok(std::string_view id) noexcept;

// Enhance's cap, per weights file (docs/design/12, 2026-10-13): a pin over it
// is refused and never requested. The packer and the catalogue check refuse
// the same figure.
inline constexpr std::uint64_t kMaxWeightsFile = 5'000'000'000;

// One upstream file of the signed manifest's `remote` list.
struct pinned {
  std::string url;
  std::string path;    // "models/<id>/<file>", '/'-separated
  std::string sha256;  // lowercase hex
  std::uint64_t size = 0;
  std::string licence;
};

struct option {
  std::string id;
  std::string text;
};

// What the sheet shows for a model (E2), in the screen vocabulary's shapes.
struct knob {
  std::string id;
  std::string label;
  std::string type;  // "slider" | "choice" | "toggle"
  double min = 0;
  double max = 0;
  double step = 0;
  json::value default_value;    // a number (slider), an option id (choice), a boolean (toggle)
  std::vector<option> options;  // choice
  std::vector<kind> kinds;      // the kinds it applies to: some of the model's
};

struct model {
  std::string id;     // [a-z0-9-]{1,32}, its folder's name
  std::string name;        // "Standard"
  std::string model_name;  // its "model": what it is, "Real-ESRGAN x4plus"
  std::string licence;
  std::vector<kind> kinds;
  std::uint64_t run_bytes = 0;    // peak memory one unit of work needs
  std::uint64_t cache_bytes = 0;  // what the provider compiles beside it (Core ML)
  bool cuda = false;              // passed its self-test on these providers
  bool coreml = false;
  // Time for one of the kind's units on the reference CPU; none: needs a GPU.
  // 64 bits: the catalogue check allows any whole number under 2^53.
  std::optional<std::uint64_t> cpu_unit_ms;
  std::vector<knob> knobs;
  std::vector<pinned> files;  // from the manifest's `remote`, under models/<id>/

  [[nodiscard]] std::uint64_t download_bytes() const noexcept;
  [[nodiscard]] bool serves(kind k) const noexcept;
};

struct kind_entry {
  kind k = kind::still;
  std::string name;  // "Photos", "Video", "Audio"
  std::string unit;  // "a 12 MP photo": what cpu_unit_ms times
  std::uint64_t cpu_budget_ms = 0;
  std::vector<std::string> models;  // ids, largest first; each one in catalog::models
};

struct catalog {
  // Always all three kinds, in kKinds order (an empty catalogue too).
  std::array<kind_entry, 3> kinds{{{kind::still, "Photos", "", 0, {}},
                                   {kind::video, "Video", "", 0, {}},
                                   {kind::audio, "Audio", "", 0, {}}}};
  std::vector<model> models;

  [[nodiscard]] const model* find(std::string_view id) const noexcept;
  [[nodiscard]] const kind_entry& of(kind k) const noexcept { return kinds[static_cast<std::size_t>(k)]; }
};

// The manifest's `remote` entries; malformed ones (a URL the host's rule
// refuses, an unsafe path, a SHA-256 that is not 64 lowercase hex, no size
// or licence) are skipped.
[[nodiscard]] std::vector<pinned> read_remote(std::string_view manifest_json);

// models/<id>/model.json's text for an id; nullopt when it is missing.
using model_source = std::function<std::optional<std::string>(std::string_view id)>;

// status::corrupt when the index is not JSON, not an object, or not schema 1.
[[nodiscard]] result<catalog> parse_catalog(std::string_view index_json, const model_source& model_json,
                                            std::span<const pinned> remote);

// From a verified version folder (UTF-8). A version with no
// models/enhance.json has an empty catalogue: all three kinds, no models.
[[nodiscard]] result<catalog> read_catalog(const std::string& version_dir_utf8);

}  // namespace mv::enhance
