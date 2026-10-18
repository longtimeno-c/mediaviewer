// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// An internet code (issue #472 PR 2, "direct code"): what a receiver's window
// shows so a sender anywhere can reach it, with no server of anyone's.
//
// It holds a 48-bit secret (CPace's password, cpace.h) and up to four of the
// receiver's own public addresses: an IPv6 address, a port its router mapped
// (PCP, NAT-PMP, UPnP), or one the person forwarded by hand. The addresses
// are encrypted under a key from the secret, so a link preview or a glance
// at a chat shows no address. A two-byte check catches a mistyped code here,
// before it is spent on the receiver (a code dies at its first attempt).
//
// Text: Crockford base32 in groups of four ("7KQ2-M9XA-..."), case and
// O/0, I/L/1 ignored; or a link, mediaviewer://sharepoint?code=<text>. An
// IPv4-only code is 26 characters; one IPv6 address alone, 45; four places at
// most, about 90. (The issue's "about 16 characters" cannot hold an address
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

#include "addons/sharepoint/net.h"

namespace mv::sharepoint::invite {

inline constexpr std::size_t kSecretBytes = 6;
inline constexpr std::size_t kMaxPlaces = 4;
inline constexpr std::uint8_t kVersion = 2;  // the protocol it opens
inline constexpr const char* kLinkPrefix = "mediaviewer://sharepoint?code=";

struct code {
  std::array<std::uint8_t, kSecretBytes> secret{};
  std::vector<net::endpoint> places;  // IPv4 dotted or IPv6 text, a port each; at most kMaxPlaces
};

// A fresh secret, for these places (the first kMaxPlaces that parse).
[[nodiscard]] code make(const std::vector<net::endpoint>& places);
[[nodiscard]] std::string encode(const code& c);
[[nodiscard]] std::string link(const code& c);
// The text or the link, as a person pasted or typed it. Empty: not a code
// (wrong length, a typo the check caught, an address that is not one).
[[nodiscard]] std::optional<code> decode(std::string_view text);

}  // namespace mv::sharepoint::invite
