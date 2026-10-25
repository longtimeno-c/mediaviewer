// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/update.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "core/json.h"

#ifndef MV_DRIFTWIRE_SITE_URL
#define MV_DRIFTWIRE_SITE_URL "https://longtimeno-c.github.io/mediaviewer/driftwire"
#endif

namespace mv::driftwire::cli {

namespace fs = std::filesystem;

namespace {

fs::path to_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string lower_slashed(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '\\') c = '/';
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

// An address that is safe to put in a shell command as it is: no quote, no
// space, nothing a shell reads.
bool url_chars_ok(std::string_view s) {
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '.' || c == '_' || c == '~' || c == ':' || c == '/' || c == '%';
    if (!ok) return false;
  }
  return !s.empty();
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// ---- pure ----------------------------------------------------------------------------------

std::optional<std::vector<std::uint32_t>> parse_version(std::string_view s) {
  std::vector<std::uint32_t> parts;
  std::size_t at = 0;
  while (true) {
    std::size_t end = at;
    std::uint64_t n = 0;
    while (end < s.size() && s[end] >= '0' && s[end] <= '9') {
      n = n * 10 + static_cast<std::uint64_t>(s[end] - '0');
      if (n > 0xFFFFFFFFu || end - at >= 9) return std::nullopt;
      ++end;
    }
    if (end == at) return std::nullopt;
    parts.push_back(static_cast<std::uint32_t>(n));
    if (parts.size() > 4) return std::nullopt;
    if (end == s.size()) break;
    if (s[end] != '.') return std::nullopt;
    at = end + 1;
  }
  return parts;
}

bool newer_version(std::string_view latest, std::string_view current) {
  const auto l = parse_version(latest);
  const auto c = parse_version(current);
  if (!l || !c) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint32_t a = i < l->size() ? (*l)[i] : 0;
    const std::uint32_t b = i < c->size() ? (*c)[i] : 0;
    if (a != b) return a > b;
  }
  return false;
}

std::optional<std::string> parse_latest(std::string_view text) {
  const auto v = json::parse(text, 4);
  if (!v) return std::nullopt;
  const std::string* version = v->str("version");
  if (!version || !parse_version(*version)) return std::nullopt;
  return *version;
}

install_kind classify_install(const install_facts& f) {
  if (f.exe.empty()) return install_kind::none;
  if (f.windows) {
    if (f.local_app_data.empty()) return install_kind::none;
    std::string want = lower_slashed(f.local_app_data);
    while (!want.empty() && want.back() == '/') want.pop_back();
    want += "/programs/driftwire/driftwire.exe";
    return lower_slashed(f.exe) == want ? install_kind::windows : install_kind::none;
  }
  if (f.mac) {
    // The formula's keg: <prefix>/Cellar/driftwire/<version>/bin/driftwire.
    return f.exe.find("/Cellar/driftwire/") != std::string::npos ? install_kind::homebrew : install_kind::none;
  }
  // The packages put it in /usr/bin (/bin is the same folder on a merged /usr).
  if (f.exe == "/usr/bin/driftwire" || f.exe == "/bin/driftwire") return install_kind::linux_sh;
  // The tarball: <prefix>/bin/driftwire beside <prefix>/share/driftwire/install-manifest.
  constexpr std::string_view tail = "/bin/driftwire";
  if (f.exe.size() > tail.size() && f.exe.compare(f.exe.size() - tail.size(), tail.size(), tail) == 0 && f.exists) {
    const std::string prefix = f.exe.substr(0, f.exe.size() - tail.size());
    if (f.exists(prefix + "/share/driftwire/install-manifest")) return install_kind::linux_sh;
  }
  return install_kind::none;
}

std::string update_command_shown(install_kind k, std::string_view site) {
  const std::string s(site);
  switch (k) {
    case install_kind::windows: return "irm " + s + "/install.ps1 | iex";
    case install_kind::homebrew: return "brew upgrade driftwire";
    case install_kind::linux_sh: return "curl -fsSL " + s + "/install.sh | sh";
    case install_kind::none: break;
  }
  return {};
}

std::string update_command_run(install_kind k, std::string_view site) {
  if (!url_chars_ok(site)) return {};
  const std::string s(site);
  switch (k) {
    case install_kind::windows:
      // The one-liner, as the page has it. A running driftwire.exe is moved
      // aside, not in the way (install.ps1).
      return "powershell.exe -NoProfile -Command \"irm '" + s + "/install.ps1' | iex\"";
    case install_kind::homebrew: return "brew upgrade driftwire";
    case install_kind::linux_sh:
      // install.sh again: from a file, not a pipe, so a download that fails
      // is a failure (POSIX sh has no pipefail), with curl or wget as it
      // takes either.
      return "t=$(mktemp) || exit 1; if command -v curl >/dev/null 2>&1; then curl -fsSL '" + s +
             "/install.sh' -o \"$t\"; else wget -qO \"$t\" '" + s + "/install.sh'; fi && sh \"$t\"; s=$?; rm -f \"$t\"; "
             "exit $s";
    case install_kind::none: break;
  }
  return {};
}

std::string site_url(const char* env_override) {
  if (env_override && *env_override) {
    std::string s(env_override);
    while (!s.empty() && s.back() == '/') s.pop_back();
    const bool https = starts_with(s, "https://") && s.size() > 8;
    const bool local = starts_with(s, "http://127.0.0.1") || starts_with(s, "http://localhost");
    if ((https || local) && url_chars_ok(s)) return s;
  }
  std::string s(MV_DRIFTWIRE_SITE_URL);
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

update_state parse_update_state(std::string_view text) {
  update_state s;
  const auto v = json::parse(text, 4);
  if (!v) return s;
  if (const auto b = v->boolean("check")) s.check = *b;
  if (const auto t = v->integer("checked")) s.checked = *t;
  if (const std::string* l = v->str("latest"); l && parse_version(*l)) s.latest = *l;
  return s;
}

std::string update_state_json(const update_state& s) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("check").boolean(s.check);
  w.key("checked").integer(s.checked).key("latest").string(s.latest).end_object();
  return w.take();
}

bool check_due(const update_state& s, std::int64_t now_s) {
  if (!s.check) return false;
  // A clock set back makes the last answer "in the future": ask again.
  return s.checked <= 0 || now_s - s.checked >= kCheckEverySeconds || now_s < s.checked;
}

// ---- the files -----------------------------------------------------------------------------

update_state load_update_state(const std::string& data_dir) {
  if (data_dir.empty()) return {};
  std::ifstream in(to_path(data_dir + "/" + kUpdateFile), std::ios::binary);
  if (!in) return {};
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.size() > 4096) return {};
  return parse_update_state(text);
}

bool save_update_state(const std::string& data_dir, const update_state& s) {
  if (data_dir.empty()) return false;
  const fs::path path = to_path(data_dir + "/" + kUpdateFile);
  fs::path fresh = path;
  fresh += ".new";
  {
    std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string text = update_state_json(s);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) return false;
  }
  std::error_code ec;
  fs::rename(fresh, path, ec);  // replaces, on Windows too
  if (ec) fs::remove(fresh, ec);
  return !ec;
}

install_kind this_install() {
  install_facts f;
  f.exe = self_path();
  f.local_app_data = local_app_data();
#if defined(_WIN32)
  f.windows = true;
#elif defined(__APPLE__)
  f.mac = true;
#endif
  f.exists = [](const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(to_path(p), ec);
  };
  return classify_install(f);
}

// ---- the thread ----------------------------------------------------------------------------

struct update_check::shared {
  std::mutex m;
  std::function<void()> wake;  // nobody once stopped
  std::string data_dir, version, site;
  fetch_fn fetch;
  bool enabled = false;
  bool fetching = false;
  std::string newer;
  bool taken = false;
};

update_check::~update_check() { stop(); }

void update_check::start(std::string data_dir, std::string version, std::string site, std::function<void()> wake,
                         fetch_fn fetch) {
  data_dir_ = data_dir;
  s_ = std::make_shared<shared>();
  s_->data_dir = std::move(data_dir);
  s_->version = std::move(version);
  s_->site = std::move(site);
  s_->wake = std::move(wake);
  s_->fetch = fetch ? std::move(fetch) : [](const std::string& url) { return https_get(url, 4096, kCheckTimeoutMs); };
  const update_state st = load_update_state(s_->data_dir);
  s_->enabled = st.check;
  if (!st.check) return;
  if (newer_version(st.latest, s_->version)) s_->newer = st.latest;
  if (check_due(st, unix_now())) launch(s_);
}

void update_check::launch(const std::shared_ptr<shared>& s) {
  {
    std::lock_guard lock(s->m);
    if (s->fetching) return;
    s->fetching = true;
  }
  // Detached: a slow network never holds the session's closing up; the
  // thread keeps `s` alive and calls nobody once stopped.
  std::thread([s] {
    const http_answer a = s->fetch(s->site + "/latest.json");
    const std::optional<std::string> latest = a.status == 200 ? parse_latest(a.body) : std::nullopt;
    bool keep = false;
    {
      std::lock_guard lock(s->m);
      s->fetching = false;
      keep = latest && s->enabled;  // no answer: asked again next time
    }
    if (!keep) return;
    // The file outside the lock (the session takes it only to read the slot).
    update_state st = load_update_state(s->data_dir);
    st.checked = unix_now();
    st.latest = *latest;
    {
      std::lock_guard lock(s->m);
      st.check = s->enabled;  // /updates off while this was out
    }
    (void)save_update_state(s->data_dir, st);
    std::lock_guard lock(s->m);
    if (s->enabled && newer_version(*latest, s->version) && !s->taken) {
      s->newer = *latest;
      // Under the lock: stop() waits for a wake under way, so the terminal
      // it wakes is still there.
      if (s->wake) s->wake();
    }
  }).detach();
}

std::string update_check::take() {
  if (!s_) return {};
  std::lock_guard lock(s_->m);
  if (s_->taken || s_->newer.empty() || !s_->enabled) return {};
  s_->taken = true;
  return s_->newer;
}

bool update_check::set_enabled(bool on) {
  if (!s_) return false;
  update_state st = load_update_state(s_->data_dir);
  st.check = on;
  bool due = false;
  {
    std::lock_guard lock(s_->m);
    s_->enabled = on;
    due = on && check_due(st, unix_now());
  }
  const bool saved = save_update_state(s_->data_dir, st);
  if (due) launch(s_);
  return saved;
}

bool update_check::enabled() const {
  if (!s_) return false;
  std::lock_guard lock(s_->m);
  return s_->enabled;
}

void update_check::stop() {
  if (!s_) return;
  std::lock_guard lock(s_->m);
  s_->wake = nullptr;
}

}  // namespace mv::driftwire::cli
