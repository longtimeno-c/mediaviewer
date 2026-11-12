// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library (docs/plans/mobile.md §11.1, owner 2026-11-12): a way to try
// the app, and App Review's way in, with no Mac and no NAS. One fixed code opens
// a real signed catalogue generation, verified and installed by the same sync as
// any library. Only the transport differs: `static_remote` reads a static HTTPS
// folder (a GitHub release) through the host's own HTTPS client, in place of
// the Driftwire link. Nothing here knows which OS it runs on (D9).
//
// The folder's layout, one flat file per (root, path):
//   roots.json                        [{"id":"catalogue","label":"Catalogue"},{"id":"media","label":"<share>"}]
//   <root id>__<path, '/' as "__">    every other file, e.g. catalogue__gen__000001__manifest.json
// Like the service's root list, roots.json is not signed and is trusted only as far
// as what it leads to: sync believes nothing that the pinned key did not sign.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "mobile/link.h"
#include "mobile/link_payload.h"

namespace mv::mobile::sample {

// What a person types on the Scan screen, any case, spaces around it ignored.
inline constexpr std::string_view kCode = "SAMPLE";
// Its QR code and link: the same library.
inline constexpr std::string_view kLink = "mediaviewer://sample";
inline constexpr std::string_view kLibraryID = "5efedabf-6c37-41dc-850f-4fb00e3efd9a";
inline constexpr std::string_view kLibraryName = "Sample library";
// The sample library's own publisher, never the owner's: tools/ios/sample-library.sh
// signs with its secret half, which is not in the repository.
inline constexpr std::string_view kPublisherKey = "2329be451a7c5d60c7ebd45f8d9199e0aa78443885d9e7d66a64ee92c2ca68ea";
// A GitHub release of this repository. Free, no server process, read-only to the phone.
inline constexpr std::string_view kBase = "https://github.com/longtimeno-c/mediaviewer/releases/download/sample-library/";

// Whether `text` is the sample code or its link.
[[nodiscard]] bool is_code(std::string_view text);
// The sample library as a scanned code would describe it: no service, no expiry.
[[nodiscard]] pair_payload payload();

// The file name of (root, rel) in the static folder; nullopt when either holds
// anything but letters, digits, '.', '_', '-', ' ' (as '_') and '/' (as "__"), or
// a "." or ".." component. The publisher's script uses the same rule.
[[nodiscard]] std::optional<std::string> flat_name(std::string_view root, std::string_view rel);

// The host's HTTPS GET of [at, at + length) of `url` (length UINT64_MAX: to the
// end), bytes to `sink` in order. status::not_found for a 404, ::cancelled when
// `cancel` fired or the sink said stop, ::io for anything else. Blocks.
using fetch_fn = std::function<expected(const std::string& url, std::uint64_t at, std::uint64_t length,
                                        const byte_sink& sink, const cancel_flag* cancel)>;

class static_remote final : public remote_file {
 public:
  // `base` ends in '/'.
  static_remote(std::string base, fetch_fn fetch);

  // Reads roots.json afresh: the folder answers. status::io when it does not.
  [[nodiscard]] expected connect(const cancel_flag* cancel = nullptr);
  // "Forget this library": every read in flight stops at its next chunk, and every later one is refused.
  void shut_down() noexcept;
  [[nodiscard]] bool connected() const;
  void disconnect() noexcept;
  // The host the folder is on, for the Connection screen ("github.com").
  [[nodiscard]] std::string host() const;

  [[nodiscard]] result<std::vector<driftwire::serve::root_info>> roots() override;
  [[nodiscard]] result<std::vector<driftwire::serve::list_entry>> list(const std::string& root,
                                                                       const std::string& rel) override;
  [[nodiscard]] result<driftwire::serve::stat_info> stat(const std::string& root, const std::string& rel) override;
  [[nodiscard]] expected read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                    std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel,
                                    bool bulk) override;

 private:
  [[nodiscard]] result<std::vector<driftwire::serve::root_info>> read_roots(const cancel_flag* cancel);
  [[nodiscard]] bool is_shut() const;

  std::string base_;
  fetch_fn fetch_;
  mutable std::mutex m_;
  std::optional<std::vector<driftwire::serve::root_info>> roots_;  // under m_
  bool connected_ = false;                                        // under m_
  bool shut_ = false;                                             // under m_
};

}  // namespace mv::mobile::sample
