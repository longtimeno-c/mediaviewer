// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/drives.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

#include "io/child_process.h"

namespace mv::driftwire::cli::drives {

os_kind this_os() noexcept {
#if defined(_WIN32)
  return os_kind::windows;
#elif defined(__APPLE__)
  return os_kind::mac;
#else
  return os_kind::linux_os;
#endif
}

// ---- the choices --------------------------------------------------------------------------

const char* choice_word(choice c) noexcept {
  switch (c) {
    case choice::separate: return "separate";
    case choice::mirror: return "mirror";
    case choice::span: return "span";
    case choice::stripe: return "stripe";
    case choice::parity: return "parity";
  }
  return "separate";
}

std::optional<choice> parse_choice(std::string_view w) noexcept {
  for (const choice c : kAllChoices) {
    if (w == choice_word(c)) return c;
  }
  if (w == "raid1") return choice::mirror;
  if (w == "jbod" || w == "concat") return choice::span;
  if (w == "raid0") return choice::stripe;
  if (w == "raid5") return choice::parity;
  return std::nullopt;
}

const char* not_a_backup_text() noexcept {
  return "RAID is not a backup. Deleting a file deletes it on every drive, and a mistake, a virus or a "
         "fire takes them all at once. Keep one copy somewhere else: a phone's backup folder, a second NAS, "
         "or a drive kept away from this computer.";
}

std::string size_words(std::uint64_t bytes) {
  struct unit {
    std::uint64_t size;
    const char* name;
  };
  static constexpr unit units[] = {{1'000'000'000'000ull, "TB"}, {1'000'000'000ull, "GB"}, {1'000'000ull, "MB"},
                                   {1'000ull, "KB"}};
  for (const auto& u : units) {
    if (bytes >= u.size) {
      const double v = static_cast<double>(bytes) / static_cast<double>(u.size);
      char buf[32];
      if (v < 10.0) {
        std::snprintf(buf, sizeof buf, "%.1f %s", v, u.name);
      } else {
        std::snprintf(buf, sizeof buf, "%.0f %s", v, u.name);
      }
      std::string s = buf;
      if (s.size() > 5 && s.compare(s.size() - 5, 2, ".0") == 0) s.erase(s.size() - 5, 2);  // "2.0 TB" -> "2 TB"
      return s;
    }
  }
  return std::to_string(bytes) + " bytes";
}

std::uint64_t space_of(choice c, std::span<const std::uint64_t> sizes) noexcept {
  if (sizes.empty()) return 0;
  std::uint64_t sum = 0, least = std::numeric_limits<std::uint64_t>::max();
  for (const auto s : sizes) {
    sum += s;
    least = std::min(least, s);
  }
  const std::uint64_t n = sizes.size();
  switch (c) {
    case choice::separate: return sum;
    case choice::mirror: return n >= 2 ? least : 0;
    case choice::span: return n >= 2 ? sum : 0;
    case choice::stripe: return n >= 2 ? least * n : 0;
    case choice::parity: return n >= 3 ? least * (n - 1) : 0;
  }
  return 0;
}

std::vector<option> options_for(std::span<const std::uint64_t> sizes, os_kind os, bool pool_ok) {
  std::vector<option> out;
  const std::size_t n = sizes.size();
  for (const choice c : kAllChoices) {
    option o;
    o.c = c;
    o.space = space_of(c, sizes);
    const std::string space = size_words(o.space);
    switch (c) {
      case choice::separate:
        o.title = "Separate";
        o.space_words = space + " in all, each drive its own folder. Nothing is erased.";
        o.dead_words = "Only the files on that drive are lost.";
        o.speed_words = "Each drive's own speed.";
        break;
      case choice::mirror:
        o.title = "Mirror (RAID 1)";
        o.recommended = true;
        o.space_words = space + ", the size of the smallest drive.";
        o.dead_words = "Nothing is lost. Replace the dead drive and the mirror rebuilds itself.";
        o.speed_words = "Reads a little faster; writes like one drive.";
        break;
      case choice::span:
        o.title = "Span (JBOD)";
        o.space_words = space + ", all the drives added together.";
        o.dead_words = "The files on that drive are lost, and often more.";
        o.speed_words = "Like one drive.";
        break;
      case choice::stripe:
        o.title = "Stripe (RAID 0)";
        o.warning = true;
        o.space_words = space + ", all the drives added together.";
        o.dead_words = "Everything is lost, on every drive. Not for a photo library.";
        o.speed_words = "The fastest.";
        break;
      case choice::parity:
        o.title = "Parity (RAID 5)";
        o.space_words = space + ", all but one drive's worth.";
        o.dead_words = "Nothing is lost if only one dies; if a second dies before it rebuilds, everything is.";
        o.speed_words = "Reads fast; slow to write.";
        break;
    }
    o.offered = true;
    if (c != choice::separate) {
      if (c == choice::parity && os == os_kind::mac) {
        o.why_not = "macOS can't make this (AppleRAID has no parity). Use Mirror, or combine the drives on a Windows PC.";
      } else if (os == os_kind::linux_os) {
        o.why_not = "Combining drives on Linux (mdadm) is not built yet.";
      } else if (!pool_ok) {
        o.why_not = "Storage Spaces is not available on this PC, so drives cannot be combined here.";
      } else if (c == choice::parity && n < 3) {
        o.why_not = "Needs 3 or more drives.";
      } else if (n < 2) {
        o.why_not = "Needs 2 or more drives.";
      }
    } else if (n < 1) {
      o.why_not = "Pick a drive first.";
    }
    o.offered = o.why_not.empty();
    out.push_back(std::move(o));
  }
  return out;
}

// ---- the disks ----------------------------------------------------------------------------

std::uint64_t used_of(const disk& d) noexcept {
  std::uint64_t u = 0;
  for (const auto& v : d.volumes) u += v.used;
  return u;
}

std::size_t folders_of(const disk& d) noexcept {
  std::size_t f = 0;
  for (const auto& v : d.volumes) f += v.folders;
  return f;
}

void unique_names(std::vector<disk>& disks) {
  for (std::size_t i = 0; i < disks.size(); ++i) {
    disk& d = disks[i];
    d.name.clear();
    for (const auto& v : d.volumes) {
      if (!v.label.empty()) {
        d.name = v.label;
        break;
      }
    }
    if (d.name.empty()) d.name = d.model;
    if (d.name.empty()) d.name = "Drive " + std::to_string(i + 1);
  }
  std::vector<std::string> base;
  base.reserve(disks.size());
  for (const auto& d : disks) base.push_back(d.name);
  for (std::size_t i = 0; i < disks.size(); ++i) {
    if (std::count(base.begin(), base.end(), base[i]) > 1) disks[i].name = base[i] + " (" + disks[i].id + ")";
  }
}

std::string facts_line(const disk& d) {
  std::string s = d.name + " \xC2\xB7 " + size_words(d.size);
  if (!d.model.empty() && d.model != d.name) s += " \xC2\xB7 " + d.model;
  bool mounted = false;
  for (const auto& v : d.volumes) mounted = mounted || !v.mount.empty();
  if (!mounted) {
    s += d.volumes.empty() ? " \xC2\xB7 empty (no volume)" : " \xC2\xB7 not mounted (what is on it is unknown)";
  } else {
    const std::size_t f = folders_of(d);
    s += " \xC2\xB7 " + std::to_string(f) + (f == 1 ? " folder, " : " folders, ") + size_words(used_of(d)) + " used";
  }
  if (d.smart == "failing") s += " \xC2\xB7 reports it is failing";
  return s;
}

// ---- the guards ---------------------------------------------------------------------------

namespace {

std::string folded(std::string_view s, os_kind os) {
  std::string out(s);
  if (os != os_kind::linux_os) {
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (os == os_kind::windows) std::replace(out.begin(), out.end(), '/', '\\');
  return out;
}

bool is_sep(char c, os_kind os) { return c == '/' || (os == os_kind::windows && c == '\\'); }

std::string trimmed(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}

}  // namespace

bool path_within(std::string_view path, std::string_view folder, os_kind os) {
  if (path.empty() || folder.empty()) return false;
  std::string p = folded(path, os), f = folded(folder, os);
  while (f.size() > 1 && is_sep(f.back(), os)) f.pop_back();
  while (p.size() > 1 && is_sep(p.back(), os)) p.pop_back();
  if (p == f) return true;
  if (f.size() == 1 && is_sep(f[0], os)) return is_sep(p[0], os);  // "/": everything absolute
  if (os == os_kind::windows && f.size() == 2 && f[1] == ':') return p.size() > 2 && p.compare(0, 2, f) == 0 && is_sep(p[2], os);
  return p.size() > f.size() && p.compare(0, f.size(), f) == 0 && is_sep(p[f.size()], os);
}

std::string refusal_of(const disk& d, const guard& g) {
  if (d.system) return "it holds this computer's system";
  if (!g.images_under.empty() && (!d.image || !path_within(d.image_path, g.images_under, g.os))) {
    return "the test build touches only disk images made for its tests";
  }
  if (g.os == os_kind::mac && d.internal && !d.image) return "it is this Mac's internal drive";
  if (d.in_set) return "it is already part of a combined set or a storage pool";
  if (d.read_only) return "it is read-only";
  if (!d.online) return "Windows has it offline";
  for (const auto& v : d.volumes) {
    if (v.mount.empty()) continue;
    if (v.has_catalogue) return "it holds a MediaViewer catalogue (a .mediaviewer folder)";
    for (const auto& p : g.protected_paths) {
      if (path_within(p, v.mount, g.os)) return "it holds MediaViewer's library or data";
    }
    for (const auto& p : g.shared_paths) {
      if (path_within(p, v.mount, g.os)) {
        return "it holds a folder this server shares, which may be its only copy: copy it to another drive "
               "and take it back with `driftwire host unshare LABEL` first";
      }
    }
  }
  return {};
}

// ---- the plan -----------------------------------------------------------------------------

bool good_label(std::string_view label) noexcept {
  if (label.empty() || label.size() > 27 || label.front() == ' ' || label.back() == ' ') return false;
  for (const char c : label) {
    const auto u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == ' ' || c == '-' || c == '_') || u >= 0x80) return false;
  }
  return true;
}

plan make_plan(choice c, std::string_view label, std::span<const std::string> ids, std::span<const disk> all,
               const guard& g, os_kind os, bool pool_ok) {
  plan p;
  p.c = c;
  p.label = std::string(label);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(i), ids[i]) != ids.begin() + static_cast<std::ptrdiff_t>(i)) {
      p.error = "a drive is named twice (" + ids[i] + ")";
      return p;
    }
    const auto it = std::find_if(all.begin(), all.end(), [&](const disk& d) { return d.id == ids[i]; });
    if (it == all.end()) {
      p.error = "there is no drive " + ids[i] + " (driftwire host drives list shows them)";
      return p;
    }
    p.disks.push_back(*it);
  }
  std::vector<std::uint64_t> sizes;
  for (const auto& d : p.disks) sizes.push_back(d.size);
  const auto opts = options_for(sizes, os, pool_ok);
  const option& o = opts[static_cast<std::size_t>(c)];
  if (!o.offered) {
    p.error = o.title + ": " + o.why_not;
    return p;
  }
  p.space = o.space;
  if (c == choice::separate) {
    for (const auto& d : p.disks) {
      const bool any = std::any_of(d.volumes.begin(), d.volumes.end(), [](const volume_facts& v) { return !v.mount.empty(); });
      if (!any) p.refusals.push_back(d.name + ": it has no mounted volume to share");
    }
    return p;  // nothing is erased: the erase guards do not apply
  }
  if (!good_label(label)) {
    p.error = "the combined drive's name (--label) is 1 to 27 letters, digits, spaces, '-' or '_'";
    return p;
  }
  for (const auto& d : p.disks) {
    if (const std::string why = refusal_of(d, g); !why.empty()) p.refusals.push_back(d.name + ": " + why);
  }
  return p;
}

bool confirmed(const plan& p, std::span<const std::string> typed) {
  if (p.c == choice::separate) return true;
  if (typed.size() != p.disks.size()) return false;
  for (std::size_t i = 0; i < typed.size(); ++i) {
    if (trimmed(typed[i]) != p.disks[i].name || p.disks[i].name.empty()) return false;
  }
  return true;
}

bool same_disk(const disk& a, const disk& b) noexcept {
  return a.id == b.id && a.identity == b.identity && a.size == b.size && a.model == b.model &&
         a.image == b.image && a.image_path == b.image_path;
}

// ---- health -------------------------------------------------------------------------------

const char* health_word(health h) noexcept {
  switch (h) {
    case health::healthy: return "healthy";
    case health::rebuilding: return "rebuilding";
    case health::degraded: return "degraded";
    case health::offline: return "offline";
    case health::failing: return "failing";
  }
  return "healthy";
}

std::optional<health> parse_health(std::string_view w) noexcept {
  for (const health h : {health::healthy, health::rebuilding, health::degraded, health::offline, health::failing}) {
    if (w == health_word(h)) return h;
  }
  return std::nullopt;
}

namespace {
int rank(health h) noexcept {
  switch (h) {
    case health::healthy: return 0;
    case health::rebuilding: return 1;
    case health::failing: return 2;
    case health::degraded: return 3;
    case health::offline: return 4;
  }
  return 0;
}
}  // namespace

health worst(std::span<const set_health> sets) noexcept {
  health w = health::healthy;
  for (const auto& s : sets) {
    if (rank(s.state) > rank(w)) w = s.state;
  }
  return w;
}

std::string health_line(const set_health& s) {
  std::string line = s.name + " (" + choice_word(s.kind) + "): ";
  switch (s.state) {
    case health::healthy:
      return line + "healthy";
    case health::rebuilding:
      return line + "rebuilding" + (s.percent >= 0 ? ", " + std::to_string(s.percent) + "% done" : std::string());
    case health::degraded:
      return line + "a drive is missing or failed; replace it: driftwire host drives replace \"" + s.name + "\"";
    case health::offline:
      return line + "offline: not all of its drives are there";
    case health::failing:
      return line + "a drive reports it is failing; replace it soon: driftwire host drives replace \"" + s.name + "\"";
  }
  return line;
}

std::string change_words(const set_health& now, health before) {
  if (rank(now.state) <= rank(before) || now.state == health::rebuilding) return {};
  switch (now.state) {
    case health::degraded:
      return now.kind == choice::mirror ? "Your server's mirror \"" + now.name + "\" lost a drive. Its files are safe until another fails: replace it soon."
                                        : "\"" + now.name + "\" lost a drive. Its files are safe until another fails: replace it soon.";
    case health::offline:
      return "\"" + now.name + "\" is offline: not all of its drives are there.";
    case health::failing:
      return "A drive in \"" + now.name + "\" reports it is failing. Replace it soon.";
    default:
      return {};
  }
}

// ---- the OS tools' JSON -------------------------------------------------------------------

namespace {

std::string str_of(const json::value& v, std::string_view key) {
  const std::string* s = v.str(key);
  return s ? *s : std::string();
}

std::uint64_t uint_of(const json::value& v, std::string_view key) {
  const json::value* x = v.find(key);
  if (!x || x->k != json::kind::number) return 0;
  if (x->is_integer) return x->i > 0 ? static_cast<std::uint64_t>(x->i) : 0;
  return x->d > 0 ? static_cast<std::uint64_t>(x->d) : 0;
}

bool bool_of(const json::value& v, std::string_view key) { return v.boolean(key).value_or(false); }

// PowerShell's ConvertTo-Json gives one object, not an array of one.
std::vector<const json::value*> items(const json::value* v) {
  std::vector<const json::value*> out;
  if (!v) return out;
  if (v->k == json::kind::array) {
    for (const auto& e : v->a) {
      if (e.k == json::kind::object) out.push_back(&e);
    }
  } else if (v->k == json::kind::object) {
    out.push_back(v);
  }
  return out;
}

int percent_in(std::string_view s) {
  const auto pct = s.find('%');
  if (pct == std::string_view::npos) return -1;
  std::size_t b = pct;
  while (b > 0 && std::isdigit(static_cast<unsigned char>(s[b - 1]))) --b;
  if (b == pct) return -1;
  return std::clamp(std::atoi(std::string(s.substr(b, pct - b)).c_str()), 0, 100);
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool has(std::string_view s, std::string_view w) { return s.find(w) != std::string_view::npos; }

void add_mac_volume(disk& d, const json::value& part) {
  const std::string content = str_of(part, "Content");
  if (content == "Apple_RAID" || content == "Apple_RAID_Offline" || content == "Apple_Boot_RAID") d.in_set = true;
  if (content == "EFI" || content == "Apple_Boot" || str_of(part, "VolumeName") == "EFI") return;
  if (content == "Apple_APFS" || content == "Apple_APFS_Container" || content == "Apple_APFS_Recovery") return;  // its volumes come from the container
  volume_facts v;
  v.mount = str_of(part, "MountPoint");
  v.label = str_of(part, "VolumeName");
  v.size = uint_of(part, "Size");
  v.used = uint_of(part, "CapacityInUse");
  if (v.mount == "/" || v.mount == "/System/Volumes/Data") d.system = true;
  d.volumes.push_back(std::move(v));
}

}  // namespace

std::optional<disk> parse_mac_disk(const json::value& info, const json::value& all_list, const std::string& image_path) {
  if (info.k != json::kind::object) return std::nullopt;
  disk d;
  d.id = str_of(info, "DeviceIdentifier");
  if (d.id.empty() || !bool_of(info, "WholeDisk")) return std::nullopt;
  d.model = str_of(info, "MediaName");
  d.size = uint_of(info, "TotalSize");
  if (d.size == 0) d.size = uint_of(info, "Size");
  d.internal = bool_of(info, "Internal");
  d.read_only = info.boolean("WritableMedia") == false;
  d.image = str_of(info, "BusProtocol") == "Disk Image" || str_of(info, "VirtualOrPhysical") == "Virtual" || !image_path.empty();
  d.image_path = image_path;
  d.in_set = bool_of(info, "RAIDMaster") || bool_of(info, "RAIDSlice");
  // A synthesized APFS container disk is not a drive.
  if (str_of(info, "VirtualOrPhysical") == "Virtual" && image_path.empty() && str_of(info, "BusProtocol") != "Disk Image") {
    return std::nullopt;
  }
  const std::string smart = str_of(info, "SMARTStatus");
  d.smart = smart == "Verified" ? "verified" : smart == "Failing" ? "failing" : "";
  std::string uuid = str_of(info, "DiskUUID");
  if (uuid.empty()) uuid = str_of(info, "MediaUUID");
  d.identity = uuid + "|" + str_of(info, "DeviceTreePath") + "|" + str_of(info, "IORegistryEntryName") + "|" +
               std::to_string(d.size) + "|" + image_path;
  // Its partitions and the APFS volumes stored on them.
  std::vector<std::string> parts;
  for (const json::value* e : items(all_list.find("AllDisksAndPartitions"))) {
    if (str_of(*e, "DeviceIdentifier") != d.id) continue;
    if (!str_of(*e, "MountPoint").empty() || !str_of(*e, "VolumeName").empty()) add_mac_volume(d, *e);  // a volume with no partition map
    for (const json::value* p : items(e->find("Partitions"))) {
      parts.push_back(str_of(*p, "DeviceIdentifier"));
      add_mac_volume(d, *p);
    }
  }
  for (const json::value* e : items(all_list.find("AllDisksAndPartitions"))) {
    bool ours = false;
    for (const json::value* s : items(e->find("APFSPhysicalStores"))) {
      ours = ours || std::find(parts.begin(), parts.end(), str_of(*s, "DeviceIdentifier")) != parts.end();
    }
    if (!ours) continue;
    for (const json::value* v : items(e->find("APFSVolumes"))) {
      volume_facts f;
      f.mount = str_of(*v, "MountPoint");
      f.label = str_of(*v, "VolumeName");
      f.size = uint_of(*v, "Size");
      f.used = uint_of(*v, "CapacityInUse");
      if (f.mount == "/" || f.mount == "/System/Volumes/Data") d.system = true;
      const json::value* roles = v->find("Roles");
      if (roles && roles->k == json::kind::array) {
        for (const auto& r : roles->a) {
          if (r.k == json::kind::string && (r.s == "System" || r.s == "Data" || r.s == "VM" || r.s == "Preboot")) d.system = true;
        }
      }
      d.volumes.push_back(std::move(f));
    }
  }
  return d;
}

std::vector<set_health> parse_mac_raid(const json::value& list) {
  std::vector<set_health> out;
  for (const json::value* e : items(list.find("AppleRAIDSets"))) {
    set_health s;
    s.id = str_of(*e, "AppleRAIDSetUUID");
    s.name = str_of(*e, "Name");
    const std::string level = str_of(*e, "Level");
    s.kind = level == "Stripe" ? choice::stripe : level == "Concat" ? choice::span : choice::mirror;
    s.size = uint_of(*e, "Size");
    std::size_t online = 0;
    bool rebuilding = false, missing = false;
    for (const json::value* m : items(e->find("Members"))) {
      member x;
      x.id = str_of(*m, "BSD Name");
      x.uuid = str_of(*m, "AppleRAIDMemberUUID");
      x.status = str_of(*m, "MemberStatus");
      x.rebuilding = starts_with(x.status, "Rebuilding");
      x.percent = x.rebuilding ? percent_in(x.status) : -1;
      x.missing = x.id.empty() || x.status.empty() || x.status == "Failed" || x.status == "Offline" ||
                  starts_with(x.status, "Missing");
      if (x.status == "Online") ++online;
      rebuilding = rebuilding || x.rebuilding;
      missing = missing || x.missing;
      if (x.rebuilding && x.percent >= 0) s.percent = x.percent;
      s.members.push_back(std::move(x));
    }
    const std::string status = str_of(*e, "Status");
    if (status == "Offline" || (s.kind != choice::mirror && missing)) {
      s.state = health::offline;
    } else if (rebuilding) {
      s.state = health::rebuilding;
    } else if (status == "Degraded" || missing || (s.kind == choice::mirror && online < 2)) {
      s.state = health::degraded;
    } else {
      s.state = health::healthy;
    }
    if (s.state == health::offline && s.kind == choice::mirror && online >= 1) s.state = health::degraded;  // one half still there
    s.mount = str_of(*e, "MountPoint");  // the port fills it from the set's BSD Name
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_hdiutil_images(const json::value& info) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const json::value* img : items(info.find("images"))) {
    const std::string path = str_of(*img, "image-path");
    for (const json::value* ent : items(img->find("system-entities"))) {
      std::string dev = str_of(*ent, "dev-entry");
      if (starts_with(dev, "/dev/")) dev = dev.substr(5);
      if (!dev.empty() && dev.find('s', 4) == std::string::npos) out.emplace_back(dev, path);  // whole disks only
    }
  }
  return out;
}

std::optional<windows_listing> parse_windows(const json::value& doc) {
  if (doc.k != json::kind::object || !doc.find("disks")) return std::nullopt;
  windows_listing w;
  w.pool_ok = bool_of(doc, "pool_ok");
  for (const json::value* e : items(doc.find("disks"))) {
    disk d;
    const auto num = e->integer("Number");
    if (!num || *num < 0) continue;
    d.id = std::to_string(*num);
    d.model = str_of(*e, "FriendlyName");
    if (d.model.empty()) d.model = str_of(*e, "Model");
    d.identity = str_of(*e, "UniqueId");
    d.size = uint_of(*e, "Size");
    d.system = bool_of(*e, "IsSystem") || bool_of(*e, "IsBoot");
    d.read_only = bool_of(*e, "IsReadOnly");
    d.online = !bool_of(*e, "IsOffline");
    d.in_set = bool_of(*e, "InPool");
    d.image_path = str_of(*e, "ImagePath");
    d.image = !d.image_path.empty() || str_of(*e, "BusType") == "File Backed Virtual";
    d.internal = str_of(*e, "BusType") != "USB";
    const std::string h = str_of(*e, "Health"), op = str_of(*e, "Operational");
    d.smart = (h == "Warning" || h == "Unhealthy" || op.find("Predictive") != std::string::npos) ? "failing"
              : h == "Healthy" ? "verified" : "";
    for (const json::value* v : items(e->find("Volumes"))) {
      volume_facts f;
      f.mount = str_of(*v, "Path");
      f.label = str_of(*v, "Label");
      f.size = uint_of(*v, "Size");
      const std::uint64_t free = uint_of(*v, "Free");
      f.used = f.size > free ? f.size - free : 0;
      f.folders = static_cast<std::size_t>(uint_of(*v, "Folders"));
      f.has_catalogue = bool_of(*v, "Catalogue");
      if (bool_of(*v, "System")) d.system = true;
      d.volumes.push_back(std::move(f));
    }
    w.disks.push_back(std::move(d));
  }
  for (const json::value* e : items(doc.find("sets"))) {
    set_health s;
    s.id = str_of(*e, "UniqueId");
    s.name = str_of(*e, "FriendlyName");
    s.size = uint_of(*e, "Size");
    s.mount = str_of(*e, "Mount");
    const std::string res = str_of(*e, "Resiliency");
    const auto cols = e->integer("Columns").value_or(1);
    std::vector<const json::value*> ms = items(e->find("Members"));
    s.kind = res == "Mirror" ? choice::mirror : res == "Parity" ? choice::parity
             : (cols <= 1 && ms.size() > 1) ? choice::span : choice::stripe;
    const std::string h = str_of(*e, "Health"), op = str_of(*e, "Operational");
    s.percent = static_cast<int>(e->integer("Percent").value_or(-1));
    bool failing = false, missing = false;
    for (const json::value* m : ms) {
      member x;
      const auto n = m->integer("Number");
      x.id = n && *n >= 0 ? std::to_string(*n) : std::string();
      x.uuid = str_of(*m, "UniqueId");
      const std::string mh = str_of(*m, "Health"), mo = str_of(*m, "Operational");
      x.status = mo.empty() ? mh : mo;
      x.missing = x.id.empty() || has(mo, "Lost Communication") || mh == "Unhealthy" || str_of(*m, "Usage") == "Retired";
      failing = failing || mh == "Warning" || mo.find("Predictive") != std::string::npos;
      missing = missing || x.missing;
      s.members.push_back(std::move(x));
    }
    // OperationalStatus is a list ("Degraded,Incomplete"): each word looked for.
    if (has(op, "Detached") || has(op, "No Redundancy") || (h == "Unhealthy" && s.kind != choice::mirror && s.kind != choice::parity)) {
      s.state = health::offline;
    } else if (has(op, "InService") || has(op, "In Service")) {
      s.state = health::rebuilding;
    } else if (has(op, "Degraded") || has(op, "Incomplete") || h == "Warning" || h == "Unhealthy" || missing) {
      s.state = health::degraded;
    } else if (failing) {
      s.state = health::failing;
    } else {
      s.state = health::healthy;
    }
    w.sets.push_back(std::move(s));
  }
  return w;
}

// ---- what each OS is asked ----------------------------------------------------------------

std::vector<std::string> mac_create_args(const plan& p) {
  const char* level = p.c == choice::mirror ? "mirror" : p.c == choice::stripe ? "stripe" : p.c == choice::span ? "concat" : nullptr;
  if (!level || !p.ok()) return {};
  std::vector<std::string> a = {"appleRAID", "create", level, p.label, "JHFS+"};
  for (const auto& d : p.disks) a.push_back(d.id);
  return a;
}

namespace {
std::string sh_quote(std::string_view s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}
}  // namespace

std::string mac_admin_script(std::span<const std::string> diskutil_args) {
  std::string cmd = "/usr/sbin/diskutil";
  for (const auto& a : diskutil_args) cmd += " " + sh_quote(a);
  std::string esc;
  for (const char c : cmd) {
    if (c == '\\' || c == '"') esc += '\\';
    esc += c;
  }
  return "do shell script \"" + esc + "\" with administrator privileges";
}

std::string ps_quote(std::string_view s) {
  std::string out = "'";
  for (const char c : s) {
    out += c;
    if (c == '\'') out += '\'';
  }
  return out + "'";
}

namespace {
// Each disk checked again in the elevated script, by its UniqueId and its system flags.
std::string ps_check(const disk& d) {
  return "$d = Get-Disk -Number " + d.id + "\n"
         "if ($d.UniqueId -ne " + ps_quote(d.identity) + ") { throw 'A drive changed since it was confirmed. Nothing was erased.' }\n"
         "if ($d.IsSystem -or $d.IsBoot) { throw 'That drive holds this computer''s system. Nothing was erased.' }\n";
}
std::string ps_clear(const disk& d) {
  return "$d = Get-Disk -Number " + d.id + "\n"
         "if ($d.IsOffline) { Set-Disk -Number " + d.id + " -IsOffline $false }\n"
         "if ($d.IsReadOnly) { Set-Disk -Number " + d.id + " -IsReadOnly $false }\n"
         "if ($d.PartitionStyle -ne 'RAW') { Clear-Disk -Number " + d.id + " -RemoveData -RemoveOEM -Confirm:$false }\n";
}
}  // namespace

std::string windows_create_script(const plan& p) {
  if (p.c == choice::separate || !p.ok() || p.disks.size() < 2) return {};
  std::string s = "$ErrorActionPreference = 'Stop'\n";
  for (const auto& d : p.disks) s += ps_check(d);
  for (const auto& d : p.disks) s += ps_clear(d);
  std::string ids;
  for (const auto& d : p.disks) ids += (ids.empty() ? "" : ",") + ps_quote(d.id);
  const std::string n = std::to_string(p.disks.size());
  s += "$pds = @(Get-PhysicalDisk -CanPool $true | Where-Object { @(" + ids + ") -contains $_.DeviceId })\n"
       "if ($pds.Count -ne " + n + ") { throw 'Windows cannot pool these drives (Storage Spaces says they are not ready).' }\n"
       "$ss = Get-StorageSubSystem | Where-Object { $_.FriendlyName -like 'Windows Storage*' } | Select-Object -First 1\n"
       "$pool = New-StoragePool -FriendlyName " + ps_quote("MediaViewer " + p.label) +
       " -StorageSubSystemUniqueId $ss.UniqueId -PhysicalDisks $pds\n";
  std::string vd = "$vd = New-VirtualDisk -StoragePoolUniqueId $pool.UniqueId -FriendlyName " + ps_quote(p.label) +
                   " -ProvisioningType Fixed -UseMaximumSize";
  switch (p.c) {
    case choice::mirror: vd += " -ResiliencySettingName Mirror -NumberOfDataCopies 2"; break;
    case choice::span: vd += " -ResiliencySettingName Simple -NumberOfColumns 1"; break;
    case choice::stripe: vd += " -ResiliencySettingName Simple -NumberOfColumns " + n; break;
    case choice::parity: vd += " -ResiliencySettingName Parity -PhysicalDiskRedundancy 1 -NumberOfColumns " + n; break;
    case choice::separate: break;
  }
  s += vd + "\n";
  s += "$disk = $vd | Get-Disk\n"
       "Initialize-Disk -Number $disk.Number -PartitionStyle GPT\n"
       "$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter\n"
       "Format-Volume -Partition $part -FileSystem NTFS -NewFileSystemLabel " + ps_quote(p.label) + " -Confirm:$false | Out-Null\n"
       "'MOUNT ' + $part.DriveLetter + ':\\'\n";
  return s;
}

std::vector<std::vector<std::string>> mac_replace_steps(const set_health& s, const disk& fresh) {
  std::vector<std::vector<std::string>> steps;
  if (s.kind != choice::mirror || s.id.empty()) return steps;
  for (const auto& m : s.members) {
    if (m.missing && !m.uuid.empty()) steps.push_back({"appleRAID", "remove", m.uuid, s.id});
  }
  steps.push_back({"appleRAID", "add", "member", fresh.id, s.id});
  return steps;
}

std::string windows_replace_script(const set_health& s, const disk& fresh) {
  if ((s.kind != choice::mirror && s.kind != choice::parity) || s.id.empty()) return {};
  std::string r = "$ErrorActionPreference = 'Stop'\n" + ps_check(fresh) + ps_clear(fresh);
  r += "$vd = Get-VirtualDisk -UniqueId " + ps_quote(s.id) + "\n"
       "$pool = $vd | Get-StoragePool\n"
       "$pd = @(Get-PhysicalDisk -CanPool $true | Where-Object { $_.DeviceId -eq " + ps_quote(fresh.id) + " })\n"
       "if ($pd.Count -ne 1) { throw 'Windows cannot add that drive to the pool.' }\n"
       "Add-PhysicalDisk -StoragePoolUniqueId $pool.UniqueId -PhysicalDisks $pd\n";
  for (const auto& m : s.members) {
    if (m.missing && !m.uuid.empty()) {
      r += "Get-PhysicalDisk -UniqueId " + ps_quote(m.uuid) + " | Set-PhysicalDisk -Usage Retired\n";
    }
  }
  r += "Repair-VirtualDisk -UniqueId " + ps_quote(s.id) + " -AsJob | Out-Null\n";
  // A retired drive leaves the pool once nothing is on it (the next `replace` or this one, when it is empty).
  r += "Get-PhysicalDisk -StoragePool $pool | Where-Object { $_.Usage -eq 'Retired' -and $_.AllocatedSize -le 268435456 } | "
       "ForEach-Object { Remove-PhysicalDisk -StoragePoolUniqueId $pool.UniqueId -PhysicalDisks $_ -Confirm:$false }\n"
       "'DONE'\n";
  return r;
}

// ---- running a tool -----------------------------------------------------------------------

std::optional<std::string> run_tool(const std::string& exe, std::span<const std::string> args, int& code) {
  io::child_process p;
  if (!p.start(exe, args, true)) {
    code = -1;
    return std::nullopt;
  }
  p.close_stdin();
  std::string out, line;
  while (p.read_line(line)) {
    if (out.size() < 32u * 1024 * 1024) out.append(line).push_back('\n');
  }
  code = p.wait();
  return out;
}

}  // namespace mv::driftwire::cli::drives
