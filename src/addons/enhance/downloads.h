// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297 slice 1): a model's weights live outside the add-on's zip.
//
// The signed manifest pins each weights file under `remote` (a fixed
// https://huggingface.co/<repo>/resolve/<revision>/<file> URL, size, SHA-256,
// SPDX licence) at "<kind>/<file>". Nothing is requested until the person
// clicks Install for that kind on Enhance's settings page; then the shared
// model downloader (#482 T3, src/addons/ai/fetch.h) fetches them into the
// add-on's data folder: a plain GET with no query, cookies or token,
// resumable with Range (also after a restart), each file kept only when its
// SHA-256 matches. A kind's download is its own: a video model never pulls an
// audio one.
//
// Refused before any request: a pin over 5 GB (kMaxWeightsBytes), and a disk
// without room for what is still to come. The models' folder is never
// counted toward a family ceiling, like the AI pack's index.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "addons/ai/fetch.h"
#include "addons/enhance/spec.h"

namespace mv::enhance {

struct pin {
  ai::remote_file file;  // file.path is the manifest's remote path, "<kind>/<file>"
  std::string licence;   // SPDX
};

// The `remote` list of the add-on's own signed manifest.json (the host
// verified its signature before loading the add-on). Entries that do not
// parse are left out: a model naming one is then not available.
[[nodiscard]] std::vector<pin> read_pins(std::string_view manifest_json);

// What one kind's model needs fetched, from its spec and the pins.
struct model_files {
  std::vector<ai::remote_file> files;  // paths "<kind>/<file>", under the models root
  std::vector<std::string> licences;
  std::uint64_t bytes = 0;
  bool missing = false;   // a weights file the spec names has no pin
  bool too_large = false; // a pin is over kMaxWeightsBytes
  [[nodiscard]] bool usable() const noexcept { return !files.empty() && !missing && !too_large; }
};
[[nodiscard]] model_files files_for(const model_spec& spec, const std::vector<pin>& pins);

enum class dl_state : std::uint8_t {
  absent,       // never asked for, or removed
  downloading,
  waiting,      // Hugging Face did not answer: retrying by itself
  stopped,      // asked for, not finished (stopped, or the app quit): Resume
  no_space,     // refused before the GET: not enough free disk
  gone,         // a pinned file is gone (404 / 410): needs an update of Enhance
  failed,       // a file never matched its SHA-256
  refused,      // the pin is over 5 GB, or names a file it does not pin
  ready,
};

struct dl_status {
  dl_state state = dl_state::absent;
  std::uint64_t done_bytes = 0;
  std::uint64_t total_bytes = 0;
  double retry_in_s = 0;
  std::uint64_t need_bytes = 0;  // no_space: free disk wanted, and what there is
  std::uint64_t free_bytes = 0;
};

class downloads {
 public:
  struct port {
    // The platform's transport; null where there is none.
    std::function<std::unique_ptr<ai::http_transport>()> transport;
    // Free bytes on the volume holding `dir`; nullopt when unknown (no gate).
    std::function<std::optional<std::uint64_t>(const std::string& dir)> free_disk;
    // A kind's status moved. Called on the download's own thread (or the
    // caller's), never while this object's lock is held.
    std::function<void(model_kind)> changed;
  };

  // `root` is <data>/models: a kind's files land at <root>/<kind>/<file>.
  downloads(std::string root_utf8, port p, ai::fetch_options options = {});
  ~downloads();
  downloads(const downloads&) = delete;
  downloads& operator=(const downloads&) = delete;

  // Here and verified (the .ok records agree); costs a stat per file.
  [[nodiscard]] bool ready(const model_files& m) const;
  // The status, with `ready` folded in for a kind that is not running.
  [[nodiscard]] dl_status status(model_kind k, const model_files& m) const;
  // The person asked for this kind (Install / Resume): fetch what is missing
  // on a thread of the kind's own. A no-op while it runs.
  void start(model_kind k, const model_files& m);
  // Stops the kind's download; what arrived is kept for Resume.
  void stop(model_kind k);
  // Stops it and deletes its files (Remove).
  void remove(model_kind k, const model_files& m);
  // Every download stops (the add-on is shutting down). Joins.
  void stop_all();

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

 private:
  struct run {
    std::thread t;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    dl_status last;  // under m_
  };
  void join(model_kind k);
  void finish(model_kind k, dl_status s);

  std::string root_;
  port port_;
  ai::fetch_options opt_;
  mutable std::mutex m_;
  std::array<std::unique_ptr<run>, 3> runs_;
};

}  // namespace mv::enhance
