// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// CPace (draft-irtf-cfrg-cpace-18), the CFRG's balanced PAKE, in its
// ristretto255 / SHA-512 suite, initiator-responder mode (issue #472, PR 2).
//
// An internet code is short; whoever knows it is the peer. CPace turns it
// into a 64-byte key (ISK) both sides share only if both used the same code,
// and a machine in the middle gets one live guess per run and nothing to
// test guesses against offline: the messages are points that every code
// explains equally well. Protocol 2's internet handshake keys Noise with it
// (channel.cpp: XXhfs+psk0, the ISK's first 32 bytes as the psk).
//
// libsodium's ristretto255 does the group work: the generator from the code
// (crypto_core_ristretto255_from_hash, the one-way map), the scalar
// multiplication with its identity check (scalar_mult_vfy). Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/result.h"

namespace mv::driftwire::cpace {

inline constexpr std::size_t kPointBytes = 32;
inline constexpr std::size_t kIskBytes = 64;

// generator_string(DSI, PRS, CI, sid, 128) of the draft (§7.1): exposed for
// the test vectors.
[[nodiscard]] std::vector<std::uint8_t> generator_string(std::span<const std::uint8_t> prs,
                                                         std::span<const std::uint8_t> ci,
                                                         std::span<const std::uint8_t> sid);

// One side of a run. `prs`: the code's secret; `ci`: the channel identifier
// (both sides' roles); `sid`: the session id (the initiator's random 16
// bytes, sent in the clear). `fixed_scalar`: the test vectors only.
class party {
 public:
  party(std::span<const std::uint8_t> prs, std::span<const std::uint8_t> ci, std::span<const std::uint8_t> sid,
        std::optional<std::array<std::uint8_t, 32>> fixed_scalar = std::nullopt);
  ~party();
  party(const party&) = delete;
  party& operator=(const party&) = delete;

  // False when the code gave no usable generator (never, short of a broken hash).
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  // This side's message (Ya for the initiator, Yb for the responder).
  [[nodiscard]] const std::array<std::uint8_t, kPointBytes>& message() const noexcept { return y_pub_; }
  // The ISK from the peer's message, with each side's associated data
  // (initiator first, as the draft's transcript_ir orders them).
  // status::corrupt for a point that is not one, or one that gives the
  // identity (scalar_mult_vfy). The scalar is wiped either way.
  [[nodiscard]] result<std::array<std::uint8_t, kIskBytes>> finish(std::span<const std::uint8_t> peer_message,
                                                                   bool initiator, std::span<const std::uint8_t> ad_initiator,
                                                                   std::span<const std::uint8_t> ad_responder);

 private:
  std::array<std::uint8_t, 32> y_{};
  std::array<std::uint8_t, kPointBytes> y_pub_{};
  std::vector<std::uint8_t> sid_;
  bool ok_ = false;
};

}  // namespace mv::driftwire::cpace
