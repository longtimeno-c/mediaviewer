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
// A cancel from either side goes through channel::cancel (#511): said
// reliably, and final, so neither end takes it for a lost link.
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
  std::int64_t mtime = 0;  // #530: the sender's, as offered (0: not said)
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
  // #498: the one folder every file is in, or empty when they are files
  // picked one by one (parse_offer refuses any other shape).
  std::string folder;
  // #530: the sender takes the accept's "have" and "at" (resume_plan), and
  // said each file's time, so a receive that stopped can be taken up again.
  bool resume = false;
};

// #530, the receiver: what it already has of an offer, from a receive by
// the same sender that stopped (resume.h). Said in its accept: the sender
// sends nothing of the files it has, and the partial one from `at` on.
struct resume_plan {
  struct whole_file {
    std::uint32_t index = 0;
    std::string final_path;
  };
  struct partial_file {
    std::uint32_t index = 0;
    std::uint64_t at = 0;  // the bytes kept; the sender sends from here
    std::string temp_path, final_path;
  };
  std::vector<whole_file> have;  // by index, ascending
  std::optional<partial_file> partial;
  std::string landed;  // a folder offer: the folder's name here, as last time
  [[nodiscard]] bool empty() const noexcept { return have.empty() && !partial; }
};

// The accept says `have` as runs of indexes; at most this many (a message's
// room, whatever the offer's shape).
inline constexpr std::size_t kMaxHaveRuns = 8192;

// #530, the receiver: what it holds of a receive so far, for a record a
// later transfer can resume from (hooks::keep).
struct kept_state {
  std::string landed;
  std::vector<resume_plan::whole_file> whole;  // the plan's, and every file verified since
  std::optional<resume_plan::partial_file> partial;  // `at`: its bytes on stable storage
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
  // #502, the receiver: nothing of the files for this long (and no file
  // being read back) ends the receive as failed. 0: no limit.
  int idle_ms = 0;
  // #530, the receiver: set, a receive that fails (not one cancelled) keeps
  // the file it was writing, hidden, flushed, for a later resume, and says
  // what it holds: at most every second while files arrive (the partial
  // checkpointed to stable storage every few seconds), and once at the end.
  // Unset: nothing partial stays, as before.
  std::function<void(const kept_state&)> keep;
  // #530: set with `cancel` when MediaViewer quits: that stop is kept to
  // resume as a failure is, not dropped as a person's cancel.
  const std::atomic<bool>* quitting = nullptr;
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

// The longest a cancel waits to be said before the link closes (#511): for
// the frame being written, then for the peer to acknowledge it.
inline constexpr int kCancelWaitMs = 250;

struct transfer_result {
  outcome how = outcome::failed;
  bool by_peer = false;       // cancelled (or failed) by the other side's msg::cancel
  std::uint32_t done = 0;     // files verified on the receiver
  std::uint32_t skipped = 0;  // files the sender could not read whole
  std::vector<std::string> written;  // receiver: final paths, in order (tests, the summary)
  // #530: files that were already here whole, and bytes not sent again
  // (theirs and the partial file's), both ends.
  std::uint32_t resumed_files = 0;
  std::uint64_t resumed_bytes = 0;
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
// `plan` (#530): what is here already, as said in the accept.
[[nodiscard]] transfer_result receive_files(channel& ch, const incoming_offer& offer, const std::string& dest_dir,
                                            const hooks& h, const resume_plan* plan = nullptr);
// The answers the receiver sends before any byte moves. `identity` (#501):
// this machine's, said only now that its person has accepted. `plan`
// (#530): to a sender whose offer had "resume", what it need not send.
[[nodiscard]] expected send_accept(channel& ch,
                                   const std::optional<std::array<std::uint8_t, 32>>& identity = std::nullopt,
                                   const resume_plan* plan = nullptr);
[[nodiscard]] expected send_decline(channel& ch, outcome why);  // declined, busy or no_room

// "<final>.mvtmp", "<final>.mvtmp2" ... (the temporary io::verified_copy uses).
[[nodiscard]] std::string temp_name(const std::string& final_path, int attempt);

}  // namespace mv::driftwire
