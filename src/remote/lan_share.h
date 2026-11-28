// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A paired server's LAN SMB share, mounted in the Finder when the desktop
// reaches it at home (docs/plans/desktop-client.md §6a "The share in the
// Finder"). The host makes one SMB share per named folder, named
// share_users::share_name(label) (docs/plans/self-host-users-smb.md §2), and
// never forwards SMB on the router, so this mounts only when the link that
// answered was the server's LAN address: never over the internet, the relay,
// or a public address.
//
// What to mount is decided here, portable and pure (D9, no I/O); the mount
// itself is the host's (shell/lan_share_mac.mm, NetFS). No password passes
// through here: the OS's own sign-in and its Keychain keep it.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mv::remote::lan_share {

// The host of a route's "host:port" when it is on this network: a private
// IPv4 (10/8, 172.16/12, 192.168/16), IPv4 link-local (169.254/16), an IPv6
// unique-local address (fc00::/7, given as "[fd..]:port"), or a Bonjour name
// ending in ".local". "" for anything else (a public address or name, loopback,
// IPv6 link-local, which needs a zone a URL cannot carry well).
[[nodiscard]] std::string lan_host(std::string_view address);

// One SMB volume mounted on this computer, from its "mounted from" text
// ("//user@host/share" or "//host/share", the share percent-encoded or not).
struct mounted {
  std::string host;   // lowercase
  std::string share;  // decoded, as given
};
[[nodiscard]] std::optional<mounted> parse_mounted_from(std::string_view from);

// "smb://192.168.0.79/My%20Photos" for a host and a named folder's label; an
// IPv6 host in brackets. "" without either.
[[nodiscard]] std::string share_url(std::string_view host, std::string_view label);

struct input {
  bool enabled = true;  // Settings: "Mount its shares in the Finder at home" (on by default)
  // How the last connect reached the server (mobile::route_report).
  std::string address;
  bool over_internet = false;
  bool relayed = false;
  std::vector<std::string> labels;   // the server's named folders
  std::vector<mounted> already;      // SMB volumes mounted now
};

enum class skip : unsigned char {
  none,
  off,          // the setting is off
  away,         // reached over the internet or the relay, or not on a LAN address
  no_folders,   // the server names no folders
  all_mounted,  // every share is mounted already
};

struct plan {
  std::string host;               // where the shares are ("" when skipped as away)
  std::vector<std::string> urls;  // to mount, one per share not mounted yet, in the labels' order
  skip why = skip::none;          // set when `urls` is empty
};

[[nodiscard]] plan plan_mounts(const input& in);

// The share volumes of `host` among those mounted (a share-name match is
// case-insensitive, as SMB's is). What Unmount takes away.
[[nodiscard]] std::vector<mounted> mounted_from(std::string_view host, const std::vector<mounted>& already);

// What the notice says after an automatic or asked-for mount. `server`: the
// server's name. `mounted`: shares mounted now; `failed`: shares that did not.
// `needs_sign_in`: the OS had no saved sign-in (the automatic mount never
// shows its dialog). "" when there is nothing to say (all mounted quietly
// in the background, or nothing to do).
struct outcome {
  int mounted = 0;
  int failed = 0;
  bool needs_sign_in = false;
  bool asked = false;  // the person chose Mount (the menu), not the connect
};
[[nodiscard]] std::string notice_words(std::string_view server, const outcome& o);
// For the menu when nothing could be mounted: why, in words.
[[nodiscard]] std::string skip_words(std::string_view server, skip why);

}  // namespace mv::remote::lan_share
