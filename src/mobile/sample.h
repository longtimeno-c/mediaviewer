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
//   files.json, files.sig             the signed list: library id, roots, and the size and SHA-256
//                                     of every file below that sync does not check itself
//   <root id>__<path, '/' as "__">    every other file, e.g. c0c0c0c0c0c0c0c0__gen__000001__manifest.json
// Nothing is used on trust in the host. current.json, gen/ and models/ under the
// catalogue root are what sync verifies against the pinned key. Everything else
// (the roots, the JPEG-512 packs, the originals) must be in files.json, which the
// same key signs; such a file is fetched whole, hashed and kept before one byte of
// it reaches the reader, and a file not listed is not read at all.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
// The sink's offsets count from `at` (0 is the first byte asked for).
using fetch_fn = std::function<expected(const std::string& url, std::uint64_t at, std::uint64_t length,
                                        const byte_sink& sink, const cancel_flag* cancel)>;

// files.sig signs this prefix and then files.json's bytes, so it can never be taken
// for a signature over anything else the key signs.
inline constexpr std::string_view kFilesContext = "MediaViewer sample files 1\n";

// One file the list vouches for.
struct listed_file {
  std::uint64_t size = 0;
  std::string sha256;  // lowercase hex
};

// files.json, checked: the signature by `key` over kFilesContext + text, the library
// id, and every name, size and hash well formed. nullopt otherwise.
struct file_list {
  std::vector<driftwire::serve::root_info> roots;
  std::string catalogue_root;  // the root labelled "Catalogue"
  std::map<std::string, listed_file> files;  // by flat name
};
[[nodiscard]] std::optional<file_list> parse_file_list(std::string_view text, std::string_view signature_hex,
                                                       const key32& key, std::string_view library_id);

class static_remote final : public remote_file {
 public:
  // `base` ends in '/'. `key` and `library_id` are the pinned ones; verified files
  // are kept in `verified_dir`, which Forget removes with the rest of the cache, at
  // most `verified_budget` bytes of them (least recently read go first).
  static_remote(std::string base, fetch_fn fetch, key32 key, std::string library_id, std::string verified_dir,
                std::uint64_t verified_budget = kVerifiedBudget);

  // Larger than the whole sample library (about 55 MB), so in practice nothing is
  // fetched twice; a bound all the same.
  static constexpr std::uint64_t kVerifiedBudget = 256ull << 20;

  // What the verified copies and unfinished downloads hold on disk: part of the
  // cache size the app reports.
  [[nodiscard]] std::uint64_t verified_bytes() const;

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
  [[nodiscard]] result<file_list> read_list(const cancel_flag* cancel);
  [[nodiscard]] result<file_list> list_now(const cancel_flag* cancel);
  [[nodiscard]] result<std::string> small(const std::string& name, std::uint64_t cap, const cancel_flag* cancel);
  // The local, verified copy of a listed file: fetched whole and hashed the first time.
  // The download belongs to the file, not to the reader: a reader that cancels leaves
  // what arrived in a .part, and the next reader goes on from its end (a seek in a
  // clip cancels routinely). A reader waiting for another's download honours its own cancel.
  [[nodiscard]] result<std::string> verified(const std::string& name, const listed_file& f, const cancel_flag* cancel);
  void trim_verified(const std::string& keep);
  [[nodiscard]] bool is_shut() const;

  std::string base_;
  fetch_fn fetch_;
  key32 key_;
  std::string library_id_;
  std::string verified_dir_;
  // One whole-file fetch per name at a time: a second reader of the same file waits for it,
  // and different files do not wait for each other.
  std::uint64_t verified_budget_;
  std::mutex fetching_m_;
  std::map<std::string, std::shared_ptr<std::timed_mutex>> fetching_;  // under fetching_m_
  mutable std::mutex m_;
  std::optional<file_list> list_;  // under m_
  bool connected_ = false;                                        // under m_
  bool shut_ = false;                                             // under m_
};

}  // namespace mv::mobile::sample
