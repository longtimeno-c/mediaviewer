// SPDX-License-Identifier: GPL-2.0-or-later
// PR 8 verify: "Exercise update signature rejection and rollback" — the
// native start-attempt half. Signature rules: src.managed/MediaViewer.Updater.Tests.
#include <catch2/catch_test_macros.hpp>

#include "shell/update_guard.h"

using namespace mv::shell::update;

namespace {

trial_record armed(int attempts, std::string prior_package = "MediaViewer-0.1.1-full.nupkg") {
  return trial_record{"0.1.2", "0.1.1", std::move(prior_package), attempts};
}

// Runs the guard the way the host does across successive process starts,
// applying the write each decision implies.
start_action start_once(trial_record& t, std::string_view running) {
  const start_decision d = decide_start(t, running);
  if (d.action == start_action::counted) t.attempts = d.attempts;
  if (d.action == start_action::abandon || d.action == start_action::rollback) t = {};
  return d.action;
}

}  // namespace

TEST_CASE("update guard: no trial is a normal start", "[update]") {
  CHECK(decide_start({}, "0.1.1").action == start_action::normal);
}

TEST_CASE("update guard: two failed starts roll back on the third", "[update]") {
  trial_record t = armed(0);
  CHECK(start_once(t, "0.1.2") == start_action::counted);  // start 1 ... crashes
  CHECK(t.attempts == 1);
  CHECK(start_once(t, "0.1.2") == start_action::counted);  // start 2 ... crashes
  CHECK(t.attempts == 2);
  CHECK(start_once(t, "0.1.2") == start_action::rollback);  // never runs the broken build a third time
  CHECK(t.version.empty());
  CHECK(start_once(t, "0.1.1") == start_action::normal);  // the prior version starts clean
}

TEST_CASE("update guard: a confirmed start ends the trial", "[update]") {
  trial_record t = armed(0);
  CHECK(start_once(t, "0.1.2") == start_action::counted);
  t = {};  // confirm_started cleared [trial]
  CHECK(start_once(t, "0.1.2") == start_action::normal);
}

TEST_CASE("update guard: one crash then a good start does not roll back", "[update]") {
  trial_record t = armed(0);
  CHECK(start_once(t, "0.1.2") == start_action::counted);
  CHECK(start_once(t, "0.1.2") == start_action::counted);  // second start is the good one
  t = {};
  CHECK(decide_start(t, "0.1.2").action == start_action::normal);
}

TEST_CASE("update guard: stale record for a version that never applied is dropped", "[update]") {
  CHECK(decide_start(armed(0), "0.1.1").action == start_action::abandon);
  CHECK(decide_start(armed(5), "0.1.1").action == start_action::abandon);
  CHECK(decide_start(armed(0), "").action == start_action::abandon);
}

TEST_CASE("update guard: nothing kept to roll back to abandons instead of looping", "[update]") {
  CHECK(decide_start(armed(2, ""), "0.1.2").action == start_action::abandon);
}

TEST_CASE("update guard: sq.version parsing", "[update]") {
  CHECK(parse_manifest_version("<?xml?><package><metadata><id>MediaViewer</id>"
                               "<version>0.1.2</version></metadata></package>") == "0.1.2");
  CHECK(parse_manifest_version("<version>\r\n 1.2.3 \n</version>") == "1.2.3");
  CHECK(parse_manifest_version("<id>x</id>").empty());
  CHECK(parse_manifest_version("<version>1.0\\..\\x</version>").empty());
}

TEST_CASE("update guard: failed-version list", "[update]") {
  CHECK(append_version_list("", "0.1.2") == "0.1.2");
  CHECK(append_version_list("0.1.2", "0.1.2") == "0.1.2");
  CHECK(append_version_list("0.1.0,0.1.2", "0.1.3") == "0.1.0,0.1.2,0.1.3");
}

TEST_CASE("update guard: hook arguments", "[update]") {
  CHECK(is_velopack_hook(L"--veloapp-install"));
  CHECK(is_velopack_hook(L"--veloapp-updated"));
  CHECK(is_velopack_hook(L"--squirrel-firstrun"));
  CHECK_FALSE(is_velopack_hook(L"--open"));
  CHECK_FALSE(is_velopack_hook(L"C:\\photos"));
}

TEST_CASE("update guard: restart arguments carry the view", "[update]") {
  view_restore v;
  CHECK(restart_arguments(v).empty());
  v.path = L"D:\\DCIM\\IMG_0001.JPG";
  v.zoom_percent = 200;
  v.fullscreen = true;
  const auto args = restart_arguments(v);
  REQUIRE(args.size() == 4);
  CHECK(args[0] == L"--restore-zoom");
  CHECK(args[1] == L"200");
  CHECK(args[2] == L"--restore-fullscreen");
  CHECK(args[3] == v.path);
  v.path = L"-weird";
  CHECK(restart_arguments(v).back() != L"-weird");
  const std::wstring blob = join_arguments({L"a", L"bc"});
  CHECK(blob == std::wstring(L"a\0bc\0", 5));
}

// plan/10 PR 8 verify: "Uninstall from Apps & features removes the shortcuts
// and install directory." Velopack writes a second entry that would remove the
// directory WITHOUT the wizard's shortcuts (and, from PR 15, without the
// ProgId registrations), so the host deletes it after every update. It must
// only ever delete an entry that points at this install's own Update.exe.
TEST_CASE("update guard: only Velopack's own uninstall entry is claimed", "[update]") {
  const std::wstring update_exe = LR"(C:\Users\a\AppData\Local\MediaViewer\Update.exe)";

  CHECK(is_velopack_uninstall_string(L"\"" + update_exe + L"\" --uninstall", update_exe));
  CHECK(is_velopack_uninstall_string(update_exe + L" --uninstall", update_exe));
  // Case-insensitive, like the filesystem.
  CHECK(is_velopack_uninstall_string(
      LR"("c:\users\a\appdata\local\mediaviewer\update.exe" --uninstall)", update_exe));
  // No arguments at all is still ours.
  CHECK(is_velopack_uninstall_string(L"\"" + update_exe + L"\"", update_exe));

  // The wizard's own entry: left alone, or the app becomes ununinstallable.
  CHECK_FALSE(is_velopack_uninstall_string(
      LR"("C:\Users\a\AppData\Local\MediaViewer\unins000.exe" /SILENT)", update_exe));
  // Another install of the same app elsewhere.
  CHECK_FALSE(is_velopack_uninstall_string(LR"("D:\Other\MediaViewer\Update.exe" --uninstall)",
                                           update_exe));
  // A different product that happens to use the key name.
  CHECK_FALSE(is_velopack_uninstall_string(LR"("C:\Program Files\Thing\unins.exe")", update_exe));
  // Malformed input must not match anything.
  CHECK_FALSE(is_velopack_uninstall_string(L"\"" + update_exe, update_exe));
  CHECK_FALSE(is_velopack_uninstall_string(L"", update_exe));
  CHECK_FALSE(is_velopack_uninstall_string(L"\"" + update_exe + L"\" --uninstall", L""));

  // A dev build is not an install: nothing is touched.
  install_layout dev;
  CHECK_FALSE(remove_velopack_uninstall_entry(dev));
}
