// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/drive_copier.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire::cli::drive_copy {

namespace fs = std::filesystem;

namespace {

fs::path to_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// `root` + a '/'-separated relative path.
fs::path under(const std::string& root, std::string_view rel) {
  fs::path p = to_path(root);
  if (!rel.empty()) p /= to_path(rel).make_preferred();
  return p;
}

bool write_whole(const fs::path& path, const std::string& text) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f.flush()) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);  // replaces: a torn write leaves the old file whole
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

std::optional<std::string> read_whole(const fs::path& path, std::size_t max = 64u << 20) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::string text;
  char buf[65536];
  while (f.read(buf, sizeof buf) || f.gcount() > 0) {
    text.append(buf, static_cast<std::size_t>(f.gcount()));
    if (text.size() > max) return std::nullopt;
  }
  return text;
}

fs::path store_of(const std::string& mount) { return to_path(mount) / to_path(kStoreName); }

bool stopping(const std::atomic<bool>* stop) { return stop && stop->load(std::memory_order_relaxed); }

void walk(const std::string& root, const std::string& rel, bool top_skips, std::vector<entry>& out, bool& incomplete,
          const std::atomic<bool>* stop) {
  if (stopping(stop)) {
    incomplete = true;
    return;
  }
  const std::string dir = rel.empty() ? root : io::join_path(root, io::native_relative(rel));
  auto kids = io::list_children(dir, true);
  if (!kids) {
    incomplete = true;
    return;
  }
  std::sort(kids->begin(), kids->end(), [](const io::dir_child& a, const io::dir_child& b) { return a.name < b.name; });
  for (const auto& k : *kids) {
    if (rel.empty() && top_skips && skipped_at_top(k.name)) continue;
    std::string path = rel.empty() ? k.name : rel + "/" + k.name;
    if (k.is_directory) {
      out.push_back({path, entry_kind::folder, 0, 0});
      walk(root, path, top_skips, out, incomplete, stop);
    } else {
      out.push_back({std::move(path), entry_kind::file, k.size, k.mtime_unix});
    }
  }
}

bool save_bin(const std::string& mount, const bin& b) {
  std::error_code ec;
  fs::create_directories(store_of(mount) / "bin", ec);
  return write_whole(store_of(mount) / "bin.json", to_json(b));
}

void drop_items(const std::string& mount, bin& b, const std::vector<std::string>& ids) {
  if (ids.empty()) return;
  const std::set<std::string> gone(ids.begin(), ids.end());
  for (const auto& id : ids) {
    std::error_code ec;
    fs::remove_all(store_of(mount) / "bin" / to_path(id), ec);
  }
  std::erase_if(b.items, [&](const bin_item& it) { return gone.count(it.id) != 0; });
}

enum class copy_outcome : std::uint8_t { done, failed, full, stopped };

// Source → "<dest>.mvpart" → dest, the source's time kept. The source is opened for reading only.
copy_outcome copy_file(const fs::path& from, const fs::path& to, std::string_view rel, const pass_options& o) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::path part = to;
  part += std::string(kPartSuffix);
  std::ifstream in(from, std::ios::binary);
  if (!in) return copy_outcome::failed;  // open elsewhere for exclusive use, or gone: the next pass
  const auto when = fs::last_write_time(from, ec);
  if (ec) return copy_outcome::failed;
  copy_outcome result = copy_outcome::done;
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return copy_outcome::failed;
    std::vector<char> buf(1u << 20);
    std::uint64_t written = 0;
    while (result == copy_outcome::done) {
      if (stopping(o.stop)) {
        result = copy_outcome::stopped;
        break;
      }
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const auto n = in.gcount();
      if (n <= 0) {
        if (in.bad()) result = copy_outcome::failed;
        break;
      }
      if (!out.write(buf.data(), n)) {
        const auto sp = fs::space(to.parent_path(), ec);
        result = !ec && sp.available < static_cast<std::uintmax_t>(n) ? copy_outcome::full : copy_outcome::failed;
        break;
      }
      written += static_cast<std::uint64_t>(n);
      if (o.fail_at && o.fail_at(rel, written)) result = copy_outcome::failed;
    }
    if (result == copy_outcome::done && !out.flush()) result = copy_outcome::failed;
  }
  if (result != copy_outcome::done) {
    fs::remove(part, ec);
    return result;
  }
  fs::last_write_time(part, when, ec);
  if (!ec) fs::rename(part, to, ec);
  if (ec) {
    fs::remove(part, ec);
    return copy_outcome::failed;
  }
  return copy_outcome::done;
}

}  // namespace

std::string copy_folder_name(std::string_view label) { return "MediaViewer copy of " + std::string(label); }

bool skipped_at_top(std::string_view name) noexcept {
  static constexpr std::string_view kSkip[] = {
      "System Volume Information", "$RECYCLE.BIN", "$Recycle.Bin", "RECYCLER", ".Spotlight-V100", ".Trashes",
      ".fseventsd", ".TemporaryItems", ".DocumentRevisions-V100", ".mediaviewer-copy", ".mediaviewer-copy.json",
      "pagefile.sys", "hiberfil.sys", "swapfile.sys"};
  for (auto s : kSkip) {
    if (name == s) return true;
  }
  return name.rfind("MediaViewer copy of ", 0) == 0;  // another drive's copy kept here
}

std::string marker_json(const marker& m) {
  json::writer w;
  w.begin_object().key("version").integer(1).key("label").string(m.label).key("complete_at").integer(m.complete_at);
  w.end_object();
  return w.take();
}

std::optional<marker> marker_from_json(std::string_view text) {
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const auto* label = doc->str("label");
  if (!label) return std::nullopt;
  marker m;
  m.label = *label;
  m.complete_at = doc->integer("complete_at").value_or(0);
  return m;
}

bool has_marker(const std::string& mount) {
  std::error_code ec;
  return !mount.empty() && fs::is_regular_file(to_path(mount) / to_path(kMarkerName), ec);
}

bool write_marker(const std::string& mount, const marker& m) {
  return write_whole(to_path(mount) / to_path(kMarkerName), marker_json(m));
}

std::optional<marker> read_marker(const std::string& mount) {
  const auto text = read_whole(to_path(mount) / to_path(kMarkerName), 64 * 1024);
  return text ? marker_from_json(*text) : std::nullopt;
}

void remove_marker(const std::string& mount) {
  std::error_code ec;
  fs::remove(to_path(mount) / to_path(kMarkerName), ec);
}

std::vector<entry> list_tree(const std::string& root, bool top_skips, bool& incomplete, const std::atomic<bool>* stop) {
  std::vector<entry> out;
  incomplete = false;
  walk(root, {}, top_skips, out, incomplete, stop);
  return out;
}

bin read_bin(const std::string& copy_mount) {
  const auto text = read_whole(store_of(copy_mount) / "bin.json");
  if (!text) return {};
  auto b = bin_from_json(*text);
  return b ? std::move(*b) : bin{};
}

pass_result run_pass(const pass_options& o) {
  pass_result r;
  std::error_code ec;
  if (o.source.empty() || !fs::is_directory(to_path(o.source), ec)) {
    r.trouble = pass_trouble::source_missing;
    return r;
  }
  if (o.copy_mount.empty() || !fs::is_directory(to_path(o.copy_mount), ec)) {
    r.trouble = pass_trouble::copy_missing;
    return r;
  }
  const std::string copy_root = io::join_path(o.copy_mount, copy_folder_name(o.label));
  fs::create_directories(to_path(copy_root), ec);
  if (ec) {
    r.trouble = pass_trouble::copy_missing;
    return r;
  }

  bin b = read_bin(o.copy_mount);
  {
    const auto old = expired(b, o.keep, o.now);
    r.expired = old.size();
    drop_items(o.copy_mount, b, old);
    if (!old.empty()) (void)save_bin(o.copy_mount, b);
  }

  bool source_incomplete = false, copy_incomplete = false;
  r.listing = list_tree(o.source, true, source_incomplete, o.stop);
  const std::vector<entry> copy = list_tree(copy_root, false, copy_incomplete, o.stop);
  if (stopping(o.stop)) {
    r.stopped = true;
    r.bin_now = std::move(b);
    return r;
  }
  // An empty source beside a copy with files is a drive not there (a folder left where it
  // was mounted), not everything deleted: nothing goes to the bin for it.
  if (r.listing.empty() && !copy.empty()) {
    r.trouble = pass_trouble::source_missing;
    r.bin_now = std::move(b);
    return r;
  }

  plan_input in;
  in.source = r.listing;
  in.copy = copy;
  if (o.previous) {
    in.previous_source = *o.previous;
    in.has_previous = true;
  }
  in.now = o.now;
  in.settle_seconds = o.settle_seconds;
#if defined(__linux__)
  in.fold_case = false;
#endif
  const auto space = fs::space(to_path(o.copy_mount), ec);
  in.copy_free = ec ? 0 : static_cast<std::uint64_t>(space.available);
  plan p = make_plan(in);
  if (!p.fits) {
    // Room from the bin, oldest first, only past its share of the drive; never from the live copy.
    const auto ids = trim_for_room(b, ec ? 0 : static_cast<std::uint64_t>(space.capacity), in.copy_free,
                                   p.bytes_to_copy + (64u << 20));
    r.trimmed = ids.size();
    drop_items(o.copy_mount, b, ids);
    if (!ids.empty()) (void)save_bin(o.copy_mount, b);
    const auto again = fs::space(to_path(o.copy_mount), ec);
    in.copy_free = ec ? 0 : static_cast<std::uint64_t>(again.available);
    p = make_plan(in);
  }
  r.held = p.held;
  if (!p.fits) {
    r.trouble = pass_trouble::copy_full;
    r.needed = p.bytes_to_copy;
    r.free = in.copy_free;
    r.files_left = p.files_to_copy;
    r.bytes_left = p.bytes_to_copy;
    r.bin_now = std::move(b);
    return r;
  }

  std::uint64_t files_left = p.files_to_copy, bytes_left = p.bytes_to_copy;
  std::size_t unsaved = 0;
  const auto flush_bin = [&] {
    if (unsaved > 0) (void)save_bin(o.copy_mount, b);
    unsaved = 0;
  };
  const auto move_to_bin = [&](const action& a) {
    const std::string id = next_id(b, o.now);
    fs::create_directories(store_of(o.copy_mount) / "bin", ec);
    fs::rename(under(copy_root, a.path), store_of(o.copy_mount) / "bin" / to_path(id), ec);
    if (ec) return false;
    b.items.push_back({id, a.path, a.size, o.now, a.reason, a.folder});
    ++r.to_bin;
    if (++unsaved >= 200) flush_bin();
    return true;
  };

  bool skip_replace_copy = false;
  for (const action& a : p.actions) {
    if (stopping(o.stop)) {
      r.stopped = true;
      break;
    }
    switch (a.kind) {
      case action_kind::drop_partial:
        fs::remove(under(copy_root, a.path + std::string(kPartSuffix)), ec);
        break;
      case action_kind::rename_case:
        fs::rename(under(copy_root, a.from), under(copy_root, a.path), ec);
        break;
      case action_kind::to_bin:
        if (a.reason == bin_reason::deleted && source_incomplete) break;  // maybe only unlisted, not gone
        // A replaced file that can't go to the bin is not copied over: its old version stays.
        skip_replace_copy = !move_to_bin(a) && a.reason == bin_reason::replaced;
        break;
      case action_kind::copy: {
        if (skip_replace_copy) {
          skip_replace_copy = false;
          ++r.failed;
          break;
        }
        const auto out = copy_file(under(o.source, a.path), under(copy_root, a.path), a.path, o);
        if (out == copy_outcome::done) {
          ++r.copied;
          r.bytes_copied += a.size;
          --files_left;
          bytes_left -= std::min(bytes_left, a.size);
        } else if (out == copy_outcome::failed) {
          ++r.failed;
        } else if (out == copy_outcome::stopped) {
          r.stopped = true;
        } else {
          r.trouble = pass_trouble::copy_full;
          const auto sp = fs::space(to_path(o.copy_mount), ec);
          r.needed = bytes_left;
          r.free = ec ? 0 : static_cast<std::uint64_t>(sp.available);
        }
        if (o.progress) o.progress(files_left, bytes_left);
        break;
      }
      case action_kind::hold:
      case action_kind::skip_link:
        break;
    }
    if (r.stopped || r.trouble != pass_trouble::none) break;
  }
  flush_bin();
  r.files_left = files_left;
  r.bytes_left = bytes_left;
  if (r.complete()) {
    auto m = read_marker(o.copy_mount).value_or(marker{o.label, 0});
    m.label = o.label;
    m.complete_at = o.now;
    (void)write_marker(o.copy_mount, m);
  }
  r.bin_now = std::move(b);
  return r;
}

}  // namespace mv::driftwire::cli::drive_copy
