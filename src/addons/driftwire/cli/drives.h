// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host drives` (docs/plans/self-host-drives.md, issue #686 §3): two
// or more drives for the server, each shared as it is or erased and combined
// into one volume with the OS's own tools (no RAID code of ours). Erasing is
// the most dangerous thing the app does (docs/design/12, 2026-11-14), so every
// guard below is mandatory and pure, tested on every OS.
//
//   the choices   options_for(): Separate, Mirror, Span, Stripe, Parity, every
//                 one shown with the space it gives, what a dead drive costs
//                 and its speed; a row the OS cannot do says why.
//   the guards    refusal_of(): system, library/catalogue, a shared folder, a
//                 set member, read-only; with `images_under` (the test build)
//                 anything but a disk image made under that folder.
//   the plan      make_plan(): the drives, the space, the names to type.
//   health        set_health and health words for `host status`, the card, the
//                 phone (serve caps "storage").
//   the port      drives::port, one file per OS (D9): drives_mac.mm
//                 (diskutil/AppleRAID), drives_win.cpp (Storage Spaces through
//                 PowerShell), drives_linux.cpp (mdadm, later: says so).
//
// The disks' facts come from the OS tools as JSON (PowerShell's ConvertTo-Json,
// a property list converted on the Mac) and are read by the pure parsers here,
// so the Windows parser runs in the Mac's tests and the other way round.
// Nothing here logs a path, a volume's name or a serial (rule 6); `host status`
// prints a set's name, which the person chose.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace mv::driftwire::cli::drives {

enum class os_kind : std::uint8_t { mac, windows, linux_os };
[[nodiscard]] os_kind this_os() noexcept;

// ---- the choices --------------------------------------------------------------------------

enum class choice : std::uint8_t { separate, mirror, span, stripe, parity };
inline constexpr choice kAllChoices[] = {choice::separate, choice::mirror, choice::span, choice::stripe,
                                         choice::parity};
[[nodiscard]] const char* choice_word(choice c) noexcept;  // "mirror": the command line's and JSON's
[[nodiscard]] std::optional<choice> parse_choice(std::string_view word) noexcept;

struct option {
  choice c = choice::separate;
  std::string title;         // "Mirror (RAID 1)"
  bool offered = false;      // false: `why_not` says why (the row is still shown)
  std::string why_not;       // "macOS can't make this. …", "Needs 3 or more drives."
  bool recommended = false;  // Mirror: the one for photos
  bool warning = false;      // Stripe: everything is lost with one drive
  std::uint64_t space = 0;   // bytes the person gets (Separate: the drives' total, each apart)
  std::string space_words;   // "1.8 TB, the smallest drive"
  std::string dead_words;    // what one dead drive costs
  std::string speed_words;
};
// Every row, in kAllChoices' order, for drives of these sizes. `pool_ok`: the
// OS can combine at all (Windows: Storage Spaces reachable).
[[nodiscard]] std::vector<option> options_for(std::span<const std::uint64_t> sizes, os_kind os, bool pool_ok = true);
// "RAID is not a backup. …": always above the rows.
[[nodiscard]] const char* not_a_backup_text() noexcept;
// "1.8 TB", "500 GB", "64 MB" (decimal units, as drives are sold).
[[nodiscard]] std::string size_words(std::uint64_t bytes);
// A combined volume's space for these sizes (0: not possible with this many).
[[nodiscard]] std::uint64_t space_of(choice c, std::span<const std::uint64_t> sizes) noexcept;

// ---- the disks ----------------------------------------------------------------------------

struct volume_facts {
  std::string mount;        // where it is mounted ("" when not); Windows "E:\"
  std::string label;        // its name
  std::uint64_t size = 0;
  std::uint64_t used = 0;   // 0 when not mounted (unknown)
  std::size_t folders = 0;  // folders at its top, hidden ones left out
  bool has_catalogue = false;  // a .mediaviewer folder at its top
};

struct disk {
  std::string id;         // what the OS calls it: "disk4", Windows' disk number "2"
  std::string name;       // what the person types (unique_names() makes it so)
  std::string model;      // "Samsung T7", "WDC WD40EFRX"
  std::string identity;   // the OS's lasting id (Windows UniqueId; the Mac's media UUID or IORegistry path)
  std::uint64_t size = 0;
  bool internal = false;
  bool system = false;    // holds this computer's system (boot) volume
  bool read_only = false;
  bool in_set = false;    // already a member of a RAID set or a storage pool
  bool online = true;     // Windows: OperationalStatus Online
  bool image = false;     // a disk image (hdiutil, a VHD)
  std::string image_path; // the image's file, for the test guard ("" for a real disk)
  std::string smart;      // "verified", "failing", "" when the OS does not say
  std::vector<volume_facts> volumes;
  // The Mac: each partition's id and DiskUUID, checked again inside the
  // administrator prompt (a renumbered diskN is caught there, not only before it).
  std::vector<std::pair<std::string, std::string>> parts;
};

// The names people type, one per disk and none the same: a volume's label,
// else the model, else "Drive N"; two the same get " (id)".
void unique_names(std::vector<disk>& disks);
[[nodiscard]] std::uint64_t used_of(const disk& d) noexcept;
[[nodiscard]] std::size_t folders_of(const disk& d) noexcept;
// "Photos 2TB · 1.8 TB · Samsung T7 · 3 folders, 420 GB used": one line for a list.
[[nodiscard]] std::string facts_line(const disk& d);

// ---- the guards ---------------------------------------------------------------------------

struct guard {
  // Folders that must never be erased: driftwire's data folder (the host's
  // library), MediaViewer's own data folder, the app's catalogue folders.
  std::vector<std::string> protected_paths;
  // Folders the host shares (serve.json's roots): maybe the only copy.
  std::vector<std::string> shared_paths;
  // The test build: only disk images whose file is under this folder ("": any disk).
  std::string images_under;
  os_kind os = this_os();  // the Mac: an internal drive is refused too (it is the system's)
};
// "" when the disk may be erased, else why not, in words for the person.
[[nodiscard]] std::string refusal_of(const disk& d, const guard& g);
// Whether `path` is `folder` or inside it (case-folded on Windows and the Mac).
[[nodiscard]] bool path_within(std::string_view path, std::string_view folder, os_kind os);

// ---- the plan -----------------------------------------------------------------------------

struct plan {
  choice c = choice::separate;
  std::string label;            // the combined volume's name (and its shared label)
  std::vector<disk> disks;      // in the order given
  std::uint64_t space = 0;
  std::vector<std::string> refusals;  // "Photos 2TB: holds this computer's system": any one stops it
  std::string error;                  // a plan that cannot be (an unknown id, a choice not offered, a bad name)
  [[nodiscard]] bool ok() const { return error.empty() && refusals.empty(); }
};
// A combined volume's name: 1–27 letters, digits, spaces, '-' or '_', not
// starting with a space (AppleRAID's and NTFS's labels both take it, and it
// passes through a shell and PowerShell untouched).
[[nodiscard]] bool good_label(std::string_view label) noexcept;
// `sets`: the combined drives there are already (a label one of them has is refused).
struct set_health;
[[nodiscard]] plan make_plan(choice c, std::string_view label, std::span<const std::string> ids,
                             std::span<const disk> all, const guard& g, os_kind os, bool pool_ok = true,
                             std::span<const set_health> sets = {});
// The names typed, in the plan's order: each exactly its disk's name (spaces
// at the ends dropped). Separate needs none.
[[nodiscard]] bool confirmed(const plan& p, std::span<const std::string> typed);
// The facts that must not change between confirming and erasing.
[[nodiscard]] bool same_disk(const disk& confirmed, const disk& now) noexcept;

// ---- health -------------------------------------------------------------------------------

enum class health : std::uint8_t { healthy, rebuilding, degraded, offline, failing };
[[nodiscard]] const char* health_word(health h) noexcept;  // "healthy", … (JSON, state.json, serve caps)
[[nodiscard]] std::optional<health> parse_health(std::string_view w) noexcept;

struct member {
  std::string id;        // "disk12s1", a physical disk number; "" for one missing
  std::string uuid;      // the OS's id for the member (AppleRAID member UUID, UniqueId)
  std::string status;    // the OS's word, kept: "Online", "Failed", "Missing", "Rebuilding"
  bool missing = false;  // not there, or failed
  bool rebuilding = false;
  int percent = -1;      // rebuild progress when the OS gives it
};
struct set_health {
  std::string id;        // AppleRAID set UUID; Windows the virtual disk's UniqueId
  std::string name;      // the set's name
  choice kind = choice::mirror;
  health state = health::healthy;
  int percent = -1;      // rebuild progress (-1: unknown)
  std::string mount;     // its volume's mount point ("" when not mounted)
  std::string bsd;       // the Mac: the set's own disk ("disk14"; "" while offline)
  std::uint64_t size = 0;
  std::vector<member> members;
};
// The worst of the sets (healthy when there are none).
[[nodiscard]] health worst(std::span<const set_health> sets) noexcept;
// "Photos (mirror): healthy", "Photos (mirror): a drive is missing — replace it: driftwire host drives replace Photos".
[[nodiscard]] std::string health_line(const set_health& s);
// The words the notification says on a change from `before` to `now` ("": none).
[[nodiscard]] std::string change_words(const set_health& now, health before);

// ---- the OS tools' JSON, read (pure; every OS's tests) ----------------------------------

// macOS: `diskutil info -plist diskN` (as JSON) for a whole disk, with
// `diskutil list -plist` (all of it: the disk's partitions, and the APFS
// containers whose physical stores are on it) and `hdiutil info -plist`'s
// image for it ("" none). Used space and folder counts are left 0 (the port
// fills them from the mounted volumes).
[[nodiscard]] std::optional<disk> parse_mac_disk(const json::value& info, const json::value& all_list,
                                                 const std::string& image_path);
// `diskutil appleRAID list -plist` (as JSON).
[[nodiscard]] std::vector<set_health> parse_mac_raid(const json::value& list);
// `hdiutil info -plist` (as JSON): /dev/diskN -> image file.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_hdiutil_images(const json::value& info);
// Windows: the drives script's JSON ({"disks":[…],"sets":[…],"pool_ok":true}).
struct windows_listing {
  std::vector<disk> disks;
  std::vector<set_health> sets;
  bool pool_ok = false;
};
[[nodiscard]] std::optional<windows_listing> parse_windows(const json::value& doc);

// ---- what each OS is asked (pure, tested) ---------------------------------------------

// macOS: diskutil's arguments for the plan ("appleRAID", "create", "mirror",
// label, "JHFS+", ids...). Empty for Separate and Parity.
[[nodiscard]] std::vector<std::string> mac_create_args(const plan& p);
// The `do shell script` text osascript runs with administrator privileges.
[[nodiscard]] std::string mac_admin_script(std::span<const std::string> diskutil_args);
// The shell line run in one administrator prompt: first, for every disk, its
// size, model, internal flag and partitions' DiskUUIDs compared with what was
// confirmed (exit 3, nothing erased, on any difference), then each step.
[[nodiscard]] std::string mac_checked_shell(const std::vector<std::vector<std::string>>& steps, std::span<const disk> disks);
// That line as AppleScript: do shell script "…" with administrator privileges.
[[nodiscard]] std::string mac_admin_wrap(std::string_view shell);
// Windows: the PowerShell script that combines the plan's disks, checking each
// one's UniqueId and IsSystem/IsBoot first. Empty for Separate.
[[nodiscard]] std::string windows_create_script(const plan& p);
// Replacing a set's missing member with `fresh`.
[[nodiscard]] std::vector<std::vector<std::string>> mac_replace_steps(const set_health& s, const disk& fresh);
[[nodiscard]] std::string windows_replace_script(const set_health& s, const disk& fresh);
// A PowerShell single-quoted string ('it''s').
[[nodiscard]] std::string ps_quote(std::string_view s);

// ---- the port -----------------------------------------------------------------------------

struct listing {
  std::vector<disk> disks;      // every whole disk the OS has, the system's too (it is refused, and shown)
  std::vector<set_health> sets;
  bool pool_ok = true;          // the OS can combine (Windows: Storage Spaces reachable)
  std::string error;            // the tools could not be asked ("" when listed)
};

struct run_result {
  bool ok = false;
  std::string why;     // when not ok: one line for the person (the tool's own last line, never a path)
  std::string mount;   // the new volume's mount point, once there
};

class port {
 public:
  virtual ~port() = default;
  // Blocks (runs the OS tools): a worker or the command line only.
  [[nodiscard]] virtual listing list() = 0;
  // The sets and their health alone (the drives keeper, every few minutes): no
  // volume is opened, so a sleeping drive is not woken. Blocks.
  [[nodiscard]] virtual listing sets_only() { return list(); }
  // Erases the plan's disks and combines them. `elevate`: through the OS's
  // administrator prompt (the product always); false only for the test build's
  // own disk images, which the person who made them may change already.
  [[nodiscard]] virtual run_result create(const plan& p, bool elevate) = 0;
  [[nodiscard]] virtual run_result replace(const set_health& s, const disk& fresh, bool elevate) = 0;
  // A desktop notification (the drives keeper, when a set loses a drive).
  virtual void notify(const std::string& title, const std::string& body) = 0;
};
// This OS's (drives_mac.mm, drives_win.cpp, drives_linux.cpp).
[[nodiscard]] std::unique_ptr<port> os_port();

// Runs a program and returns what it printed (stdout and stderr), or nullopt
// when it could not be started; `code` its exit code. Blocks.
[[nodiscard]] std::optional<std::string> run_tool(const std::string& exe, std::span<const std::string> args, int& code);

}  // namespace mv::driftwire::cli::drives
