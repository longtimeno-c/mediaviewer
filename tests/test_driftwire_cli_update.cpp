// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire command line's update check and `driftwire update`
// (cli/update.h, docs/design/28 "Updates"): versions, latest.json, how a copy
// was installed, the commands that update it, the kept answer, and the
// check's thread with a stand-in for the network.
#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/update.h"

namespace cli = mv::driftwire::cli;
namespace fs = std::filesystem;

namespace {

bool has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

std::string utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// A data folder of its own, removed after.
struct scratch_dir {
  fs::path path;
  scratch_dir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("mv-dw-update-" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(path);
  }
  ~scratch_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  [[nodiscard]] std::string str() const { return utf8(path); }
};

// The wake the session would get, for the thread's answer.
struct waker {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
  std::function<void()> fn() {
    return [this] {
      std::lock_guard lock(m);
      ++count;
      cv.notify_all();
    };
  }
  bool wait_for(int n, int ms = 5000) {
    std::unique_lock lock(m);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return count >= n; });
  }
};

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr const char* kSite = "https://example.invalid/driftwire";

}  // namespace

TEST_CASE("driftwire cli update: versions are digits and dots, compared part by part", "[driftwire][cli]") {
  CHECK(cli::parse_version("0.1.38") == std::vector<std::uint32_t>{0, 1, 38});
  CHECK(cli::parse_version("2") == std::vector<std::uint32_t>{2});
  CHECK(cli::parse_version("1.2.3.4") == std::vector<std::uint32_t>{1, 2, 3, 4});
  for (const char* bad : {"", "1.", ".1", "1..2", "1.2.3.4.5", "v1.2", "1.2-beta", "1.2 ", "1.2\x1b[2J",
                          "99999999999", "1,2"}) {
    CAPTURE(bad);
    CHECK_FALSE(cli::parse_version(bad));
  }
  CHECK(cli::newer_version("0.1.40", "0.1.38"));
  CHECK(cli::newer_version("0.2", "0.1.38"));
  CHECK(cli::newer_version("1.0.0", "0.99.99"));
  CHECK(cli::newer_version("0.1.10", "0.1.9"));  // numbers, not text
  CHECK_FALSE(cli::newer_version("0.1.38", "0.1.38"));
  CHECK_FALSE(cli::newer_version("0.1.38.0", "0.1.38"));
  CHECK_FALSE(cli::newer_version("0.1.37", "0.1.38"));
  CHECK_FALSE(cli::newer_version("junk", "0.1.38"));
  CHECK_FALSE(cli::newer_version("0.1.40", ""));
}

TEST_CASE("driftwire cli update: latest.json names one version or nothing", "[driftwire][cli]") {
  CHECK(cli::parse_latest(R"({"version": "0.1.40"})") == "0.1.40");
  CHECK(cli::parse_latest(R"({"version":"1.2.3","released":"2026-10-25"})") == "1.2.3");
  CHECK_FALSE(cli::parse_latest(""));
  CHECK_FALSE(cli::parse_latest("<html>404</html>"));
  CHECK_FALSE(cli::parse_latest(R"({"version": 140})"));
  CHECK_FALSE(cli::parse_latest(R"({"version": "0.1.40\u001b[2J"})"));  // text for the terminal is no version
  CHECK_FALSE(cli::parse_latest(R"(["0.1.40"])"));
}

TEST_CASE("driftwire cli update: how this copy came to be here", "[driftwire][cli]") {
  const auto on_windows = [](std::string exe, std::string lad) {
    cli::install_facts f;
    f.exe = std::move(exe);
    f.local_app_data = std::move(lad);
    f.windows = true;
    return cli::classify_install(f);
  };
  CHECK(on_windows(R"(C:\Users\ann\AppData\Local\Programs\Driftwire\driftwire.exe)", R"(C:\Users\ann\AppData\Local)") ==
        cli::install_kind::windows);
  CHECK(on_windows(R"(c:\users\ANN\appdata\local\programs\driftwire\DRIFTWIRE.EXE)", R"(C:\Users\ann\AppData\Local\)") ==
        cli::install_kind::windows);
  CHECK(on_windows(R"(D:\src\mediaviewer\build\bin\Release\driftwire.exe)", R"(C:\Users\ann\AppData\Local)") ==
        cli::install_kind::none);
  CHECK(on_windows(R"(C:\Users\ann\AppData\Local\Programs\Driftwire\driftwire.exe)", "") == cli::install_kind::none);

  const auto on_mac = [](std::string exe) {
    cli::install_facts f;
    f.exe = std::move(exe);
    f.mac = true;
    return cli::classify_install(f);
  };
  CHECK(on_mac("/opt/homebrew/Cellar/driftwire/0.1.38/bin/driftwire") == cli::install_kind::homebrew);
  CHECK(on_mac("/usr/local/Cellar/driftwire/0.1.38/bin/driftwire") == cli::install_kind::homebrew);
  CHECK(on_mac("/Users/ann/src/mediaviewer/build/bin/driftwire") == cli::install_kind::none);

  const auto on_linux = [](std::string exe, std::vector<std::string> files = {}) {
    cli::install_facts f;
    f.exe = std::move(exe);
    f.exists = [files](const std::string& p) { return std::find(files.begin(), files.end(), p) != files.end(); };
    return cli::classify_install(f);
  };
  CHECK(on_linux("/usr/bin/driftwire") == cli::install_kind::linux_sh);
  CHECK(on_linux("/home/ann/.local/bin/driftwire", {"/home/ann/.local/share/driftwire/install-manifest"}) ==
        cli::install_kind::linux_sh);
  CHECK(on_linux("/usr/local/bin/driftwire", {"/usr/local/share/driftwire/install-manifest"}) ==
        cli::install_kind::linux_sh);
  CHECK(on_linux("/usr/local/bin/driftwire") == cli::install_kind::none);  // no installer's record
  CHECK(on_linux("/home/ann/build-cli/bin/driftwire") == cli::install_kind::none);
  CHECK(on_linux("") == cli::install_kind::none);
}

TEST_CASE("driftwire cli update: each channel's own command, with the site in it", "[driftwire][cli]") {
  CHECK(cli::update_command_shown(cli::install_kind::homebrew, kSite) == "brew upgrade driftwire");
  CHECK(cli::update_command_shown(cli::install_kind::windows, kSite) ==
        "irm https://example.invalid/driftwire/install.ps1 | iex");
  CHECK(cli::update_command_shown(cli::install_kind::linux_sh, kSite) ==
        "curl -fsSL https://example.invalid/driftwire/install.sh | sh");
  CHECK(cli::update_command_shown(cli::install_kind::none, kSite).empty());

  const std::string win = cli::update_command_run(cli::install_kind::windows, kSite);
  CHECK(win == "powershell.exe -NoProfile -Command \"irm 'https://example.invalid/driftwire/install.ps1' | iex\"");
  const std::string lin = cli::update_command_run(cli::install_kind::linux_sh, kSite);
  CHECK(has(lin, "curl -fsSL 'https://example.invalid/driftwire/install.sh' -o \"$t\""));
  CHECK(has(lin, "wget -qO \"$t\" 'https://example.invalid/driftwire/install.sh'"));
  CHECK(has(lin, "&& sh \"$t\""));  // a download that fails runs nothing
  CHECK(cli::update_command_run(cli::install_kind::homebrew, kSite) == "brew upgrade driftwire");
  CHECK(cli::update_command_run(cli::install_kind::none, kSite).empty());
  // An address a shell would read as more than an address is never put in a command.
  CHECK(cli::update_command_run(cli::install_kind::linux_sh, "https://x.invalid/a';rm -rf ~;'").empty());
  CHECK(cli::update_command_run(cli::install_kind::windows, "https://x.invalid/a b").empty());
}

TEST_CASE("driftwire cli update: the site is built in, or DRIFTWIRE_REPO_URL's when it is safe", "[driftwire][cli]") {
  const std::string built_in = cli::site_url(nullptr);
  CHECK(built_in.rfind("https://", 0) == 0);
  CHECK(built_in.back() != '/');
  CHECK(cli::site_url("") == built_in);
  CHECK(cli::site_url("https://mirror.example/driftwire/") == "https://mirror.example/driftwire");
  CHECK(cli::site_url("http://127.0.0.1:8123/driftwire") == "http://127.0.0.1:8123/driftwire");
  CHECK(cli::site_url("http://localhost:8123/driftwire") == "http://localhost:8123/driftwire");
  CHECK(cli::site_url("http://mirror.example/driftwire") == built_in);  // plain http elsewhere
  CHECK(cli::site_url("https://") == built_in);
  CHECK(cli::site_url("https://mirror.example/\"; evil") == built_in);
}

TEST_CASE("driftwire cli update: the kept answer, and when to ask again", "[driftwire][cli]") {
  cli::update_state s;
  s.check = false;
  s.checked = 1761400000;
  s.latest = "0.1.40";
  const cli::update_state back = cli::parse_update_state(cli::update_state_json(s));
  CHECK_FALSE(back.check);
  CHECK(back.checked == 1761400000);
  CHECK(back.latest == "0.1.40");
  // Anything broken is a fresh start: checking, never asked.
  const cli::update_state none = cli::parse_update_state("{\"check\": tr");
  CHECK(none.check);
  CHECK(none.checked == 0);
  CHECK(cli::parse_update_state(R"({"latest": "rm -rf"})").latest.empty());

  cli::update_state on;
  CHECK(cli::check_due(on, 1761400000));  // never asked
  on.checked = 1761400000;
  CHECK_FALSE(cli::check_due(on, 1761400000 + 3600));
  CHECK(cli::check_due(on, 1761400000 + cli::kCheckEverySeconds));
  CHECK(cli::check_due(on, 1761400000 - 60));  // the clock went back
  on.check = false;
  CHECK_FALSE(cli::check_due(on, 1761400000 + 10 * cli::kCheckEverySeconds));

  scratch_dir dir;
  REQUIRE(cli::save_update_state(dir.str(), s));
  REQUIRE(cli::save_update_state(dir.str(), s));  // over the last one
  const cli::update_state read = cli::load_update_state(dir.str());
  CHECK_FALSE(read.check);
  CHECK(read.latest == "0.1.40");
  CHECK_FALSE(fs::exists(dir.path / "driftwire_update.json.new"));
  CHECK(cli::load_update_state((dir.path / "nowhere").string()).check);
}

TEST_CASE("driftwire cli update: the check asks once a day, off the session's thread", "[driftwire][cli]") {
  scratch_dir dir;
  std::atomic<int> asked{0};
  std::string asked_url;
  std::mutex url_m;
  const auto site_says = [&](std::string body, int status = 200) {
    return [&, body, status](const std::string& url) {
      {
        std::lock_guard lock(url_m);
        asked_url = url;
      }
      ++asked;
      return cli::http_answer{status, body, {}};
    };
  };

  SECTION("a newer version: one wake, one notice") {
    waker w;
    cli::update_check c;
    c.start(dir.str(), "0.1.38", kSite, w.fn(), site_says(R"({"version": "0.1.40"})"));
    REQUIRE(w.wait_for(1));
    CHECK(asked == 1);
    {
      std::lock_guard lock(url_m);
      CHECK(asked_url == "https://example.invalid/driftwire/latest.json");
    }
    CHECK(c.take() == "0.1.40");
    CHECK(c.take().empty());  // once
    const cli::update_state kept = cli::load_update_state(dir.str());
    CHECK(kept.latest == "0.1.40");
    CHECK(kept.checked >= unix_now() - 60);

    // The next run within the day asks nobody, and says so at once.
    waker w2;
    cli::update_check again;
    again.start(dir.str(), "0.1.38", kSite, w2.fn(), site_says(R"({"version": "0.1.41"})"));
    CHECK(again.take() == "0.1.40");
    CHECK_FALSE(w2.wait_for(1, 200));
    CHECK(asked == 1);
    // Updated meanwhile: the kept version is not newer, so nothing to say.
    cli::update_check updated;
    updated.start(dir.str(), "0.1.40", kSite, w2.fn(), site_says(R"({"version": "0.1.41"})"));
    CHECK(updated.take().empty());
  }

  SECTION("up to date: kept, and nothing to say") {
    waker w;
    cli::update_check c;
    c.start(dir.str(), "0.1.40", kSite, w.fn(), site_says(R"({"version": "0.1.40"})"));
    for (int i = 0; i < 100 && cli::load_update_state(dir.str()).checked == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(cli::load_update_state(dir.str()).latest == "0.1.40");
    CHECK(c.take().empty());
    CHECK(w.count == 0);
  }

  SECTION("no answer, or not one: asked again next time") {
    for (const auto& [body, status] : std::vector<std::pair<std::string, int>>{
             {"", 0}, {"<html>Not Found</html>", 404}, {"{\"version\": \"soon\"}", 200}}) {
      CAPTURE(status);
      waker w;
      {
        cli::update_check c;
        c.start(dir.str(), "0.1.38", kSite, w.fn(), site_says(body, status));
        const int before = asked;
        for (int i = 0; i < 100 && asked == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(c.take().empty());
      }
      CHECK(cli::load_update_state(dir.str()).checked == 0);
      CHECK(w.count == 0);
    }
  }

  SECTION("turned off: no question at all, kept for next time") {
    cli::update_state off;
    off.check = false;
    off.latest = "0.1.40";  // even with a newer one known
    REQUIRE(cli::save_update_state(dir.str(), off));
    waker w;
    cli::update_check c;
    c.start(dir.str(), "0.1.38", kSite, w.fn(), site_says(R"({"version": "0.1.40"})"));
    CHECK_FALSE(c.enabled());
    CHECK(c.take().empty());
    CHECK_FALSE(w.wait_for(1, 200));
    CHECK(asked == 0);

    // On again: kept, and asked now (never asked before).
    REQUIRE(c.set_enabled(true));
    CHECK(cli::load_update_state(dir.str()).check);
    REQUIRE(w.wait_for(1));
    CHECK(c.take() == "0.1.40");
    REQUIRE(c.set_enabled(false));
    CHECK_FALSE(cli::load_update_state(dir.str()).check);
    CHECK(cli::load_update_state(dir.str()).latest == "0.1.40");
  }

  SECTION("stopped: an answer that comes later wakes nobody") {
    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool open = false;
    waker w;
    {
      cli::update_check c;
      c.start(dir.str(), "0.1.38", kSite, w.fn(), [&](const std::string&) {
        std::unique_lock lock(gate_m);
        gate_cv.wait(lock, [&] { return open; });
        return cli::http_answer{200, R"({"version": "0.1.40"})", {}};
      });
      c.stop();
    }
    {
      std::lock_guard lock(gate_m);
      open = true;
    }
    gate_cv.notify_all();
    for (int i = 0; i < 100 && cli::load_update_state(dir.str()).checked == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(w.count == 0);
    CHECK(cli::load_update_state(dir.str()).latest == "0.1.40");  // still kept for the next run
  }
}

TEST_CASE("driftwire cli update: `driftwire update` is a command of its own", "[driftwire][cli]") {
  const auto parse = [](std::vector<std::string> argv) {
    return cli::parse_args(std::span<const std::string>(argv), false);
  };
  CHECK(parse({"update"}).a.cmd == cli::command::update);
  CHECK(parse({"update"}).error.empty());
  CHECK(has(parse({"update", "now"}).error, "update takes no arguments"));
  CHECK(has(parse({"update", "--to", "Ann"}).error, "--to goes with send"));
  CHECK(has(cli::help_text(), "driftwire update"));
  CHECK(has(cli::help_text(), "DRIFTWIRE_NO_UPDATE_CHECK"));
}
