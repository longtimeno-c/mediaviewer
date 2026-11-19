// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Add-ons → Phone access as a view tree (docs/design/25 §10), drawn by both
// hosts in their Phone access section (docs/plans/phone-access-addon.md §2).
// One copy of the card's rows and words for the Mac and Windows, where before
// there were two (PhoneAccessView.swift, IslandHost.Phone.cs + PhoneWords.cs):
// the server (this computer, a Synology, something else), the folder on it,
// what the phone searches, claiming, the passphrase, the code a phone scans
// with its check code, the phones, the relay, the shared folders, and handing
// the library to or from another computer.
//
// build() reads the link's state (link::state_json) and this screen's own
// state (what was typed, which part is open) and writes the tree. on_event()
// turns a click or a change into the link's requests ({"do":...}), exactly the
// ones the cards sent. What is typed in a secure field (the server passphrase,
// the relay key, the DuckDNS token) is kept here until it is sent, never in a
// tree (the field is "secure": its value never comes back), and forgotten on
// send and when Settings closes (forget_secrets).
//
// Pure: no I/O, no clock of its own (`now` is passed), no lock. One caller at
// a time (the add-on's worker and its ticker share it under its own mutex).
// Portable (D9).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mv::phone::card {

// The word for this computer: "this Mac", "this PC", "this computer".
enum class computer : std::uint8_t { mac, pc, other };

// This computer -> Drives (docs/plans/self-host-drives.md §6): what the card
// holds while the person picks drives and a way to use them.
struct drives_state {
  bool open = false;
  std::vector<std::string> picked;  // drive ids, in the listing's order
  std::string choice;               // "" none; "separate", "mirror", ...
  std::string label;                // the combined drive's name
  std::vector<std::pair<std::string, std::string>> typed;  // drive id -> the name typed
  std::string replacing;            // a set's id
  std::string replace_with, replace_typed;
  std::string asked;                // "combine" | "replace" until its outcome shows
};

// This computer -> Users (docs/plans/self-host-users-smb.md §8).
struct users_state {
  bool adding = false;
  std::string name, role = "view";
  std::string password, password_again, unlock_with;  // secure: never in a tree
  bool refreshed = false;  // users_refresh asked since the card opened
};

// The rules the card applies before it sends (the program checks them again).
// A user name the service takes: a-z first, then a-z 0-9 . _ -, at most 20, not reserved.
[[nodiscard]] bool valid_user_name(std::string_view name);
// The combined drive's name: 1 to 27 ASCII letters, digits, spaces, '-' or '_',
// no space at either end.
[[nodiscard]] bool good_drive_label(std::string_view label);
// One typed name per drive, in order, each equal to that drive's name once trimmed.
[[nodiscard]] bool drive_names_match(const std::vector<std::string>& typed, const std::vector<std::string>& names);

class screen {
 public:
  explicit screen(computer c = computer::other) : computer_(c) {}

  // The whole tree for the link's `state_json`, at unix time `now`.
  [[nodiscard]] std::string build(std::string_view state_json, std::int64_t now);
  // A click (kind 1), change (2) or submit (3) on `node`, as the host checked it.
  // The requests to queue on the link, in order; the tree may have changed too.
  [[nodiscard]] std::vector<std::string> on_event(std::string_view node, std::uint32_t kind,
                                                  std::string_view value_json, std::int64_t now);
  // What was typed and never sent, and the parts it was typed in (Settings closed).
  void forget_secrets();
  // Requests the last build wants sent (a section shown for the first time asks
  // for its list); taken once.
  [[nodiscard]] std::vector<std::string> take_queued();

  // The pairing code's text from the last build ("" when none shows): the QR's.
  [[nodiscard]] const std::string& qr_text() const noexcept { return qr_; }
  // The surface the QR is drawn into (the tree's `surface` id).
  static constexpr const char* kQrSurface = "qr";
  // Whether the last build had something that changes each second (the code's countdown).
  [[nodiscard]] bool ticking() const noexcept { return !qr_.empty(); }

  // A phone's ("ph", its key) or a shared folder's ("fo", its id) row id prefix.
  [[nodiscard]] static std::string row_id(const char* prefix, const std::string& of);

  // ---- words, for the tests -----------------------------------------------------
  [[nodiscard]] static std::string share_label(std::string_view path);
  [[nodiscard]] static std::string gigabytes(std::int64_t bytes);
  // "500" or "1,5" (GB) to bytes; -1 when it is not a number.
  [[nodiscard]] static std::int64_t bytes(std::string_view gigabytes);
  [[nodiscard]] static std::string duration_text(std::int64_t seconds);
  // "in 3 minutes", "2 hours ago".
  [[nodiscard]] static std::string relative(std::int64_t then, std::int64_t now);

 private:
  struct host_folder {
    std::string path;
    bool backup = false;
  };

  computer computer_;
  // Typed, not sent. Plain fields are also in the tree (so a new tree keeps
  // them); the secure ones never are.
  std::string local_, public_, new_label_, new_path_, new_limit_, new_reserve_ = "1";
  bool new_backup_ = false;
  bool adding_folder_ = false;  // the Add a folder form is open (Add folder… / Add backup folder…)
  std::string editing_;  // a backup folder's id whose sizes are being changed
  std::string edit_limit_, edit_reserve_;
  bool confirm_restart_ = false, confirm_hand_over_ = false;
  std::string passphrase_, passphrase_again_, unlock_with_;  // secure
  bool changing_ = false, unlock_wanted_ = false;
  std::int64_t code_for_ = 0;
  bool relay_editing_ = false;
  std::string relay_address_, relay_key_;  // relay_key_ secure
  bool setup_open_ = false;
  std::string setup_kind_ = "this";  // "this", "synology", "other"
  std::vector<host_folder> host_folders_;
  std::string duck_name_, duck_token_;  // duck_token_ secure
  bool duck_change_ = false;  // this computer's DuckDNS name being changed, after setup
  std::string duck_note_;     // why Save didn't send, drawn by the form, until the next event
  // The VPN's own domain (docs/plans/vpn-stealth.md §3; `host name domain`), being changed.
  std::string domain_name_;
  bool domain_change_ = false;
  bool server_here_ = false;  // the server is this computer (its folders are picked, not typed)
  std::string note_;  // this screen's own line ("Choose a folder…"), until the next event
  bool seeded_local_ = false, seeded_public_ = false;
  std::string last_error_;

  // From the last build, to map rows back to the link's ids.
  std::vector<std::string> phone_keys_, folder_ids_;
  std::vector<std::int64_t> phone_ends_;
  std::vector<bool> phone_limited_, phone_backup_, folder_backup_;
  std::vector<std::int64_t> folder_limit_, folder_reserve_;
  std::string asking_key_, relay_now_;
  bool claimed_ = false, busy_ = false, grants_ = false;
  std::string qr_;

  std::uint64_t secret_gen_ = 1;  // a secure field's id suffix (build)
  drives_state drives_;
  users_state users_;
  std::vector<std::string> queued_;
  // From the last build: the drives listing's ids and names, and the choices offered.
  std::vector<std::string> disk_ids_, disk_names_, set_ids_, set_names_, offered_, user_names_, user_roles_;
  std::vector<std::vector<std::string>> user_folders_;
  std::vector<std::vector<std::string>> set_members_;

  [[nodiscard]] std::vector<std::string> handle(std::string_view node, std::uint32_t kind, std::string_view value_json,
                                                std::int64_t now);
  [[nodiscard]] bool holds_secret() const noexcept;
  [[nodiscard]] const char* here() const noexcept;
  [[nodiscard]] const char* Here() const noexcept;
};

}  // namespace mv::phone::card
