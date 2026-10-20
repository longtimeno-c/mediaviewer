// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Moving the files over an open channel (docs/design/28 "What moves").
//
// Sender: an offer (relative paths and sizes, nothing else), then, once the
// person on the other side accepts, each file as file_begin, data chunks and
// file_end carrying the BLAKE3 of the bytes as they were read. The original is
// only ever read (rule 5).
//
// Receiver: the same rule as io::verified_copy (docs/design/24), on bytes
// that arrive from a link instead of a file:
//   - each file goes to a sibling temporary ("<final>.mvtmp") created
//     exclusively, hashed as it is written;
//   - at file_end the hash must match the sender's, the temporary is flushed
//     to stable storage, read back uncached and hashed again;
//   - only then is it renamed into place, never over anything: a taken name
//     becomes "name (2).ext" (keep both, as F7 does);
//   - a cancel, a dropped link, a mismatch or any error removes the
//     temporary, so nothing partial is ever left. Files already verified
//     stay.
// Every path the sender names passes wire::safe_relative and stays inside the
// folder the person picked. Worker threads only (rule 1); nothing is logged
// with a path or a name (rule 6).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/channel.h"
#include "addons/driftwire/net.h"

namespace mv::driftwire {

struct outgoing_file {
  std::string path;  // absolute, on this machine (never sent)
  std::string rel;   // what the receiver sees: "IMG_1.JPG", "Trip/IMG_1.JPG"
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
};

struct incoming_file {
  std::string rel;
  std::uint64_t size = 0;
};

struct incoming_offer {
  std::vector<incoming_file> files;
  std::uint64_t bytes = 0;
  bool handover = false;  // #472 PR 5: the sender takes msg::places
  bool dht = false;       // #472: and meets on the DHT if both sides move
  // #501: the sender's long-term identity (known.h), for Remember. Nearby it
  // is not the handshake's static (a key for that link alone): it travels
  // here, sealed, once the sender's person has typed the right code. Absent
  // from a sender before #501, or one with nothing to remember with.
  std::optional<std::array<std::uint8_t, 32>> key;
};

// An identity key as the offer and the accept carry it ("key": 64 hex
// digits), and back (nullopt for anything else).
[[nodiscard]] std::string key_hex(const std::array<std::uint8_t, 32>& key);
[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> parse_key_hex(std::string_view hex);

// bytes moved so far (both ends), files finished so far.
using progress_fn = std::function<void(std::uint64_t bytes, std::uint32_t files)>;

// What a transfer polls and calls between chunks.
struct hooks {
  const std::atomic<bool>* cancel = nullptr;  // set: stop, say so to the other side
  progress_fn progress;                       // may be empty
  // Blocks while the viewer is presenting (the host table's should_yield,
  // as Import waits between buffers). May be empty.
  std::function<void()> yield;
  // Why a transfer ended, if it was not delivered: fixed ASCII with a source
  // line, for the host's log. May be empty.
  std::function<void(const char*)> note;
  // #472 PR 5, the sender: say in the offer that msg::places is understood,
  // and what to do with it (the receiver's places, its JSON). The receiver's
  // side sends it apart (send_places).
  bool handover = false;
  bool handover_dht = false;  // #472: the offer says it meets on the DHT too
  std::function<void(std::string_view json)> on_places;
  // #501, the sender: this machine's identity, said in the offer (after the
  // code), so the receiver can remember it. Empty: not said.
  std::optional<std::array<std::uint8_t, 32>> identity;
};

enum class outcome : std::uint8_t {
  delivered,  // every file arrived and was verified (or skipped, counted apart)
  declined,   // the other side said no
  busy,       // the other side is already in a transfer
  no_room,    // the destination does not have the space
  cancelled,  // this side or the other cancelled
  failed,     // the link dropped, or something did not verify
};
[[nodiscard]] const char* outcome_name(outcome o) noexcept;

struct transfer_result {
  outcome how = outcome::failed;
  std::uint32_t done = 0;     // files verified on the receiver
  std::uint32_t skipped = 0;  // files the sender could not read whole
  std::vector<std::string> written;  // receiver: final paths, in order (tests, the summary)
  // #501, the sender: the identity the receiver said in its accept.
  std::optional<std::array<std::uint8_t, 32>> peer_key;
};

// ---- sender ------------------------------------------------------------------
// The offer for `files`, as sealed JSON.
[[nodiscard]] std::string offer_json(const std::vector<outgoing_file>& files, bool handover = false,
                                     bool dht = false,
                                     const std::optional<std::array<std::uint8_t, 32>>& identity = std::nullopt);
// After the channel is open: offers, waits for the answer, calls
// `on_accepted`, then sends. The cancel flag is polled between chunks; the
// engine also shuts the channel down to end a wait.
[[nodiscard]] transfer_result send_files(channel& ch, const std::vector<outgoing_file>& files, const hooks& h,
                                         const std::function<void()>& on_accepted);

// ---- receiver ----------------------------------------------------------------
// Reads and checks the offer: a bad path, too many files, sizes that do not
// add up or a malformed message refuses it whole (status::corrupt).
// Waits at most `timeout_ms` (the sender's person confirming the code first).
[[nodiscard]] result<incoming_offer> read_offer(channel& ch, int timeout_ms);
[[nodiscard]] result<incoming_offer> parse_offer(std::string_view json);
// #472 PR 5, the receiver: where it can be reached from the internet, to a
// sender whose offer had "handover". Any thread (the channel's sends are).
// `dht`: this side also waits on the DHT if both sides move (the offer had
// "dht" too; dht_handover).
[[nodiscard]] expected send_places(channel& ch, const std::vector<net::endpoint>& places, bool dht = false);
// The sender: msg::places' JSON, read (at most kMaxPlaces, each checked).
inline constexpr std::size_t kMaxPlaces = 8;
[[nodiscard]] std::vector<net::endpoint> parse_places(std::string_view json);
[[nodiscard]] bool places_dht(std::string_view json);
// After the person accepted (msg::accept already sent): receives into
// `dest_dir`, which exists.
[[nodiscard]] transfer_result receive_files(channel& ch, const incoming_offer& offer, const std::string& dest_dir,
                                            const hooks& h);
// The answers the receiver sends before any byte moves. `identity` (#501):
// this machine's, said only now that its person has accepted.
[[nodiscard]] expected send_accept(channel& ch,
                                   const std::optional<std::array<std::uint8_t, 32>>& identity = std::nullopt);
[[nodiscard]] expected send_decline(channel& ch, outcome why);  // declined, busy or no_room

// "<final>.mvtmp", "<final>.mvtmp2" ... (the temporary io::verified_copy uses).
[[nodiscard]] std::string temp_name(const std::string& final_path, int attempt);

}  // namespace mv::driftwire
