// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// drives::port on macOS: diskutil and AppleRAID (docs/plans/self-host-drives.md §2).
// The tools' property lists are read with CoreFoundation into json::value for
// the pure parsers in drives.cpp. Erasing goes through osascript's
// administrator prompt; the test build's own disk images are changed directly
// (the person who attached them may already).
#include "addons/driftwire/cli/drives.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <chrono>
#include <thread>

namespace mv::driftwire::cli::drives {

namespace {

constexpr const char* kDiskutil = "/usr/sbin/diskutil";
constexpr const char* kHdiutil = "/usr/bin/hdiutil";
constexpr const char* kOsascript = "/usr/bin/osascript";

std::string cf_string(CFStringRef s) {
  if (!s) return {};
  if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return fast;
  const CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(len), '\0');
  if (!CFStringGetCString(s, out.data(), len, kCFStringEncodingUTF8)) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

json::value to_json(CFTypeRef t, int depth = 0) {
  json::value v;
  if (!t || depth > 16) return v;
  const CFTypeID id = CFGetTypeID(t);
  if (id == CFStringGetTypeID()) {
    v.k = json::kind::string;
    v.s = cf_string(static_cast<CFStringRef>(t));
  } else if (id == CFBooleanGetTypeID()) {
    v.k = json::kind::boolean;
    v.b = CFBooleanGetValue(static_cast<CFBooleanRef>(t));
  } else if (id == CFNumberGetTypeID()) {
    v.k = json::kind::number;
    const auto n = static_cast<CFNumberRef>(t);
    if (CFNumberIsFloatType(n)) {
      CFNumberGetValue(n, kCFNumberDoubleType, &v.d);
    } else {
      long long x = 0;
      CFNumberGetValue(n, kCFNumberLongLongType, &x);
      v.is_integer = true;
      v.i = x;
      v.d = static_cast<double>(x);
    }
  } else if (id == CFArrayGetTypeID()) {
    v.k = json::kind::array;
    const auto a = static_cast<CFArrayRef>(t);
    for (CFIndex i = 0; i < CFArrayGetCount(a); ++i) v.a.push_back(to_json(CFArrayGetValueAtIndex(a, i), depth + 1));
  } else if (id == CFDictionaryGetTypeID()) {
    v.k = json::kind::object;
    const auto d = static_cast<CFDictionaryRef>(t);
    const CFIndex n = CFDictionaryGetCount(d);
    std::vector<const void*> keys(static_cast<std::size_t>(n)), vals(static_cast<std::size_t>(n));
    CFDictionaryGetKeysAndValues(d, keys.data(), vals.data());
    for (CFIndex i = 0; i < n; ++i) {
      if (CFGetTypeID(keys[static_cast<std::size_t>(i)]) != CFStringGetTypeID()) continue;
      v.o.emplace_back(cf_string(static_cast<CFStringRef>(keys[static_cast<std::size_t>(i)])),
                       to_json(vals[static_cast<std::size_t>(i)], depth + 1));
    }
  }
  return v;
}

std::optional<json::value> plist_of(const std::string& exe, std::vector<std::string> args) {
  int code = 0;
  const auto out = run_tool(exe, args, code);
  if (!out || code != 0) return std::nullopt;
  CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(out->data()), static_cast<CFIndex>(out->size()));
  if (!data) return std::nullopt;
  CFPropertyListRef pl = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
  CFRelease(data);
  if (!pl) return std::nullopt;
  json::value v = to_json(pl);
  CFRelease(pl);
  return v;
}

void fill_usage(volume_facts& v) {
  if (v.mount.empty()) return;
  struct statfs s{};
  if (::statfs(v.mount.c_str(), &s) == 0) {
    const std::uint64_t total = static_cast<std::uint64_t>(s.f_blocks) * s.f_bsize;
    const std::uint64_t free = static_cast<std::uint64_t>(s.f_bfree) * s.f_bsize;
    if (v.used == 0 && total > free) v.used = total - free;
    if (v.size == 0) v.size = total;
  }
  if (DIR* d = ::opendir(v.mount.c_str())) {
    while (const dirent* e = ::readdir(d)) {
      const std::string name = e->d_name;
      if (name.empty() || name[0] == '.') {
        if (name == ".mediaviewer") v.has_catalogue = true;
        continue;
      }
      if (e->d_type == DT_DIR) ++v.folders;
    }
    ::closedir(d);
  }
}

std::string mount_of(const std::string& bsd) {
  if (bsd.empty()) return {};
  const auto info = plist_of(kDiskutil, {"info", "-plist", bsd});
  if (!info) return {};
  const std::string* m = info->str("MountPoint");
  return m ? *m : std::string();
}

std::string last_line(const std::string& out) {
  std::string s = out;
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  const auto nl = s.rfind('\n');
  return nl == std::string::npos ? s : s.substr(nl + 1);
}

// One tool run, or several joined in one administrator prompt.
run_result run_steps(const std::vector<std::vector<std::string>>& steps, bool elevate) {
  run_result r;
  if (steps.empty()) {
    r.why = "nothing to do";
    return r;
  }
  int code = 0;
  std::optional<std::string> out;
  if (elevate) {
    std::string script;
    for (const auto& s : steps) {
      std::string one = mac_admin_script(s);
      // "do shell script \"A\" with administrator privileges": joined as A && B in one prompt.
      const std::string head = "do shell script \"", tail = "\" with administrator privileges";
      one = one.substr(head.size(), one.size() - head.size() - tail.size());
      script += (script.empty() ? "" : " && ") + one;
    }
    out = run_tool(kOsascript, std::vector<std::string>{"-e", "do shell script \"" + script + "\" with administrator privileges"}, code);
  } else {
    for (const auto& s : steps) {
      out = run_tool(kDiskutil, s, code);
      if (!out || code != 0) break;
    }
  }
  if (!out) {
    r.why = "diskutil could not be started";
  } else if (code != 0) {
    r.why = last_line(*out);
    if (r.why.find("-128") != std::string::npos || r.why.find("canceled") != std::string::npos) r.why = "the administrator prompt was cancelled; nothing was erased";
    if (r.why.empty()) r.why = "diskutil failed";
  } else {
    r.ok = true;
  }
  return r;
}

class mac_port final : public port {
 public:
  listing list() override {
    listing l;
    const auto all = plist_of(kDiskutil, {"list", "-plist"});
    if (!all) {
      l.error = "diskutil could not list the drives";
      return l;
    }
    std::vector<std::pair<std::string, std::string>> images;
    if (const auto hd = plist_of(kHdiutil, {"info", "-plist"})) images = parse_hdiutil_images(*hd);
    const json::value* wholes = all->find("WholeDisks");
    if (wholes && wholes->k == json::kind::array) {
      for (const auto& w : wholes->a) {
        if (w.k != json::kind::string) continue;
        const auto info = plist_of(kDiskutil, {"info", "-plist", w.s});
        if (!info) continue;
        std::string image;
        for (const auto& [dev, path] : images) {
          if (dev == w.s) image = path;
        }
        auto d = parse_mac_disk(*info, *all, image);
        if (!d) continue;
        for (auto& v : d->volumes) fill_usage(v);
        l.disks.push_back(std::move(*d));
      }
    }
    l.sets = raid_sets(l.error);
    unique_names(l.disks);
    return l;
  }

  listing sets_only() override {
    listing l;
    l.sets = raid_sets(l.error);
    // SMART for each member's whole disk: `diskutil info` asks the drive's
    // controller, it opens no volume.
    for (auto& s : l.sets) {
      for (const auto& m : s.members) {
        if (m.id.empty() || s.state != health::healthy) continue;
        std::string whole = m.id;
        if (const auto at = whole.find('s', 4); at != std::string::npos) whole.resize(at);
        const auto info = plist_of(kDiskutil, {"info", "-plist", whole});
        const std::string* smart = info ? info->str("SMARTStatus") : nullptr;
        if (smart && *smart == "Failing") s.state = health::failing;
      }
    }
    return l;
  }

  run_result create(const plan& p, bool elevate) override {
    run_result r;
    const auto args = mac_create_args(p);
    if (args.empty()) {
      r.why = "macOS cannot make that";
      return r;
    }
    // The facts again, just before: what is erased is what was confirmed.
    const listing now = list();
    for (const auto& d : p.disks) {
      const auto it = std::find_if(now.disks.begin(), now.disks.end(), [&](const disk& x) { return x.id == d.id; });
      if (it == now.disks.end() || !same_disk(d, *it)) {
        r.why = "a drive changed since it was confirmed (" + d.name + "); nothing was erased";
        return r;
      }
    }
    r = run_steps({args}, elevate);
    if (!r.ok) return r;
    r.mount = set_mount(p.label);
    return r;
  }

  run_result replace(const set_health& s, const disk& fresh, bool elevate) override {
    run_result r;
    const auto steps = mac_replace_steps(s, fresh);
    if (steps.empty()) {
      r.why = "only a mirror can take a new drive and rebuild; a stripe or a span has nothing to rebuild from";
      return r;
    }
    const listing now = list();
    const auto it = std::find_if(now.disks.begin(), now.disks.end(), [&](const disk& x) { return x.id == fresh.id; });
    if (it == now.disks.end() || !same_disk(fresh, *it)) {
      r.why = "the new drive changed since it was confirmed; nothing was erased";
      return r;
    }
    r = run_steps(steps, elevate);
    if (r.ok) r.mount = set_mount(s.name);
    return r;
  }

  void notify(const std::string& title, const std::string& body) override {
    const auto esc = [](const std::string& s) {
      std::string o;
      for (const char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
      }
      return o;
    };
    int code = 0;
    (void)run_tool(kOsascript, std::vector<std::string>{"-e", "display notification \"" + esc(body) + "\" with title \"" + esc(title) + "\""}, code);
  }

 private:
  std::vector<set_health> raid_sets(std::string& error) {
    const auto raid = plist_of(kDiskutil, {"appleRAID", "list", "-plist"});
    if (!raid) {
      error = "diskutil could not list the RAID sets";
      return {};
    }
    auto sets = parse_mac_raid(*raid);
    const json::value* raw = raid->find("AppleRAIDSets");
    for (std::size_t i = 0; raw && raw->k == json::kind::array && i < raw->a.size() && i < sets.size(); ++i) {
      const std::string* bsd = raw->a[i].str("BSD Name");
      if (bsd) sets[i].mount = mount_of(*bsd);
    }
    return sets;
  }

  std::string set_mount(const std::string& name) {
    for (int i = 0; i < 20; ++i) {
      std::string error;
      for (const auto& s : raid_sets(error)) {
        if (s.name == name && !s.mount.empty()) return s.mount;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return {};
  }
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<mac_port>(); }

}  // namespace mv::driftwire::cli::drives
