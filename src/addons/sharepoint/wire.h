// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Sharepoint's wire format (docs/design/28 "The wire"): what two MediaViewers
// say to each other, kept apart from sockets and Bluetooth so it is tested on
// its own.
//
//   - A beacon is one UDP datagram: "MVSP1" and a small JSON object with the
//     sender's random session id, its machine name and the TCP port it listens
//     on. Never a path, a file name or anything about a file (rule 6).
//   - A link (TCP, or the Bluetooth pipe) starts with a plain hello carrying an
//     X25519 public key; everything after it is sealed (channel.h).
//   - The short code both screens show is derived from both public keys, so a
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

namespace mv::sharepoint {

inline constexpr std::uint32_t kProtocolVersion = 1;
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
// One offer, at most.
inline constexpr std::size_t kMaxFiles = 100000;
inline constexpr std::size_t kMaxRelativeBytes = 1024;
// A part of a relative path, at most: room for ".mvtmp16" under any
// filesystem's 255-byte name limit.
inline constexpr std::size_t kMaxPartBytes = 240;
// One file's size, at most (an offer above it is refused whole).
inline constexpr std::uint64_t kMaxFileBytes = 1ull << 50;

enum class transport : std::uint8_t { wifi, bluetooth };
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
};

// "123 456": six digits from both public keys (client = the sender).
[[nodiscard]] std::string pairing_code(std::span<const std::uint8_t, 32> client_pk,
                                       std::span<const std::uint8_t, 32> server_pk);

// A sender's relative path ("Trip/IMG_1.JPG"): '/'-separated, every part a
// name any of the two OSes can hold, nothing that climbs out, no device name,
// valid UTF-8. The receiver refuses the whole offer if any path fails.
[[nodiscard]] bool safe_relative(std::string_view rel) noexcept;
// The last part of a path, either separator.
[[nodiscard]] std::string_view leaf_name(std::string_view path) noexcept;

struct beacon {
  std::string id;    // 16 lowercase hex digits, random per session
  std::string name;  // the machine's name, cut to kMaxNameBytes at a character
  std::uint16_t port = 0;
};
[[nodiscard]] std::string encode_beacon(const beacon& b);
[[nodiscard]] std::optional<beacon> decode_beacon(std::span<const std::uint8_t> datagram);

// A machine name, cut to kMaxNameBytes without splitting a character, with
// control characters dropped. Empty input gives "MediaViewer".
[[nodiscard]] std::string clean_name(std::string_view raw);
[[nodiscard]] bool valid_id(std::string_view id) noexcept;

// "340 MB", "1.2 GB", "12 KB".
[[nodiscard]] std::string size_text(std::uint64_t bytes);

}  // namespace mv::sharepoint
