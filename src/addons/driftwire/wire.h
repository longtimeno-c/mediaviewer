// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's wire format (docs/design/28 "The wire"): what two MediaViewers
// say to each other, kept apart from sockets and Bluetooth so it is tested on
// its own.
//
//   - A beacon is one UDP datagram: "MVSP1" and a small JSON object with the
//     sender's random session id, its machine name and the TCP port it listens
//     on. Never a path, a file name or anything about a file (rule 6).
//   - A link (TCP, or the Bluetooth pipe) starts with a plain hello: protocol
//     1's X25519 commitment, or protocol 2's first Noise message (#472);
//     everything after the handshake is sealed (channel.h).
//   - The short code both screens show is derived from the handshake, so a
//     machine in the middle shows a different code on each side.
//   - A receiver never writes outside the folder the person chose:
//     safe_relative() is the only gate a sender's path passes through.
//
// Portable (D9): no socket, no platform header.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mv::driftwire {

// The protocol a client speaks when the peer has it (#472: Noise, hybrid
// post-quantum, a session that rejoins over any bearer). A peer that announces
// none speaks protocol 1, which still works on the LAN and Bluetooth.
inline constexpr std::uint32_t kProtocolVersion = 2;
inline constexpr std::uint32_t kProtocol1 = 1;
// The beacon's and the Bluetooth INFO's own "v": protocol 1 clients ignore a
// beacon with any other, so it stays 1 and "p" says the protocol.
inline constexpr std::uint32_t kBeaconVersion = 1;
// Discovery: one UDP port, sent to an organisation-local multicast group and
// to the subnet's broadcast address (an access point that drops one usually
// passes the other).
inline constexpr std::uint16_t kBeaconPort = 47613;
inline constexpr const char* kMulticastGroup = "239.255.77.83";
inline constexpr std::size_t kMaxBeaconBytes = 512;
inline constexpr std::size_t kMaxNameBytes = 64;
// A sealed message's plaintext, at most. File bytes travel in chunks of
// kChunkBytes, each its own message.
inline constexpr std::size_t kChunkBytes = 256 * 1024;
inline constexpr std::size_t kMaxMessageBytes = kChunkBytes + 4096;
// Timeouts (milliseconds). Both ends send a ping at least every kPingMs while
// a link is open, so a silent link is a dropped one.
inline constexpr int kPingMs = 4000;
inline constexpr int kSilenceMs = 20000;
inline constexpr int kConnectMs = 8000;
// How long a person has to confirm the code (sender) or answer an offer
// (receiver) before it is called off.
inline constexpr int kAskMs = 180000;
// Protocol 2's session (#472). Nothing heard for kSuspectMs and the link is
// suspect: the sender races the other bearers. kRejoinMs without a link ends
// the session, and so do kFlapRejoins rejoins in kFlapMs. A ping at least
// every kPing2Ms (it carries the acknowledgement), and an acknowledgement at
// least every kAckBytes received. At most kReplayBytes are kept unacknowledged.
inline constexpr int kSuspectMs = 3000;
inline constexpr int kRejoinMs = 20000;
inline constexpr int kFlapMs = 10000;
inline constexpr int kFlapRejoins = 3;
inline constexpr int kPing2Ms = 1000;
inline constexpr std::size_t kAckBytes = 1u * 1024 * 1024;
inline constexpr std::size_t kReplayBytes = 16u * 1024 * 1024;
// After a move to another link, the sender stays at least this long before
// moving for speed (a dropped link moves at once).
inline constexpr int kSwitchHoldMs = 10000;
// One offer, at most.
inline constexpr std::size_t kMaxFiles = 100000;
inline constexpr std::size_t kMaxRelativeBytes = 1024;
// A part of a relative path, at most: room for ".mvtmp16" under any
// filesystem's 255-byte name limit.
inline constexpr std::size_t kMaxPartBytes = 240;
// One file's size, at most (an offer above it is refused whole).
inline constexpr std::uint64_t kMaxFileBytes = 1ull << 50;

// `internet`: a TCP link over the internet with a code (#472 PR 2).
enum class transport : std::uint8_t { wifi, bluetooth, internet };
[[nodiscard]] const char* transport_name(transport t) noexcept;

// Sealed message types (the first plaintext byte).
enum class msg : std::uint8_t {
  ident = 1,       // {"id","name"}: who this is, now that nobody can alter it
  offer = 2,       // {"count","bytes","files":[{"rel","size"}]} (sender -> receiver)
  accept = 3,      // {} (receiver)
  decline = 4,     // {"why"} (receiver): declined, busy, no room
  file_begin = 5,  // {"i","size","mtime"} (sender)
  data = 6,        // raw bytes of the current file (sender)
  file_end = 7,    // 32 bytes: BLAKE3 of the file as read (sender)
  file_skip = 8,   // {"i"}: the sender could not read it whole (sender)
  file_done = 9,   // {"i","ok"}: written, read back and verified (receiver)
  done = 10,       // {} (sender): nothing more
  cancel = 11,     // {} (either)
  ping = 12,       // {} (either)
  // Protocol 2 only: the first sealed message on a rejoined link, either way:
  // 8 bytes, the count of messages received so far (what to re-send from).
  resume_at = 13,
  // Protocol 2, #472 PR 5: {"places":[{"a","p"},...]} (receiver -> sender),
  // where the receiver can be reached from the internet, for the sender to
  // rejoin if the local link goes. Sent only to a sender whose offer had
  // "handover": a sender before PR 5 would end the link on it.
  places = 14,
  // Protocol 2, #472 relays through the person's own computer (relay.h),
  // only on a link between two of that person's remembered devices, never
  // to a stranger. 15-19 are left for other work on the stack.
  relay = 20,         // {"places":[{"a","p"},...]} (A -> R): connect onward to these
  relay_answer = 21,  // {"ok":bool,"why"} (R -> A); after ok, only relay_data either way
  relay_data = 22,    // raw bytes of A's link to B, sealed again between A and R
};

// "123 456": six digits from both public keys (client = the sender).
// Protocol 1.
[[nodiscard]] std::string pairing_code(std::span<const std::uint8_t, 32> client_pk,
                                       std::span<const std::uint8_t, 32> server_pk);
// "123 456": six digits from protocol 2's handshake hash after the server's
// message and the client's revealed nonce.
[[nodiscard]] std::string pairing_code_v2(std::span<const std::uint8_t> handshake_hash,
                                          std::span<const std::uint8_t, 32> client_nonce);

// A sender's relative path ("Trip/IMG_1.JPG"): '/'-separated, every part a
// name any of the two OSes can hold, nothing that climbs out, no device name,
// valid UTF-8. The receiver refuses the whole offer if any path fails.
[[nodiscard]] bool safe_relative(std::string_view rel) noexcept;
// The last part of a path, either separator.
[[nodiscard]] std::string_view leaf_name(std::string_view path) noexcept;
// The first part of a '/'-separated path: "Trip" for "Trip/a.jpg", the whole of "a.jpg".
[[nodiscard]] std::string_view top_part(std::string_view rel) noexcept;

struct beacon {
  std::string id;    // 16 lowercase hex digits, random per session
  std::string name;  // the machine's name, cut to kMaxNameBytes at a character
  std::uint16_t port = 0;
  std::uint32_t protocol = kProtocol1;  // "p": the highest it speaks (none: 1)
};
[[nodiscard]] std::string encode_beacon(const beacon& b);
[[nodiscard]] std::optional<beacon> decode_beacon(std::span<const std::uint8_t> datagram);
// A beacon's or INFO's "p" as the protocol to speak to it: none, or below 2,
// is protocol 1; above this build's, this build's.
[[nodiscard]] std::uint32_t protocol_of(std::optional<std::int64_t> p) noexcept;

// A machine name, cut to kMaxNameBytes without splitting a character, with
// control characters dropped. Empty input gives "MediaViewer".
[[nodiscard]] std::string clean_name(std::string_view raw);
[[nodiscard]] bool valid_id(std::string_view id) noexcept;

// "340 MB", "1.2 GB", "12 KB".
[[nodiscard]] std::string size_text(std::uint64_t bytes);

}  // namespace mv::driftwire
