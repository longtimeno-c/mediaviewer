// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Catalogue sync (docs/plans/mobile.md §5 rule 7, §8.3, M4): read current.json
// over the link, believe it only if the pinned key signed it, refuse anything
// older than what this phone has, copy with resume, check every file against
// the signed manifest, then swap with one rename. Interrupted at any point the
// previous catalogue stays usable and the next run resumes. Blocks; workers only.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "mobile/link.h"

namespace mv::mobile {

struct sync_config {
  std::string library_dir;    // Application Support/library: this module's alone
  std::string library_id;     // from the pairing
  std::array<std::uint8_t, 32> publisher_key{};  // pinned at pairing
  std::string catalogue_root; // service root of `.mediaviewer`; empty: find it
  // A library.json with no catalogue is file mode (no_catalogue). Off for the sample library,
  // which always has one: anything else there is refused.
  bool file_mode = true;
};

// On disk at <library_dir>/installed.json; the one record of what is here.
struct installed_state {
  std::string library_id;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::string dir;            // <library_dir>/gen/<dir>
  std::string catalogue_root; // remembered, so a connection need not look
  // The highest this phone has ever accepted. Never goes down: the refusal in
  // rule 7 is against this, not against the installed generation.
  std::uint64_t high_epoch = 0;
  std::uint64_t high_generation = 0;
};
[[nodiscard]] std::optional<installed_state> read_installed(const std::string& library_dir);

enum class sync_outcome : std::uint8_t {
  up_to_date,     // nothing to do
  older_refused,  // the NAS has an older (epoch, generation): normal, and kept out (rule 8)
  copy_available, // check(): a newer generation, `total_bytes` to bring
  installed,      // copy(): it is on the phone and in use
  rejected,       // a bad signature, a wrong library, a bad hash: nothing was installed
  no_space,
  unreachable,    // the NAS could not be read
  cancelled,
  waiting,        // copy(): `allowed` said no (mobile data without the person's say-so); resumes later
  // docs/plans/phone-in-the-app.md §4: a named folder holds a library.json naming this library, but no
  // folder holds a current.json the pinned key signed for it (none yet, or none this phone can
  // verify). The phone opens the library in file mode; a later check finds the catalogue.
  no_catalogue,
};

struct sync_file {
  std::string remote_rel;  // under the catalogue root
  std::string local_rel;   // under the staging folder, or under models/
  std::uint64_t size = 0;
  std::string sha256;
  bool model = false;      // content-addressed, shared across generations
};

struct sync_plan {
  sync_outcome outcome = sync_outcome::unreachable;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t done_bytes = 0;     // what a resume already holds
  std::uint64_t needed_bytes = 0;   // when outcome is no_space
  std::string catalogue_root;
  std::string library_root;         // no_catalogue: the service root that holds library.json
  std::string manifest_text;
  std::vector<sync_file> files;
};

using sync_progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
// Asked before each file and with each chunk: may bytes move now? (§8.4: not
// over mobile data unless the person said so.) Empty: always.
using sync_allowed = std::function<bool()>;

// Reads only current.json and, when it is newer, the manifest: safe over any
// connection. Never touches the installed catalogue.
[[nodiscard]] sync_plan check_catalogue(remote_file& remote, const sync_config& c);

// Brings a checked plan down and swaps it in. outcome is installed, or the
// reason it is not; the previous catalogue is untouched unless installed.
[[nodiscard]] sync_outcome copy_catalogue(remote_file& remote, const sync_config& c, sync_plan& plan,
                                          const sync_progress& progress, const cancel_flag* cancel,
                                          const sync_allowed& allowed = {});

// Everything this phone holds of the library: "Forget this library".
void remove_library(const std::string& library_dir);

}  // namespace mv::mobile
