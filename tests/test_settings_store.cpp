// SPDX-License-Identifier: GPL-2.0-or-later
// PR 8: settings.ini writes are off the UI thread (plan/12 "Settings writes on
// the UI thread"). The store coalesces, writes atomically on its worker, and
// flushes on the exit path.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "shell/settings.h"
#include "shell/settings_store.h"

namespace fs = std::filesystem;
using namespace mv::shell;

namespace {

fs::path temp_dir(const char* tag) {
  const fs::path p = fs::temp_directory_path() /
                     (std::string("mv_settings_") + tag + "_" + std::to_string(::GetCurrentProcessId()) + "_" +
                      std::to_string(::GetTickCount64()));
  fs::create_directories(p);
  return p;
}

std::string read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// Detector armed on the test thread, which plays the UI thread.
struct ui_thread_guard {
  ui_thread_guard() { reset_settings_ui_thread_detector(); register_settings_ui_thread(); }
  ~ui_thread_guard() { reset_settings_ui_thread_detector(); }
};

}  // namespace

TEST_CASE("settings ini: parse and serialize round trip, unknown keys kept", "[shell][settings]") {
  const settings_doc doc = parse_settings_ini(
      "; comment\r\n[View]\r\nwrap = 0\r\n Background=2\r\n\r\n[update]\nauto_check=1\nlast_check=\"2026-09-14\"\n"
      "orphan\n[view]\nwrap=1\n");
  REQUIRE(doc.get_int("view", "WRAP", 1) == 0);  // first spelling wins, case-insensitive
  REQUIRE(doc.get_int("view", "background", 0) == 2);
  REQUIRE(doc.get("update", "last_check") == "2026-09-14");
  REQUIRE(doc.get_int("update", "missing", 7) == 7);
  REQUIRE(parse_settings_ini(serialize_settings_ini(doc)) == doc);

  settings_doc d = doc;
  d.set("privacy", "telemetry", "0");
  d.erase_prefix("view", "");
  REQUIRE(d.find("view", "wrap") == nullptr);
  REQUIRE(d.get("privacy", "telemetry") == "0");
}

TEST_CASE("settings store: typed values round-trip through the file and a restart", "[shell][settings]") {
  const ui_thread_guard ui;
  const fs::path dir = temp_dir("roundtrip");
  const std::wstring path = (dir / "settings.ini").wstring();
  {
    settings_store store(path);
    store.load_from_disk();
    REQUIRE(store.start());
    view_settings v;
    v.wrap = false;
    v.sticky_zoom = true;
    v.background = 3;
    save_view_settings(store, v);
    save_destinations(store, {"D:\\Cull", "E:\\K\xC3\xA9" "ep"});  // non-ANSI survives
    const key_override keys[] = {{4, 0x41, 2}, {9, 0x70, 0}};
    save_key_overrides(store, keys);
    store.set_int("update", "auto_check", 0);
    REQUIRE(store.flush(5000));
    REQUIRE(store.writes_ok() >= 1);
    store.stop();
  }
  REQUIRE_FALSE(fs::exists(dir / "settings.ini.tmp"));

  settings_store again(path);
  again.load_from_disk();
  const view_settings v = load_view_settings(again);
  REQUIRE_FALSE(v.wrap);
  REQUIRE(v.sticky_zoom);
  REQUIRE(v.background == 3);
  REQUIRE(load_destinations(again) == std::vector<std::string>{"D:\\Cull", "E:\\K\xC3\xA9" "ep"});
  const auto keys = load_key_overrides(again);
  REQUIRE(keys.size() == 2);
  REQUIRE((keys[1].row == 9 && keys[1].k == 0x70 && keys[1].mods == 0));
  REQUIRE(again.get_int("update", "auto_check", 1) == 0);

  // The file stays readable by the Win32 profile API an older build used.
  REQUIRE(::GetPrivateProfileIntW(L"view", L"background", -1, path.c_str()) == 3);
  wchar_t d1[64]{};
  ::GetPrivateProfileStringW(L"destinations", L"d1", L"", d1, 64, path.c_str());
  REQUIRE(std::wstring(d1) == L"E:\\K\u00E9ep");

  // Shrinking the keymap drops the old rows rather than leaving r1 behind.
  REQUIRE(again.start());
  save_key_overrides(again, {});
  REQUIRE(again.flush(5000));
  REQUIRE(settings_ui_thread_writes() == 0);  // every write was on the worker
  again.stop();
  settings_store third(path);
  third.load_from_disk();
  REQUIRE(third.get("keys", "r1").empty());
  REQUIRE(load_key_overrides(third).empty());
  fs::remove_all(dir);
}

TEST_CASE("settings store: reads a PR 4-7 ANSI file written by the profile API", "[shell][settings]") {
  const fs::path dir = temp_dir("legacy");
  const std::wstring path = (dir / "settings.ini").wstring();
  REQUIRE(::WritePrivateProfileStringW(L"view", L"wrap", L"0", path.c_str()));
  REQUIRE(::WritePrivateProfileStringW(L"keys", L"n", L"1", path.c_str()));
  REQUIRE(::WritePrivateProfileStringW(L"keys", L"r0", L"3,65,1", path.c_str()));
  settings_store store(path);
  store.load_from_disk();
  REQUIRE_FALSE(load_view_settings(store).wrap);
  REQUIRE(load_key_overrides(store).size() == 1);
  fs::remove_all(dir);
}

TEST_CASE("settings store: a burst of changes coalesces into few writes of the last state",
          "[shell][settings]") {
  const ui_thread_guard ui;
  const fs::path dir = temp_dir("coalesce");
  const std::wstring path = (dir / "settings.ini").wstring();
  settings_store store(path);
  store.load_from_disk();
  REQUIRE(store.start());
  for (int i = 0; i < 500; ++i) store.set_int("view", "background", i % 4);
  store.set_int("view", "background", 1);
  REQUIRE(store.flush(5000));
  // 501 mutations; the worker only ever writes the newest waiting snapshot.
  REQUIRE(store.writes_ok() >= 1);
  REQUIRE(store.writes_ok() < 501);
  REQUIRE(settings_ui_thread_writes() == 0);
  const std::uint64_t writes = store.writes_ok();
  store.set_int("view", "background", 1);  // no change: no write queued
  REQUIRE(store.flush(5000));
  REQUIRE(store.writes_ok() == writes);
  store.stop();

  settings_store after(path);
  after.load_from_disk();
  REQUIRE(after.get_int("view", "background", -1) == 1);
  fs::remove_all(dir);
}

TEST_CASE("settings store: mutations from several threads are all kept", "[shell][settings]") {
  const fs::path dir = temp_dir("threads");
  const std::wstring path = (dir / "settings.ini").wstring();
  settings_store store(path);
  REQUIRE(store.start());
  std::thread a([&] { for (int i = 0; i < 200; ++i) store.set_int("a", "k" + std::to_string(i), i); });
  std::thread b([&] { for (int i = 0; i < 200; ++i) store.set_int("b", "k" + std::to_string(i), i); });
  a.join();
  b.join();
  REQUIRE(store.flush(5000));
  store.stop();
  settings_store after(path);
  after.load_from_disk();
  REQUIRE(after.get_int("a", "k199", -1) == 199);
  REQUIRE(after.get_int("b", "k0", -1) == 0);
  REQUIRE(after.snapshot()->sections.size() == 2);
  fs::remove_all(dir);
}

TEST_CASE("settings store: a failing write keeps memory and the old file, and is retried",
          "[shell][settings]") {
  const ui_thread_guard ui;
  const fs::path dir = temp_dir("failing");
  const std::wstring path = (dir / "settings.ini").wstring();
  {
    settings_store seed(path);
    REQUIRE(seed.start());
    seed.set("view", "wrap", "1");
    REQUIRE(seed.flush(5000));
  }
  const std::string before = read_bytes(dir / "settings.ini");

  // A directory squatting on the temp name makes every write fail.
  fs::create_directories(dir / "settings.ini.tmp");
  settings_store store(path);
  store.load_from_disk();
  REQUIRE(store.start());
  store.set("view", "wrap", "0");
  REQUIRE_FALSE(store.flush(2000));
  REQUIRE(store.writes_failed() >= 1);
  REQUIRE(store.get_int("view", "wrap", -1) == 0);        // in memory, not lost
  REQUIRE(read_bytes(dir / "settings.ini") == before);   // old file intact

  fs::remove(dir / "settings.ini.tmp");
  REQUIRE(store.flush(5000));  // the exit path retries the failed snapshot
  store.stop();
  settings_store after(path);
  after.load_from_disk();
  REQUIRE(after.get_int("view", "wrap", -1) == 0);
  REQUIRE(settings_ui_thread_writes() == 0);
  fs::remove_all(dir);
}

TEST_CASE("settings store: flush without a worker writes on the exit path", "[shell][settings]") {
  const ui_thread_guard ui;
  const fs::path dir = temp_dir("exit");
  const std::wstring path = (dir / "settings.ini").wstring();
  settings_store store(path);  // never started: e.g. the worker failed to start
  store.set("privacy", "telemetry", "0");
  REQUIRE_FALSE(fs::exists(dir / "settings.ini"));  // a set alone never touches disk
  REQUIRE(store.flush(1000));
  REQUIRE(settings_ui_thread_writes() == 0);  // exit-path write is allowed
  settings_store after(path);
  after.load_from_disk();
  REQUIRE(after.get("privacy", "telemetry") == "0");

  // The detector itself: a direct write on the registered UI thread is counted.
  REQUIRE(write_settings_file_atomic(path, "[x]\r\ny=1\r\n"));
  REQUIRE(settings_ui_thread_writes() == 1);
  fs::remove_all(dir);
}
