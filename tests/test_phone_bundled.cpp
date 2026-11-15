// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire program the Phone access add-on carries (src/addons/phone/
// bundled.*, docs/plans/phone-access-addon.md §8): installed into a stable
// folder, copied only when it changed, never written over in place (Windows'
// rename-aside run on every OS), and the staged swap of a running host: the
// supervisor started from the installed program ends 75 when an update is
// renamed in, for its start-at-login entry to start the new one. Loopback only.
#include "catch_compat.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/host_cmd.h"
#include "addons/phone/bundled.h"
#include "addons/phone/self_host.h"
#include "core/json.h"
#include "import_fixture.h"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace mv::test;
namespace bundled = mv::phone::bundled;

namespace {

void write_file(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::size_t olds_in(const fs::path& dir) {
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().filename().string().find(".old-") != std::string::npos) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("phone access: its driftwire is installed once, then replaced beside, never in place", "[phone-addon]") {
  for (const bool aside : {false, true}) {
    scratch_dir s(aside ? "phone_bundled_aside" : "phone_bundled");
    const fs::path src = s / "addon" / "host", bin = s / "feature" / "bin";
    write_file(src / bundled::program_name(), "program v1");
    write_file(src / "libsodium.dll", "a library beside it");

    auto first = bundled::install(utf8(src), utf8(bin), "1.0.0", aside);
    REQUIRE(first);
    CHECK_FALSE(first->replaced);
    CHECK(first->program == utf8(bin / bundled::program_name()));
    CHECK(read_file(bin / bundled::program_name()) == "program v1");
    CHECK(read_file(bin / "libsodium.dll") == "a library beside it");

    // The same version again (every launch): nothing copied, nothing renamed.
    const auto stamp = fs::last_write_time(bin / bundled::program_name());
    auto again = bundled::install(utf8(src), utf8(bin), "1.0.0", aside);
    REQUIRE(again);
    CHECK_FALSE(again->replaced);
    CHECK((fs::last_write_time(bin / bundled::program_name()) == stamp));

    // An update: the new files come in beside and are renamed into place.
    write_file(src / bundled::program_name(), "program v2, longer");
    auto next = bundled::install(utf8(src), utf8(bin), "1.0.1", aside);
    REQUIRE(next);
    CHECK(next->replaced);
    CHECK(read_file(bin / bundled::program_name()) == "program v2, longer");
    CHECK_FALSE(fs::exists(bin / (std::string(bundled::program_name()) + ".new")));
    // Windows' way: the running files went aside (two: the program, its DLL);
    // a later install removes them once nothing runs them.
    CHECK(olds_in(bin) == (aside ? 2u : 0u));
    REQUIRE(bundled::install(utf8(src), utf8(bin), "1.0.1", aside));
    CHECK(olds_in(bin) == 0u);

    // No program in the add-on (a developer's build): nothing done.
    CHECK(bundled::install(utf8(s / "empty"), utf8(bin), "1.0.2", aside).error() == mv::status::not_found);
    CHECK(read_file(bin / bundled::program_name()) == "program v2, longer");

    bundled::remove(utf8(bin));
    CHECK_FALSE(fs::exists(bin));
  }
}

TEST_CASE("phone access: the login entry is recognised as the add-on's when it runs its program", "[phone-addon]") {
  scratch_dir s("phone_login_entry");
  mv::phone::self_host::where w;
  w.login_entry = utf8(s / "entry.plist");
  const std::string mine = "/Users/a & b/Library/Application Support/MediaViewer/Phone Access/bin/driftwire";
  CHECK_FALSE(mv::phone::self_host::login_entry_runs(w, mine));  // none
  write_file(s / "entry.plist", "<string>/Users/a &amp; b/Library/Application Support/MediaViewer/Phone Access/bin/driftwire</string>");
  CHECK(mv::phone::self_host::login_entry_runs(w, mine));
  write_file(s / "entry.plist", "<string>/opt/homebrew/opt/driftwire/bin/driftwire</string>");
  CHECK_FALSE(mv::phone::self_host::login_entry_runs(w, mine));  // a separately installed one: left alone
  // The Windows task's XML: UTF-16LE with a BOM.
  const std::string task = "<Command>C:\\Users\\Ann\\bin\\driftwire.exe</Command>";
  std::string utf16 = "\xFF\xFE";
  for (char c : task) {
    utf16 += c;
    utf16 += '\0';
  }
  write_file(s / "task.xml", utf16);
  w.login_entry = utf8(s / "task.xml");
  CHECK(mv::phone::self_host::login_entry_runs(w, "C:\\Users\\Ann\\bin\\driftwire.exe"));
  // find() takes the add-on's copy before every other place.
  write_file(s / "bin" / bundled::program_name(), "x");
  mv::phone::self_host::set_bundled(utf8(s / "bin" / bundled::program_name()));
  CHECK(mv::phone::self_host::find().program == utf8(s / "bin" / bundled::program_name()));
  mv::phone::self_host::set_bundled("");
}

#if !defined(_WIN32) && defined(MV_DRIFTWIRE_CLI_TEST_PATH)
TEST_CASE("phone access: an add-on update swaps the program under a running host, which ends 75", "[phone-addon][host]") {
  scratch_dir s("phone_bundled_swap");
  const fs::path src = s / "addon" / "host", bin = s / "feature" / "bin";
  fs::create_directories(src);
  fs::copy_file(MV_DRIFTWIRE_CLI_TEST_PATH, src / bundled::program_name());
  auto installed = bundled::install(utf8(src), utf8(bin), "1.0.0", false);
  REQUIRE(installed);

  // The host, as the login entry starts it: the supervisor from the stable copy.
  const fs::path data = s / "data";
  const auto l = mv::driftwire::cli::host_layout_of(utf8(data));
  const std::vector<std::string> args{installed->program, "host", "--data", utf8(data), "--test-loopback", "--test-quick"};
  const pid_t sup = ::fork();
  if (sup == 0) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }
  REQUIRE(sup > 0);
  const auto running = [&] {
    const auto doc = mv::json::parse(read_file(l.state));
    return doc && doc->integer("pid").value_or(0) > 0;
  };
  for (int i = 0; i < 400 && !running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(running());

  // The add-on updated: its new program installed beside and renamed in.
  { std::ofstream(src / bundled::program_name(), std::ios::binary | std::ios::app) << '\0'; }
  auto next = bundled::install(utf8(src), utf8(bin), "1.0.1", false);
  REQUIRE(next);
  CHECK(next->replaced);
  std::optional<int> code;
  for (int i = 0; i < 400 && !code; ++i) {
    int st = 0;
    if (::waitpid(sup, &st, WNOHANG) == sup) code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    else std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!code) ::kill(sup, SIGKILL);
  REQUIRE(code);
  CHECK(*code == 75);  // the start-at-login entry starts the new one
}
#endif
