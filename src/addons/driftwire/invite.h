// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An internet code (issue #472 PR 2, "direct code"): what a receiver's window
// shows so a sender anywhere can reach it, with no server of anyone's.
//
// It holds a 48-bit secret (CPace's password, cpace.h) and up to four of the
// receiver's own public addresses: an IPv6 address, a port its router mapped
// (PCP, NAT-PMP, UPnP), or its address on that network. The addresses
// are encrypted under a key from the secret, so a link preview or a glance
// at a chat shows no address. A two-byte check catches a mistyped code here,
// before it is spent on the receiver (a code dies at its first attempt).
//
// Text: Crockford base32 in groups of four ("7KQ2-M9XA-..."), case and
// O/0, I/L/1 ignored; or a link, mediaviewer://driftwire?code=<text>. An
// IPv4-only code is 26 characters; one IPv6 address alone, 45; four places at
// most, about 90; a relay place adds 16 bytes of token (about 26 more). (The issue's "about 16 characters" cannot hold an address
// and a secret; the link and a QR code carry the long ones.)
//
// Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/net.h"

namespace mv::driftwire::invite {

inline constexpr std::size_t kSecretBytes = 6;
inline constexpr std::size_t kMaxPlaces = 4;
inline constexpr std::uint8_t kVersion = 2;  // the protocol it opens
inline constexpr const char* kLinkPrefix = "mediaviewer://driftwire?code=";

// #472 relays: the receiver's relay server (the advanced "Relay (your own
// server)") and the token it waits under there (splice.h). Covered with the
// places, so only someone with the code learns either, besides that server:
// the receiver tells it the token, and a sender names it by a hash, only
// inside a sealed handshake (protocol 3, #500), never in the clear. It counts
// as one of the kMaxPlaces; a code with one is opened only by a build with
// relays.
struct relay_place {
  net::endpoint at;
  std::array<std::uint8_t, 16> token{};
};

struct code {
  std::array<std::uint8_t, kSecretBytes> secret{};
  std::vector<net::endpoint> places;  // IPv4 dotted or IPv6 text, a port each; at most kMaxPlaces
  std::optional<relay_place> relay;   // tried after the places (#472 relays)
};

// A fresh secret, for these places (the first kMaxPlaces that parse, one
// fewer with a relay).
[[nodiscard]] code make(const std::vector<net::endpoint>& places, std::optional<relay_place> relay = std::nullopt);
[[nodiscard]] std::string encode(const code& c);
[[nodiscard]] std::string link(const code& c);
// The text or the link, as a person pasted or typed it. Empty: not a code
// (wrong length, a typo the check caught, an address that is not one).
[[nodiscard]] std::optional<code> decode(std::string_view text);

}  // namespace mv::driftwire::invite
