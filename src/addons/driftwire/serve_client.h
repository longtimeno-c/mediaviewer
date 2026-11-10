// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The client's half of `driftwire serve` (serve.h, docs/design/28 "The NAS
// service"): a library channel, the capability check, and the four operations.
// M3's `remote_file` wraps this on the phone; the service's tests use it as
// the real thing. Worker threads only: every call blocks. Portable (D9).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "addons/driftwire/channel.h"
#include "addons/driftwire/serve_wire.h"
#include "core/result.h"

namespace mv::driftwire::serve {

// Why a connection could not be used.
enum class refusal : std::uint8_t { none, wrong_service, not_paired, too_old, busy, bad_code, too_many };

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
                                                                std::uint32_t window = 1024 * 1024, bool bulk = false);

  [[nodiscard]] result<std::vector<root_info>> roots(end_code* code = nullptr);
  [[nodiscard]] result<std::vector<list_entry>> list(const std::string& root, const std::string& rel,
                                                     end_code* code = nullptr);
  [[nodiscard]] result<stat_info> stat(const std::string& root, const std::string& rel, end_code* code = nullptr);

  // One request of the owner's (kAdminOp in `ops`). The answer's JSON. Refused,
  // or not the owner: status::permission_denied; `code` says how it ended.
  [[nodiscard]] result<std::string> admin(const std::string& request_json, end_code* code = nullptr);

  // Ends the session.
  void close() noexcept;
  [[nodiscard]] bool closed() const noexcept;
  // A session the service ended after it began ("revoked", "not-paired", "busy"); empty otherwise.
  [[nodiscard]] std::string ended_why() const;

 private:
  client() = default;
  struct state;
  std::shared_ptr<state> st_;
};

// §7.2: opens a channel under the claim key and asks to be the owner. The
// service's answer, or status::permission_denied / io when it said nothing.
struct claim_answer {
  std::array<std::uint8_t, 32> access_key{};
  std::array<std::uint8_t, 32> identity{};
  std::vector<root_info> roots;
};
// `prove` writes the service's nonce to kClaimProofFile on the share, so the
// service sees this desktop can write there, not only read the token.
// Every wait after the handshake ends by `deadline`. status::timeout only when
// the last answer, after the proof was written, did not come in time: the
// claim may still have gone through. Any earlier failure is a refusal or
// silence, status::permission_denied.
[[nodiscard]] result<claim_answer> claim(std::shared_ptr<link> l, const std::array<std::uint8_t, 32>& key,
                                         const std::array<std::uint8_t, 32>& device_key, const std::string& name,
                                         const std::function<bool(std::string_view nonce)>& prove,
                                         std::chrono::steady_clock::time_point deadline);

}  // namespace mv::driftwire::serve
