// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_copy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "addons/driftwire/cli/drive_copier.h"
#include "addons/driftwire/cli/drives.h"
#include "addons/driftwire/cli/volume.h"
#include "core/json.h"

namespace mv::driftwire::cli {

namespace fs = std::filesystem;
namespace dc = drive_copy;

namespace {

fs::path to_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string read_text(const std::string& path) {
  std::ifstream f(to_path(path), std::ios::binary);
  if (!f) return {};
  std::string text;
  char buf[4096];
  while (f.read(buf, sizeof buf) || f.gcount() > 0) {
    text.append(buf, static_cast<std::size_t>(f.gcount()));
    if (text.size() > (1u << 20)) return {};
  }
  return text;
}

bool write_text(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".part";
  {
    std::ofstream f(to_path(tmp), std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f.flush()) return false;
  }
  std::error_code ec;
  for (int i = 0; i < 40; ++i) {  // Windows: a reader may have it open for a moment
    fs::rename(to_path(tmp), to_path(path), ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  fs::remove(to_path(tmp), ec);
  return false;
}

std::string s_of(const json::value& v, const char* k) {
  const std::string* s = v.str(k);
  return s ? *s : std::string();
}

std::uint64_t u_of(const json::value& v, const char* k) {
  const auto n = v.integer(k);
  return n && *n > 0 ? static_cast<std::uint64_t>(*n) : 0;
}

std::string files_words(std::uint64_t n) { return std::to_string(n) + (n == 1 ? " file" : " files"); }

// Where a copy's volume is now (nullopt: not attached). POSIX: a volume's root
// is a mount point, so a folder left behind by an unplugged drive is not it.
std::optional<std::string> locate(const std::string& path, const std::string& volume, bool plain) {
  if (plain) {
    std::error_code ec;
    return fs::is_directory(to_path(path), ec) ? std::optional<std::string>(path) : std::nullopt;
  }
  serve::root_config r;
  r.path = path;
  r.volume = volume;
#if !defined(_WIN32)
  r.mount = path != "/";
#endif
  return volume_locate(r);
}

}  // namespace

// ---- copies.json --------------------------------------------------------------------------

std::string copies_json(std::span<const copy_entry> copies) {
  json::writer w;
  w.begin_object().key("version").integer(1).key("copies").begin_array();
  for (const auto& c : copies) {
    w.begin_object().key("label").string(c.label).key("to_name").string(c.to_name);
    w.key("from").string(c.from).key("from_volume").string(c.from_volume);
    w.key("to").string(c.to).key("to_volume").string(c.to_volume).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<std::vector<copy_entry>> copies_from_json(std::string_view text) {
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const json::value* a = doc->find("copies");
  if (!a || a->k != json::kind::array) return std::nullopt;
  std::vector<copy_entry> out;
  for (const auto& v : a->a) {
    if (v.k != json::kind::object) return std::nullopt;
    copy_entry c{s_of(v, "label"), s_of(v, "to_name"), s_of(v, "from"), s_of(v, "from_volume"), s_of(v, "to"),
                 s_of(v, "to_volume")};
    if (c.label.empty() || c.from.empty() || c.to.empty()) return std::nullopt;
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<copy_entry> read_copies(const std::string& host_folder) {
  const std::string text = read_text(host_folder + "/copies.json");
  if (text.empty()) return {};
  auto c = copies_from_json(text);
  return c ? std::move(*c) : std::vector<copy_entry>{};
}

bool write_copies(const std::string& host_folder, std::span<const copy_entry> copies) {
  return write_text(host_folder + "/copies.json", copies_json(copies));
}

// ---- how each stands ----------------------------------------------------------------------

const char* copy_state_word(copy_state s) noexcept {
  switch (s) {
    case copy_state::waiting: return "waiting";
    case copy_state::copying: return "copying";
    case copy_state::up_to_date: return "up_to_date";
    case copy_state::behind: return "behind";
    case copy_state::source_missing: return "source_missing";
    case copy_state::copy_missing: return "copy_missing";
    case copy_state::copy_full: return "copy_full";
  }
  return "waiting";
}

std::optional<copy_state> parse_copy_state(std::string_view w) noexcept {
  for (auto s : {copy_state::waiting, copy_state::copying, copy_state::up_to_date, copy_state::behind,
                 copy_state::source_missing, copy_state::copy_missing, copy_state::copy_full}) {
    if (w == copy_state_word(s)) return s;
  }
  return std::nullopt;
}

std::string copy_statuses_json(std::span<const copy_status> list) {
  json::writer w;
  w.begin_object().key("copies").begin_array();
  for (const auto& s : list) {
    w.begin_object().key("label").string(s.label).key("to_name").string(s.to_name).key("state").string(copy_state_word(s.state));
    w.key("files_left").integer(static_cast<std::int64_t>(s.files_left));
    w.key("bytes_left").integer(static_cast<std::int64_t>(s.bytes_left));
    w.key("failed").integer(static_cast<std::int64_t>(s.failed)).key("held").integer(static_cast<std::int64_t>(s.held));
    w.key("needed").integer(static_cast<std::int64_t>(s.needed)).key("free").integer(static_cast<std::int64_t>(s.free));
    w.key("complete_at").integer(s.complete_at);
    w.key("bin_items").integer(static_cast<std::int64_t>(s.bin_items));
    w.key("bin_bytes").integer(static_cast<std::int64_t>(s.bin_bytes)).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::vector<copy_status> copy_statuses_from_json(std::string_view text) {
  std::vector<copy_status> out;
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::object) return out;
  const json::value* a = doc->find("copies");
  if (!a || a->k != json::kind::array) return out;
  for (const auto& v : a->a) {
    if (v.k != json::kind::object || s_of(v, "label").empty()) continue;
    copy_status s;
    s.label = s_of(v, "label");
    s.to_name = s_of(v, "to_name");
    s.state = parse_copy_state(s_of(v, "state")).value_or(copy_state::waiting);
    s.files_left = u_of(v, "files_left");
    s.bytes_left = u_of(v, "bytes_left");
    s.failed = u_of(v, "failed");
    s.held = u_of(v, "held");
    s.needed = u_of(v, "needed");
    s.free = u_of(v, "free");
    s.complete_at = v.integer("complete_at").value_or(0);
    s.bin_items = u_of(v, "bin_items");
    s.bin_bytes = u_of(v, "bin_bytes");
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<copy_status> read_copy_statuses(const std::string& host_folder) {
  return copy_statuses_from_json(read_text(host_folder + "/copies-state.json"));
}

std::string copy_line(const copy_status& s, bool mac) {
  std::string out;
  switch (s.state) {
    case copy_state::waiting: out = "starts when the server runs"; break;
    case copy_state::copying:
      out = "copying, " + files_words(s.files_left) + " (" + drives::size_words(s.bytes_left) + ") left";
      break;
    case copy_state::up_to_date: out = "up to date"; break;
    case copy_state::behind:
      out = s.failed > 0 ? "behind: " + files_words(s.failed) + " couldn't be read (in use?), tried again soon"
                         : "behind: " + files_words(s.held) + " still being written, copied once they settle";
      break;
    case copy_state::source_missing: out = "\"" + s.label + "\" isn't connected; the copy keeps what it has"; break;
    case copy_state::copy_missing: out = "the copy drive isn't connected"; break;
    case copy_state::copy_full:
      out = "the copy drive is full: needs " + drives::size_words(s.needed) + ", has " + drives::size_words(s.free) +
            " free";
      break;
  }
  if (s.bin_items > 0) {
    out += std::string(mac ? ". Trash: " : ". Recycle Bin: ") + std::to_string(s.bin_items) +
           (s.bin_items == 1 ? " item, " : " items, ") + drives::size_words(s.bin_bytes);
  }
  return "Copy on \xE2\x80\x9C" + s.to_name + "\xE2\x80\x9D: " + out;
}

std::string copies_status_line(std::span<const copy_status> list) {
  std::string out;
  for (const auto& s : list) {
    out += (out.empty() ? "copy        " : "\n            ") + s.label + ": " + copy_line(s, false);
  }
  return out;
}

// ---- the keeper ---------------------------------------------------------------------------

namespace {

std::int64_t now_s() { return static_cast<std::int64_t>(std::time(nullptr)); }

class copy_keeper final : public host_keeper {
 public:
  explicit copy_keeper(copy_keeper_config c) : c_(std::move(c)) {}
  ~copy_keeper() override { stop(); }

  void start(const keeper_context& ctx) override {
    if (ctx.loopback_only && !c_.on_loopback) return;
    folder_ = ctx.folder;
    stop_.store(false);
    t_ = std::thread([this] { loop(); });
  }

  void stop() noexcept override {
    {
      std::lock_guard g(m_);
      stop_.store(true);
    }
    cv_.notify_all();
    if (t_.joinable()) t_.join();
  }

  void ports_changed(const keeper_context&) noexcept override {}

  std::string status_line() const override {
    std::lock_guard g(m_);
    return copies_status_line(status_);
  }

  std::vector<std::pair<std::string, std::int64_t>> state_numbers() const override {
    std::lock_guard g(m_);
    if (status_.empty()) return {};
    std::int64_t behind = 0;
    for (const auto& s : status_) {
      if (s.state != copy_state::up_to_date && s.state != copy_state::copying) ++behind;
    }
    return {{"copies", static_cast<std::int64_t>(status_.size())}, {"copies_behind", behind}};
  }

 private:
  void publish(const std::vector<copy_status>& s) {
    {
      std::lock_guard g(m_);
      status_ = s;
    }
    (void)write_text(folder_ + "/copies-state.json", copy_statuses_json(s));
  }

  void loop() {
    std::map<std::string, std::vector<dc::entry>> previous;  // per label: the source's last listing
    std::string config_text = "\x01";
    auto next_pass = std::chrono::steady_clock::now();
    while (!stop_.load()) {
      const std::string text = read_text(folder_ + "/copies.json");
      const bool changed = text != config_text;
      config_text = text;
      if (changed || std::chrono::steady_clock::now() >= next_pass) {
        const auto copies = read_copies(folder_);
        std::vector<copy_status> st;
        {
          std::lock_guard g(m_);
          for (const auto& c : copies) {
            const auto old = std::find_if(status_.begin(), status_.end(), [&](const copy_status& s) { return s.label == c.label; });
            copy_status fresh;
            fresh.label = c.label;
            st.push_back(old != status_.end() ? *old : fresh);
          }
        }
        bool soon = false;
        for (std::size_t i = 0; i < copies.size() && !stop_.load(); ++i) {
          soon |= pass(copies[i], st, i, previous);
        }
        std::erase_if(previous, [&](const auto& p) {
          return std::none_of(copies.begin(), copies.end(), [&](const copy_entry& c) { return c.label == p.first; });
        });
        if (!stop_.load()) publish(st);
        next_pass = std::chrono::steady_clock::now() + std::chrono::milliseconds(soon ? c_.soon_ms : c_.every_ms);
      }
      std::unique_lock lock(m_);
      cv_.wait_for(lock, std::chrono::milliseconds(c_.config_ms), [this] { return stop_.load(); });
    }
  }

  // One copy's pass; true when another should come soon (something held back, failed or missing).
  bool pass(const copy_entry& c, std::vector<copy_status>& st, std::size_t i,
            std::map<std::string, std::vector<dc::entry>>& previous) {
    copy_status& s = st[i];
    s.to_name = c.to_name;
    const auto from = locate(c.from, c.from_volume, c_.plain_folders);
    const auto to = locate(c.to, c.to_volume, c_.plain_folders);
    if (!to) {
      s.state = copy_state::copy_missing;
      return true;
    }
    if (!from) {
      s.state = copy_state::source_missing;
      return true;
    }
    dc::pass_options o;
    o.source = *from;
    o.copy_mount = *to;
    o.label = c.label;
    const auto p = previous.find(c.label);
    if (p != previous.end()) o.previous = &p->second;
    o.now = now_s();
    o.settle_seconds = c_.settle_seconds;
    o.stop = &stop_;
    // While a long first copy runs, how far it is shows every few seconds.
    auto shown = std::chrono::steady_clock::now();
    o.progress = [&](std::uint64_t files, std::uint64_t bytes) {
      if (std::chrono::steady_clock::now() - shown < std::chrono::seconds(5)) return;
      shown = std::chrono::steady_clock::now();
      s.state = copy_state::copying;
      s.files_left = files;
      s.bytes_left = bytes;
      publish(st);
    };
    dc::pass_result r = dc::run_pass(o);
    s.bin_items = r.bin_now.items.size();
    s.bin_bytes = 0;
    for (const auto& it : r.bin_now.items) s.bin_bytes += it.bytes;
    s.files_left = r.files_left;
    s.bytes_left = r.bytes_left;
    s.failed = r.failed;
    s.held = r.held;
    if (r.stopped) return false;
    previous[c.label] = std::move(r.listing);
    switch (r.trouble) {
      case dc::pass_trouble::source_missing: s.state = copy_state::source_missing; return true;
      case dc::pass_trouble::copy_missing: s.state = copy_state::copy_missing; return true;
      case dc::pass_trouble::copy_full:
        s.state = copy_state::copy_full;
        s.needed = r.needed;
        s.free = r.free;
        return false;
      case dc::pass_trouble::none: break;
    }
    if (r.complete()) {
      s.state = copy_state::up_to_date;
      s.complete_at = o.now;
      return false;
    }
    s.state = copy_state::behind;
    return true;
  }

  copy_keeper_config c_;
  std::string folder_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::thread t_;
  std::atomic<bool> stop_{false};
  std::vector<copy_status> status_;
};

}  // namespace

std::unique_ptr<host_keeper> make_copy_keeper(copy_keeper_config c) { return std::make_unique<copy_keeper>(std::move(c)); }

}  // namespace mv::driftwire::cli
