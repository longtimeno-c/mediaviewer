// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #153: the state both chromes drive, tested headless.
//
//  - A key remap survives persist and reload exactly (commands.h live_key_remaps /
//    apply_key_remaps; on Windows also through settings.ini). Both hosts persist with
//    these, so this is the round trip for the WinUI and the SwiftUI Settings screens.
//  - A per-mode snapshot of describe_commands(), the table `?`, the palette and both
//    Settings screens are built from (tests/data/command_table.snapshot). A change to
//    the default map shows up as a diff of that file in review. Regenerate it with
//    MV_UPDATE_SNAPSHOTS=1 and read the diff before committing it.
//
// The chromes' own copies of the command ids are checked against commands.h by
// src.managed/MediaViewer.Chrome.Tests (C#, and the Swift sources as text) and by the
// Swift packages' test targets.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "shell/commands.h"

#if defined(_WIN32)
#include <windows.h>

#include "shell/settings.h"
#include "shell/settings_store.h"
#endif

namespace fs = std::filesystem;
using namespace mv::shell;

namespace {

struct row_keys {
  key k;
  std::uint8_t mods;
  bool operator==(const row_keys&) const = default;
};

std::vector<row_keys> live_keys() {
  std::vector<row_keys> out;
  for (const binding& b : live_bindings()) out.push_back(row_keys{b.k, b.mods});
  return out;
}

// The built-in table only: no add-on rows, every family absent.
struct clean_table {
  clean_table() { reset(); }
  ~clean_table() { reset(); }
  static void reset() {
    set_addon_commands({});
    set_addon_commands_available(addon_family::import, false);
    set_addon_commands_available(addon_family::ai, false);
    reset_live_bindings();
  }
};

int row_of(command_id id) {
  const auto rows = default_bindings();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].command == id) return static_cast<int>(i);
  }
  return -1;
}

key random_key(std::mt19937& rng) {
  // Letters, digits and every named key: what Settings capture can produce.
  std::uniform_int_distribution<int> pick(0, 35 + (kKeyCount - static_cast<int>(key::space)));
  const int v = pick(rng);
  if (v < 26) return char_key(static_cast<char>('A' + v));
  if (v < 36) return char_key(static_cast<char>('0' + v - 26));
  return static_cast<key>(static_cast<int>(key::space) + v - 36);
}

const char* const kModeNames[] = {"browse", "video", "slideshow", "island", "loupe",
                                  "gallery", "runner", "crop", "trim", "skim"};
static_assert(std::size(kModeNames) == static_cast<std::size_t>(kModeCount),
              "a new mode needs a name here and a section in command_table.snapshot");

// describe_commands() is "id\tmodes\tname\tkeys\trunnable\trow" per line. The snapshot
// regroups it by mode: what `?` and Settings list while that mode is live.
std::string per_mode_snapshot() {
  struct line {
    int id;
    unsigned modes;
    std::string name, keys, runnable;
  };
  std::vector<line> lines;
  std::istringstream in(describe_commands());
  std::string text;
  while (std::getline(in, text)) {
    std::vector<std::string> f;
    std::size_t start = 0;
    for (;;) {
      const std::size_t tab = text.find('\t', start);
      f.push_back(text.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    REQUIRE(f.size() == 6);
    lines.push_back(line{std::stoi(f[0]), static_cast<unsigned>(std::stoul(f[1])), f[2], f[3], f[4]});
  }
  std::string out =
      "# describe_commands() by mode (tests/test_command_contract.cpp). One line per binding:\n"
      "# keys <TAB> command name <TAB> command id <TAB> palette-runnable. Rows always have\n"
      "# tabs; a line without one is a comment or a [mode] header (a key can be [ or #).\n"
      "# Regenerate: MV_UPDATE_SNAPSHOTS=1 mv_tests \"[snapshot]\", then review the diff.\n";
  for (int m = 0; m < kModeCount; ++m) {
    out += "\n[";
    out += kModeNames[m];
    out += "]\n";
    for (const line& l : lines) {
      if ((l.modes & mask_of(static_cast<mode>(m))) == 0) continue;
      out += l.keys + "\t" + l.name + "\t" + std::to_string(l.id) + "\t" + l.runnable + "\n";
    }
  }
  return out;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::string s{std::istreambuf_iterator<char>(in), {}};
  // A checkout with core.autocrlf=true hands the file back with CRLF.
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c != '\r') out += c;
  }
  return out;
}

}  // namespace

TEST_CASE("key remaps: the default table persists as nothing", "[shell][commands][remap]") {
  const clean_table clean;
  REQUIRE(live_key_remaps().empty());
  const auto before = live_keys();
  apply_key_remaps({});
  REQUIRE(live_keys() == before);
}

TEST_CASE("key remaps: a row a swap left unbound stays unbound after a reload",
          "[shell][commands][remap]") {
  const clean_table clean;
  // Open RAW ships without a key. Giving it Next's key swaps, so Next is unbound;
  // moving Open RAW on to a free key leaves Next unbound. The reload used to drop
  // the unbound row (key 0) and so hand Next its default key back.
  const int raw = row_of(command_id::open_raw);
  const int next = row_of(command_id::next);
  REQUIRE(raw >= 0);
  REQUIRE(next >= 0);
  REQUIRE(default_bindings()[static_cast<std::size_t>(raw)].k == key::none);
  const binding next_default = default_bindings()[static_cast<std::size_t>(next)];
  REQUIRE(next_default.k != key::none);

  REQUIRE(rebind_live(raw, next_default.k, next_default.mods));
  REQUIRE(live_bindings()[static_cast<std::size_t>(next)].k == key::none);
  REQUIRE(rebind_live(raw, key::f9, mod_ctrl | mod_alt | mod_shift));
  REQUIRE(live_bindings()[static_cast<std::size_t>(next)].k == key::none);

  const auto before = live_keys();
  const auto saved = live_key_remaps();
  reset_live_bindings();
  apply_key_remaps(saved);
  REQUIRE(live_keys() == before);
  REQUIRE(live_bindings()[static_cast<std::size_t>(next)].k == key::none);
}

TEST_CASE("key remaps: any sequence of Settings remaps survives persist and reload",
          "[shell][commands][remap]") {
  const clean_table clean;
  const int rows = static_cast<int>(default_bindings().size());
  std::mt19937 rng(153);
  std::uniform_int_distribution<int> pick_row(0, rows - 1);
  std::uniform_int_distribution<int> pick_mods(0, kModCombos - 1);
  std::uniform_int_distribution<int> pick_len(1, 40);
  for (int trial = 0; trial < 300; ++trial) {
    reset_live_bindings();
    const int len = pick_len(rng);
    for (int i = 0; i < len; ++i) {
      (void)rebind_live(pick_row(rng), random_key(rng), static_cast<std::uint8_t>(pick_mods(rng)));
    }
    const auto before = live_keys();
    const auto saved = live_key_remaps();
    reset_live_bindings();
    apply_key_remaps(saved);
    INFO("trial " << trial << ", " << len << " remaps, " << saved.size() << " rows saved");
    const auto after = live_keys();
    for (std::size_t i = 0; i < before.size(); ++i) {
      INFO("row " << i << " (command " << static_cast<int>(live_bindings()[i].command) << "): key "
                  << static_cast<int>(before[i].k) << "+" << int{before[i].mods} << " came back as "
                  << static_cast<int>(after[i].k) << "+" << int{after[i].mods});
      REQUIRE(after[i] == before[i]);
    }
    // And the reloaded table persists as the same list.
    const auto again = live_key_remaps();
    REQUIRE(again.size() == saved.size());
    for (std::size_t i = 0; i < saved.size(); ++i) {
      REQUIRE(again[i].row == saved[i].row);
      REQUIRE(again[i].k == saved[i].k);
      REQUIRE(again[i].mods == saved[i].mods);
    }
  }
}

TEST_CASE("key remaps: every live row and key fits the chrome_cmd_rebind wire", "[shell][commands][remap]") {
  // IslandHost.Settings.cs packs row | key << 8 | mods << 20 into one int, and
  // main.cpp unpacks row & 0xFF, key & 0xFFF (MediaViewer.Chrome.Tests pins both).
  REQUIRE(default_bindings().size() + kAddonCommandSlots <= 0x100);
  REQUIRE(kKeyCount - 1 <= 0xFFF);
  REQUIRE(kModCombos - 1 <= 7);
}

TEST_CASE("key remaps: Reset in Settings persists as the default table", "[shell][commands][remap]") {
  const clean_table clean;
  REQUIRE(rebind_live(0, key::f9, mod_ctrl));
  REQUIRE_FALSE(live_key_remaps().empty());
  reset_live_bindings();  // chrome_cmd_reset_keys
  REQUIRE(live_key_remaps().empty());
}

#if defined(_WIN32)
TEST_CASE("key remaps: round trip through settings.ini and a restart", "[shell][commands][remap][settings]") {
  const clean_table clean;
  const fs::path dir = fs::temp_directory_path() /
                       ("mv_remap_" + std::to_string(::GetCurrentProcessId()) + "_" +
                        std::to_string(::GetTickCount64()));
  fs::create_directories(dir);
  const std::wstring path = (dir / "settings.ini").wstring();

  // What chrome_cmd_rebind does (main.cpp): rebind_live, then persist_live_keys.
  const int raw = row_of(command_id::open_raw);
  const int next = row_of(command_id::next);
  const binding next_default = default_bindings()[static_cast<std::size_t>(next)];
  REQUIRE(rebind_live(raw, next_default.k, next_default.mods));
  REQUIRE(rebind_live(raw, key::f9, mod_ctrl | mod_alt | mod_shift));
  REQUIRE(rebind_live(row_of(command_id::help), char_key('/'), mod_ctrl));
  const auto before = live_keys();
  {
    settings_store store(path);
    store.load_from_disk();
    REQUIRE(store.start());
    save_key_remaps(store, live_key_remaps());
    REQUIRE(store.flush(5000));
    store.stop();
  }

  // A restart: defaults, then what the file says (wWinMain).
  reset_live_bindings();
  settings_store again(path);
  again.load_from_disk();
  apply_key_remaps(load_key_remaps(again));
  REQUIRE(live_keys() == before);
  REQUIRE(live_bindings()[static_cast<std::size_t>(next)].k == key::none);
  fs::remove_all(dir);
}
#endif

TEST_CASE("describe_commands: per-mode snapshot matches tests/data/command_table.snapshot",
          "[shell][commands][snapshot]") {
  const clean_table clean;
  const fs::path file = fs::path(__FILE__).parent_path() / "data" / "command_table.snapshot";
  const std::string now = per_mode_snapshot();
  const char* update = std::getenv("MV_UPDATE_SNAPSHOTS");
  if (update != nullptr && update[0] == '1') {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << now;
    REQUIRE(out.good());
    WARN("rewrote " << file.string());
    return;
  }
  REQUIRE(fs::exists(file));
  const std::string was = read_text(file);
  if (was != now) {
    // Name the first section and line that moved, not two 20 KB strings.
    std::istringstream a(was), b(now);
    std::string la, lb, section;
    int n = 0;
    while (true) {
      const bool ga = static_cast<bool>(std::getline(a, la));
      const bool gb = static_cast<bool>(std::getline(b, lb));
      ++n;
      if (!ga && !gb) break;
      if (gb && !lb.empty() && lb[0] == '[' && lb.find('\t') == std::string::npos) section = lb;
      if (!ga || !gb || la != lb) {
        FAIL_CHECK("command_table.snapshot line " << n << " in " << section << ":\n  snapshot: "
                                                  << (ga ? la : "<end>") << "\n  now:      "
                                                  << (gb ? lb : "<end>")
                                                  << "\nIf the change is intended, rerun with "
                                                     "MV_UPDATE_SNAPSHOTS=1 and review the diff.");
        break;
      }
    }
  }
  REQUIRE(was == now);
}

TEST_CASE("describe_commands: every listed row is a named, keyed command", "[shell][commands]") {
  const clean_table clean;
  std::istringstream in(describe_commands());
  std::string text;
  int count = 0;
  while (std::getline(in, text)) {
    ++count;
    const int id = std::stoi(text.substr(0, text.find('\t')));
    INFO(text);
    REQUIRE(id > 0);
    REQUIRE(id < kCommandCount);
    REQUIRE_FALSE(is_reserved_notification(id));
    REQUIRE_FALSE(is_retired_command(id));
    const command_info* info = find_command(static_cast<command_id>(id));
    REQUIRE(info != nullptr);
    REQUIRE_FALSE(info->keyless);
  }
  REQUIRE(count > 50);
}
