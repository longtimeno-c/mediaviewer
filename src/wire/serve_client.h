// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The client's half of `driftwire serve` (serve.h, docs/design/28 "The NAS
// service"): a library channel, the capability check, and the four operations.
// M3's `remote_file` wraps this on the phone; the service's tests use it as
// the real thing. Worker threads only: every call blocks. Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <span>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wire/channel.h"
#include "wire/owner_pass.h"
#include "wire/serve_wire.h"
#include "core/result.h"

namespace mv::driftwire::serve {

// Why a connection could not be used.
// `expired`: a limited device whose time is up (docs/plans/nas-owner-and-guests.md
// §4.3); an app from before it reads that as not_paired.
enum class refusal : std::uint8_t { none, wrong_service, not_paired, too_old, busy, bad_code, too_many, expired };

class client {
 public:
  struct options {
    std::array<std::uint8_t, 32> access_key{};
    std::array<std::uint8_t, 32> device_key{};                 // this phone's X25519 private key
    std::optional<std::array<std::uint8_t, 32>> service_key;   // pinned: the service's public identity
    std::string id = "0000000000000000";                       // 16 hex digits
    std::string name = "client";
    std::uint32_t ops = kAllOps;                               // what this client can use
    std::string pair_code;                                     // from the QR: redeemed before serve_hello
    std::string device_name;                                   // what the desktop shows (with pair_code)
    std::string device_kind;                                   // kDeviceDesktop for a desktop ("": a phone)
  };

  // Handshakes over `l`, says what it can do, reads what the service can.
  // A wrong access key gets silence: status::io or ::timeout, `why` none.
  // Other refusals set `why` (when non-null) and are status::permission_denied
  // or, for a service too old for this client, status::unsupported_format.
  [[nodiscard]] static result<std::unique_ptr<client>> open(std::shared_ptr<link> l, const options& o,
                                                            refusal* why = nullptr);
  ~client();
  client(const client&) = delete;
  client& operator=(const client&) = delete;

  [[nodiscard]] const caps_msg& caps() const noexcept;

  // One stream of a read. next() returns chunks in order until nullopt (the
  // end); code() then says how it ended. cancel() ends it where it is; a
  // new read_range is the seek.
  class read_stream {
   public:
    struct chunk {
      std::uint64_t offset = 0;
      std::vector<std::uint8_t> bytes;
    };
    ~read_stream();
    [[nodiscard]] result<std::optional<chunk>> next(int timeout_ms);
    [[nodiscard]] end_code code() const;
    void cancel();

   private:
    friend class client;
    read_stream() = default;
    struct state;
    std::shared_ptr<state> st_;
  };

  // `window`: bytes on the wire and queued here at once, at most (the
  // stream's credit, topped up as chunks are consumed). `bulk`: larger chunks
  // for a whole-file pull. `at` past the end reads nothing.
  [[nodiscard]] result<std::unique_ptr<read_stream>> read_range(const std::string& root, const std::string& rel,
                                                                std::uint64_t at, std::uint64_t length,
                                                                std::uint32_t window = 1024 * 1024, bool bulk = false,
                                                                bool meta = false);

  // Clips this phone can't play (docs/plans/phone-clip-convert.md; kConvertOp in caps). The
  // plan: status::unsupported_format when the service lists no `convert`, or answers it
  // unsupported (`why` gets kWhyNotAClip or kWhyConvertOff); not_found, permission_denied as stat.
  [[nodiscard]] result<convert_plan> convert_plan_of(const std::string& root, const std::string& rel,
                                                     std::string* why = nullptr);
  // Segment k's MPEG-TS, as a read_range of it. Its first chunk comes when the service has
  // converted it, which may take a few seconds (a next() timeout is not the end).
  [[nodiscard]] result<std::unique_ptr<read_stream>> convert_segment(const std::string& root, const std::string& rel,
                                                                     std::uint32_t k, std::uint32_t window = 1024 * 1024);

  [[nodiscard]] result<std::vector<root_info>> roots(end_code* code = nullptr);
  // `meta`: under a backup folder's kBackupMetaFolder (serve_wire.h, "writes").
  // `truncated`, when given, says the service cut the listing short (kMaxListBytes).
  [[nodiscard]] result<std::vector<list_entry>> list(const std::string& root, const std::string& rel,
                                                     end_code* code = nullptr, bool meta = false, bool* truncated = nullptr);
  [[nodiscard]] result<stat_info> stat(const std::string& root, const std::string& rel, end_code* code = nullptr,
                                       bool meta = false);

  // Phone backup: writes one file into a backup folder (serve_wire.h, "writes";
  // kWriteOp in caps). `read` fills its span from the file at an offset and
  // returns how much it filled (short only at the end). `progress` gets the
  // bytes sent so far. A refusal is status::permission_denied with `why` set
  // (kWhyNotAllowed, kWhyLimit, kWhyFull, kWhyOffset with `have`, kWhyHash,
  // kWhyBusy); `cancel` set is status::cancelled and leaves the service's
  // unfinished copy, to go on from with `at` = its `have`.
  struct write_done {
    std::string name;       // where it is now, relative to the folder
    bool existing = false;  // it was there already: nothing was sent
  };
  struct write_refused {
    std::string why;
    std::uint64_t have = 0;
  };
  using read_fn = std::function<result<std::size_t>(std::uint64_t offset, std::span<std::uint8_t> into)>;
  [[nodiscard]] result<write_done> write(const std::string& root, const std::string& rel, std::uint64_t size,
                                         const std::string& hash_hex, std::uint64_t at, const read_fn& read,
                                         const std::function<void(std::uint64_t)>& progress,
                                         const std::atomic<bool>* cancel, write_refused* why, bool meta = false,
                                         std::int64_t taken = kNoTime);

  // ---- changing the shared folders (serve_wire.h "changing"; docs/design/28 "Changing files") ----
  // For a device whose role (caps().role) is "change". Ask for kWriteOp | kChangeOps in
  // options::ops. Show New folder, Move, Rename and Delete only when changes_files() is true,
  // and on a folder only when its root_info.writable is (roots(), read again after a role
  // change). Every call blocks until the service answers or the link drops (a move across
  // volumes copies, which may take minutes on a big folder); none is cancelled once sent.
  // Paths are a root id and a '/'-separated `rel` inside it, never empty (not the folder
  // itself) and never hidden (a part starting with '.').
  //
  // A refusal fills `why` (when non-null) with a typed reason and the service's word, and is:
  //   status::permission_denied  refused (not_allowed, path, exists, busy, name, limit, full,
  //                              offset)
  //   status::not_found          not_found, or offline (the folder's drive is gone)
  //   status::unsupported_format unsupported: the service lists no such op (older than this)
  //   status::corrupt            hash (add only: send it again)
  //   status::io                 failed: the server's disk, or the link
  enum class change_why : std::uint8_t {
    none,
    unsupported,  // the service is older than these ops
    not_allowed,  // "not-allowed": not "change" now, outside its grant, or the Catalogue
    path,         // "path": the folder itself, a hidden or unsafe name, a folder into itself
    exists,       // "exists": the destination is taken; nothing was replaced (pick another name)
    busy,         // "busy": another request is on that name now (try again)
    name,         // "name": the last part is longer than kMaxWriteName
    limit,        // "limit": past a backup folder's storage limit
    full,         // "full": the server's disk would keep less than its reserve free
    offline,      // "offline": the folder's drive is not there now
    not_found,    // the source is not there (moved or deleted elsewhere)
    hash,         // "hash" (add): what reached the disk is not what was sent
    offset,       // "offset" (add): `at` is not where the unfinished copy ends; see `have`
    failed,       // the server's disk failed it, or the link dropped
  };
  struct change_refused {
    change_why why = change_why::none;
    std::string word;       // the service's own word ("" when it gave none)
    std::uint64_t have = 0; // offset: the bytes of this device's unfinished copy
  };
  // The typed reason for a service word ("not-allowed", "exists", ...); `failed` for one this
  // build does not know.
  [[nodiscard]] static change_why change_why_of(std::string_view word) noexcept;
  struct change_done {
    // Where it is now, relative to its folder: the new folder (make_folder), the file (add: a
    // taken name gets " (2)"), the destination (move), or its place in the folder's trash,
    // "YYYY-MM-DD/<rel>" (remove).
    std::string rel;
    bool existing = false;  // make_folder: it was there; add: an identical file was there
  };
  // (caps().ops & kChangeOps) == kChangeOps: the service has make_folder, move and remove, and
  // takes add in every writable folder. False from a service older than them.
  [[nodiscard]] bool changes_files() const noexcept;
  // Adds one file to `rel` (its folders made), as write() but in any writable folder of a
  // service that changes_files(), and with typed refusals. Never replaces: a taken name that
  // is not this file gets " (2)"; this very file already there is ok with `existing`.
  // Resumable: after a cancel (status::cancelled) or a dropped link, call again with `at` =
  // `why->have` from the offset refusal.
  [[nodiscard]] result<change_done> add(const std::string& root, const std::string& rel, std::uint64_t size,
                                        const std::string& hash_hex, std::uint64_t at, const read_fn& read,
                                        const std::function<void(std::uint64_t)>& progress,
                                        const std::atomic<bool>* cancel, change_refused* why = nullptr);
  // Makes the folder `rel` and any missing above it. A folder already there is ok, `existing`.
  [[nodiscard]] result<change_done> make_folder(const std::string& root, const std::string& rel,
                                                change_refused* why = nullptr);
  // Moves or renames a file or folder to `to_rel` in `to_root` (the same root, or another this
  // device may change). Never replaces: a taken destination is `exists`. The folders above
  // `to_rel` are made.
  [[nodiscard]] result<change_done> move(const std::string& root, const std::string& rel, const std::string& to_root,
                                         const std::string& to_rel, change_refused* why = nullptr);
  // Deletes a file or folder: it goes to the folder's hidden trash on the server, from which
  // only its owner removes it (on the host). No listing shows it after.
  [[nodiscard]] result<change_done> remove(const std::string& root, const std::string& rel,
                                           change_refused* why = nullptr);

  // One request of the owner's (kAdminOp in `ops`). The answer's JSON. Refused,
  // or not the owner: status::permission_denied; `code` says how it ended,
  // and `refusal` gets the stream's JSON however it ended ({"why",...} on a
  // refusal; empty from a service that gave none).
  [[nodiscard]] result<std::string> admin(const std::string& request_json, end_code* code = nullptr,
                                          std::string* refusal = nullptr);

  // The owner's passphrase (docs/plans/nas-owner-and-guests.md §3.2): asks for
  // a challenge, has `derive` make the signer from the salt and cost the
  // service sent (Argon2id runs there, on this thread), and answers it. The
  // answer's JSON ({"unlocked_s"}). Refused: status::permission_denied, and
  // `refusal` says why ({"why":"passphrase"|"locked-out"|...}). A cost outside
  // owner::sane_cost is never paid: status::corrupt.
  using derive_fn = std::function<std::optional<owner::signer>(const owner::salt_t&, std::uint64_t ops, std::uint64_t mem)>;
  [[nodiscard]] result<std::string> unlock(const derive_fn& derive, std::string* refusal = nullptr,
                                           std::uint64_t min_ops = owner::kFloorOps, std::uint64_t min_mem = owner::kFloorMem);

  // Ends the session.
  void close() noexcept;
  [[nodiscard]] bool closed() const noexcept;
  // A session the service ended after it began ("revoked", "expired",
  // "folders-changed", "not-paired", "busy"); empty otherwise.
  [[nodiscard]] std::string ended_why() const;

 private:
  client() = default;
  [[nodiscard]] result<change_done> change(open_request& r, std::uint32_t needs, change_refused* why);
  struct state;
  std::shared_ptr<state> st_;
};

// §7.2: opens a channel under the claim key and asks to be the owner. The
// service's answer, or status::permission_denied / io when it said nothing.
struct claim_answer {
  std::array<std::uint8_t, 32> access_key{};
  std::array<std::uint8_t, 32> identity{};
  std::vector<root_info> roots;
  // The service took the passphrase step (asked for or took one). False
  // from a service older than that, which kept no passphrase.
  bool passphrase_step = false;
};
// `prove` writes the service's nonce to kClaimProofFile on the share, so the
// service sees this desktop can write there, not only read the token.
// Every wait after the handshake ends by `deadline`. status::timeout when the
// last answer, after the proof was written, did not arrive (late, or the link
// dropped): the claim may still have gone through, and claiming again from
// the same device returns the same keys. Any earlier failure is a refusal or
// silence, status::permission_denied.
//
// The owner's passphrase (docs/plans/nas-owner-and-guests.md §3.3). With
// `pass`, the claim says it can take the passphrase step: a service that has
// one asks for a signature, made by `pass->sign` from the salt and cost it
// sends (Argon2id runs there); one that has none takes `pass->set`, and the
// claim fails with nothing to set. Without `pass`, the claim is as before
// this, and a service with a passphrase refuses it. `why` gets the service's
// reason when it gave one ("passphrase", "locked-out", "slow-down", ...).
struct claim_passphrase {
  std::function<std::optional<owner::signer>(const owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem)> sign;
  std::optional<owner::verifier> set;
  // The service's identity when this side knows it (`serve status` shows
  // it; a re-claim has it): any other is refused before anything is sent.
  std::optional<std::array<std::uint8_t, 32>> service_key;
  // The least cost a challenge may ask for: a fake service asking for less
  // would get an answer cheap to attack offline.
  std::uint64_t min_ops = owner::kFloorOps;
  std::uint64_t min_mem = owner::kFloorMem;
};
[[nodiscard]] result<claim_answer> claim(std::shared_ptr<link> l, const std::array<std::uint8_t, 32>& key,
                                         const std::array<std::uint8_t, 32>& device_key, const std::string& name,
                                         const std::function<bool(std::string_view nonce)>& prove,
                                         std::chrono::steady_clock::time_point deadline,
                                         const claim_passphrase* pass = nullptr, std::string* why = nullptr);

}  // namespace mv::driftwire::serve
