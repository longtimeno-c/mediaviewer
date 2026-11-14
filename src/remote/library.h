// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Another desktop opens the host's drives (docs/plans/desktop-client.md, issue
// #686): the paired servers (a self-hosted computer's `driftwire host` or a
// Synology's `driftwire serve`), one phone-style link to each
// (mobile::link_session: the route race, the relay, the direct-path upgrade,
// reconnect, range reads), their folders listed as `remote:` keys
// (remote/key.h), tiles made here from range reads, and originals fetched into
// a disk cache by content identity (remote/cache.h).
//
// Read-only (owner call 1, 2026-11-14): nothing here calls the service's
// write op. Every call that can block says so and runs on a worker, never the
// UI or render thread (rule 1). Nothing here logs a path, a name or an
// address (rule 6). Portable (D9): no OS header, no host type.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "mobile/link.h"
#include "remote/cache.h"
#include "remote/key.h"
#include "wire/secrets.h"

namespace mv::remote {

// How a server is doing, as its row says it.
enum class reach : std::uint8_t {
  unknown,        // not tried since launch
  online,
  unreachable,    // nothing answered (offline, asleep, another network without a route)
  access_ended,   // revoked, or a limited pairing whose time is up
  wrong_service,  // answered with a key that is not the one paired
  too_old,        // the service is older than this app needs
};
[[nodiscard]] const char* reach_name(reach r) noexcept;

// What the host says when a server's folder or a pairing did not work
// (docs/plans/desktop-client.md §3 "Unreachable, offline, ended"), the same
// words on both hosts. `server` is the server's name.
[[nodiscard]] std::string open_problem_words(status st, reach r, std::string_view server);
[[nodiscard]] std::string pair_problem_words(status st, std::string_view server);
[[nodiscard]] std::string stale_words(std::string_view server);

struct server_info {
  std::string id;    // 16 hex: from the service's identity
  std::string name;  // the library's name in the code, else "Server"
  reach state = reach::unknown;
  std::int64_t paired_unix = 0;
};

struct folder {
  std::string key;
  std::string name;
  bool offline = false;  // a named folder whose drive is not there right now
};

struct file {
  std::string key;
  std::string name;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
};

struct listing {
  std::string title;  // the folder's own name (a named folder's label, the server's name)
  // The server did not answer: this is the folder as it was last listed here,
  // so what was fetched still opens; the host says the server is unreachable.
  bool stale = false;
  std::vector<folder> folders;
  std::vector<file> files;  // shown kinds only (the D5 listed set), hidden names left out
};

// A seekable read of one remote file: what a clip's poster reads through
// (player::byte_source adapts it in the host). One caller at a time.
class range_reader {
 public:
  [[nodiscard]] virtual result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  virtual ~range_reader() = default;
};

struct config {
  std::string data_dir;   // servers.json (no key in it)
  std::string cache_dir;  // originals and tiles
  std::uint64_t cache_cap = 2ull << 30;
  std::string device_name = "Computer";  // what the service lists this desktop as
  // Keys live here; null: the OS store (Keychain, DPAPI) under kAppService.
  driftwire::secrets::store* store = nullptr;
  bool loopback_only = false;  // the tests
  int connect_timeout_ms = 5000;
  // After a server failed to answer, calls fail at once for this long unless
  // they are the person's own (open, expand): a gallery of tiles must not
  // wait out a timeout each.
  int retry_after_ms = 10000;
  // The tile makers: the image pipeline's JPEG-512 from a still's bytes
  // (whole, or its head), and a clip's poster through a range reader. The
  // host wires image::make_thumb_jpeg and player::poster_frame; null leaves
  // that kind without a tile.
  std::function<result<std::vector<std::uint8_t>>(std::span<const std::uint8_t>, const job_context*)> still_tile;
  std::function<result<std::vector<std::uint8_t>>(range_reader&, const job_context*)> clip_tile;
};

// What a code says before it is redeemed: shown so the person can compare
// the fingerprint with the host's card.
struct code_preview {
  std::string name;
  std::string fingerprint;
  std::int64_t expires = 0;
  bool already_paired = false;
};

using progress_fn = std::function<void(std::uint64_t done, std::uint64_t total)>;

class library {
 public:
  explicit library(config c);
  ~library();
  library(const library&) = delete;
  library& operator=(const library&) = delete;

  // The process's one library (the hosts). Configured once at launch; null
  // until then. Any thread.
  static void install(std::unique_ptr<library> lib);
  [[nodiscard]] static library* shared() noexcept;

  // Reads servers.json and the keys. BLOCKS (a file and the OS store): a worker.
  [[nodiscard]] expected load();

  // ---- pairing (docs/plans/desktop-client.md §5) -------------------------------
  // The code or the mediaviewer://pair#… link, pasted. No I/O.
  [[nodiscard]] result<code_preview> preview(std::string_view code_text, std::int64_t now_unix) const;
  // BLOCKS: the route race and the code redeemed, then saved. The new
  // server's id. status::invalid_arg: not a code; ::timeout: expired;
  // ::permission_denied: a wrong, spent or expired code, or the wrong
  // service; ::io: nothing answered; ::unsupported_format: too old.
  [[nodiscard]] result<std::string> pair(std::string_view code_text, std::int64_t now_unix,
                                         const mobile::cancel_flag* cancel = nullptr);
  // BLOCKS (the OS store, servers.json, the cache): forgets a server here.
  // The service still lists this desktop until the owner removes it there.
  [[nodiscard]] expected forget(std::string_view server_id);

  // ---- what the rows show; never blocks --------------------------------------
  [[nodiscard]] std::vector<server_info> servers() const;
  [[nodiscard]] std::optional<server_info> server(std::string_view id) const;
  // The name a folder key is shown under: a named folder's label, a
  // subfolder's leaf, the server's name. "" when unknown.
  [[nodiscard]] std::string display_name(std::string_view key) const;
  // Bumped whenever a server is added, forgotten, or changes state.
  [[nodiscard]] std::uint64_t generation() const noexcept { return gen_.load(std::memory_order_acquire); }

  // ---- reads; every one BLOCKS ------------------------------------------------
  // `person`: the person asked (opened, expanded), so a server in its retry
  // pause is tried again now.
  [[nodiscard]] result<listing> list(std::string_view folder_key, bool person = true,
                                     const mobile::cancel_flag* cancel = nullptr);
  // A JPEG-512 for a file key (docs/plans/desktop-client.md §3 "First pixel"):
  // from the tile cache, else a still's bytes (whole; a RAW over
  // kWholeTileBytes its head first, for its embedded preview), else a clip's
  // poster through range reads.
  // `cancel` (the viewer moved on) stops it as `ctx`'s cancel does, network waits included.
  [[nodiscard]] result<std::vector<std::uint8_t>> tile(std::string_view file_key, const job_context* ctx = nullptr,
                                                       const mobile::cancel_flag* cancel = nullptr);
  // The tile's file when one was made (no network). For the viewer's first picture.
  [[nodiscard]] std::optional<std::string> cached_tile(std::string_view file_key);
  // The original, fetched whole into the cache if it is not there: its local
  // path. Two callers for one key share one fetch. `progress` gets bytes so far.
  [[nodiscard]] result<std::string> fetch(std::string_view file_key, const mobile::cancel_flag* cancel = nullptr,
                                          const progress_fn& progress = {});
  // The original's local path when it is already here whole (no network).
  [[nodiscard]] std::optional<std::string> cached(std::string_view file_key);
  // Size and mtime of a file key as last listed (no network); nullopt when
  // it was never listed this session.
  [[nodiscard]] std::optional<std::pair<std::uint64_t, std::int64_t>> stamp(std::string_view file_key) const;
  // A range of a file, straight from the service.
  [[nodiscard]] expected read_range(std::string_view file_key, std::uint64_t at, std::uint64_t length,
                                    const mobile::byte_sink& sink, const mobile::cancel_flag* cancel = nullptr);

  // Read-only for now (owner call 1): false always. The role a host's admin
  // grants (#686 §2) will make this per folder.
  [[nodiscard]] bool can_write(std::string_view key) const noexcept;

  [[nodiscard]] disk_cache& cache() noexcept { return cache_; }

  static constexpr std::uint64_t kWholeTileBytes = 16ull << 20;
  static constexpr std::uint64_t kHeadTileBytes = 1ull << 20;

  // The tests: counters of what crossed the network.
  struct counters {
    std::atomic<std::uint64_t> bytes_read{0};
    std::atomic<std::uint64_t> whole_fetches{0};  // originals fetched whole and kept
    std::atomic<std::uint64_t> head_reads{0};
    std::atomic<std::uint64_t> listings{0};
  };
  [[nodiscard]] const counters& stats() const noexcept { return stats_; }

 private:
  struct server_rt;
  struct fetch_state;
  [[nodiscard]] std::shared_ptr<server_rt> find(std::string_view id) const;
  // The live link, connecting when there is none; sets the server's state.
  [[nodiscard]] expected connect(server_rt& s, bool person, const mobile::cancel_flag* cancel);
  void set_state(server_rt& s, reach r);
  [[nodiscard]] expected save_locked() const;  // m_ held
  [[nodiscard]] std::optional<identity> identity_of(std::string_view file_key);
  [[nodiscard]] std::string servers_file() const;
  [[nodiscard]] result<listing> list_live(server_rt& s, const key_parts& k, std::string_view folder_key, bool person,
                                          const mobile::cancel_flag* cancel);
  // The last listing of a folder, kept on disk for when its server does not answer.
  [[nodiscard]] std::string listing_file(std::string_view server, std::string_view folder_key) const;
  void keep_listing(std::string_view server, std::string_view folder_key, const listing& l) const;
  [[nodiscard]] std::optional<listing> kept_listing(std::string_view server, std::string_view folder_key);
  [[nodiscard]] expected fetch_into(server_rt& s, const key_parts& k, const identity& id,
                                    const mobile::cancel_flag* cancel, const progress_fn& progress);
  [[nodiscard]] driftwire::secrets::store& store();

  config cfg_;
  disk_cache cache_;
  std::unique_ptr<driftwire::secrets::store> own_store_;
  mutable std::mutex m_;
  std::map<std::string, std::shared_ptr<server_rt>> servers_;  // under m_
  // The last listing's size and mtime per file key, and each named folder's label.
  mutable std::mutex stamps_m_;
  std::map<std::string, std::pair<std::uint64_t, std::int64_t>, std::less<>> stamps_;
  std::map<std::string, std::string, std::less<>> labels_;  // folder key -> label
  std::mutex fetch_m_;
  std::map<std::string, std::shared_ptr<fetch_state>, std::less<>> fetching_;  // by file key
  std::atomic<std::uint64_t> gen_{1};
  counters stats_;
};

}  // namespace mv::remote
