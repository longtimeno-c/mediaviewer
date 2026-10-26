// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Telling the person a newer driftwire is out, and `driftwire update`
// (docs/design/28 "Updates"). Each install channel already updates it (apt,
// dnf or zypper; brew; the PowerShell line again), but nothing told anyone
// to: this does, as Claude Code's command line does.
//
//   The check    once a day at most, on a thread of its own as the session
//                opens (it never holds the session up): a plain GET of
//                <site>/latest.json, {"version": "x.y.z"}, written by the
//                release that deploys the site (tools/package/driftwire_repo.py).
//                No cookies, no identifier, no query: it reveals the address
//                and the time, as the app's update check does (rule 6: nothing
//                about a file). The answer is kept in the data folder
//                (driftwire_update.json), so a run within the day asks nobody.
//   The notice   one line in the transcript when the kept or fetched version
//                is newer than this one: "driftwire 0.1.40 is available" and
//                "driftwire update" under it.
//   update       `driftwire update` runs this copy's own channel: brew upgrade
//                on macOS (install.sh again for its archive on a Mac
//                without Homebrew), the site's install.sh again on Linux (it upgrades
//                the package through apt, dnf or zypper, or the tarball), the
//                site's install.ps1 again on Windows (it moves a running copy
//                aside). A copy none of them installed (a build of one's own)
//                is never checked for and is told where installers are.
//   Turning it   off: /updates off in the session (kept in the same file), or
//                DRIFTWIRE_NO_UPDATE_CHECK set to anything. On by default,
//                the owner's call (2026-10-25, docs/design/12).
//
// The version string from the site is never trusted as text: only digits and
// dots, at most four parts, are a version; anything else is no answer.
//
// The port (update_net_win.cpp, update_net_mac.mm, update_net_linux.cpp):
// an HTTPS GET (WinHTTP; NSURLSession; libcurl opened at run time, so the
// Linux program still needs only glibc) and the program's own path.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

inline constexpr const char* kUpdateFile = "driftwire_update.json";
inline constexpr std::int64_t kCheckEverySeconds = 24 * 60 * 60;
inline constexpr int kCheckTimeoutMs = 10000;

// ---- pure ----------------------------------------------------------------------------------

// "1.2.3" -> {1, 2, 3}: one to four parts of digits, nothing else.
[[nodiscard]] std::optional<std::vector<std::uint32_t>> parse_version(std::string_view s);
// `latest` is a version and later than `current` (a missing part is 0).
[[nodiscard]] bool newer_version(std::string_view latest, std::string_view current);
// latest.json's version, if it is one.
[[nodiscard]] std::optional<std::string> parse_latest(std::string_view json);

// How this copy came to be here, from where it runs.
enum class install_kind : std::uint8_t {
  none,     // a build of one's own, or somewhere no installer puts it
  windows,  // %LOCALAPPDATA%\Programs\Driftwire\driftwire.exe (install.ps1)
  homebrew, // a Homebrew Cellar's (the tap's formula)
  install_sh, // install.sh's: Linux's /usr/bin (apt, dnf, zypper), or the archive's prefix (Linux, a Mac without brew)
};

struct install_facts {
  std::string exe;             // the program's own path, links resolved
  std::string local_app_data;  // Windows: %LOCALAPPDATA%
  bool windows = false;
  bool mac = false;
  // Linux: the tarball's record beside the program (<prefix>/share/driftwire/install-manifest).
  std::function<bool(const std::string& path)> exists;
};
[[nodiscard]] install_kind classify_install(const install_facts& f);

// What updates it, as the person would type it ("" for none), and as this
// program runs it (through the platform's shell).
[[nodiscard]] std::string update_command_shown(install_kind k, std::string_view site);
[[nodiscard]] std::string update_command_run(install_kind k, std::string_view site);

// The site: DRIFTWIRE_REPO_URL when it is https (or http on 127.0.0.1 /
// localhost, for tests), else the address built in. No trailing '/'.
[[nodiscard]] std::string site_url(const char* env_override);

// driftwire_update.json.
struct update_state {
  bool check = true;
  std::int64_t checked = 0;  // Unix seconds of the last answer
  std::string latest;        // what the site said then
};
[[nodiscard]] update_state parse_update_state(std::string_view json);
[[nodiscard]] std::string update_state_json(const update_state& s);
[[nodiscard]] bool check_due(const update_state& s, std::int64_t now_s);

// ---- the port ------------------------------------------------------------------------------

struct http_answer {
  int status = 0;     // 0: no answer
  std::string body;   // at most max_bytes
  std::string error;  // the transport's words when there was no answer
};
// GET `url` (https only, redirects to https only, no cookies, no credentials);
// blocks up to `timeout_ms`. With no transport here: status 0 and an error.
[[nodiscard]] http_answer https_get(const std::string& url, std::size_t max_bytes, int timeout_ms);
// The running program's path, links resolved ("" when unknown).
[[nodiscard]] std::string self_path();
// %LOCALAPPDATA% on Windows; "" elsewhere.
[[nodiscard]] std::string local_app_data();

// ---- the files and the thread --------------------------------------------------------------

[[nodiscard]] update_state load_update_state(const std::string& data_dir);
bool save_update_state(const std::string& data_dir, const update_state& s);

// This copy's install kind (classify_install with the port's facts).
[[nodiscard]] install_kind this_install();

// The session's check. start() returns at once: a version already known to
// be newer is there for take() before it returns; a fetch, when one is due,
// runs on a detached thread and calls `wake` when it has answered (the
// thread keeps what it needs; stop() makes it call nobody).
class update_check {
 public:
  update_check() = default;
  ~update_check();
  update_check(const update_check&) = delete;
  update_check& operator=(const update_check&) = delete;

  // `fetch` defaults to https_get (tests replace it).
  using fetch_fn = std::function<http_answer(const std::string& url)>;
  void start(std::string data_dir, std::string version, std::string site, std::function<void()> wake,
             fetch_fn fetch = {});
  // The newer version, once: "" when there is none (yet).
  [[nodiscard]] std::string take();
  // Turned on or off from the session (kept in the file).
  bool set_enabled(bool on);
  [[nodiscard]] bool enabled() const;
  [[nodiscard]] const std::string& data_dir() const { return data_dir_; }
  void stop();

 private:
  struct shared;
  static void launch(const std::shared_ptr<shared>& s);
  std::shared_ptr<shared> s_;
  std::string data_dir_;
};

}  // namespace mv::driftwire::cli
