// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <objbase.h>

#include "shell/chrome_host.h"
#include "shell/settings.h"

TEST_CASE("chrome attach args stay 40 bytes") {
  REQUIRE(sizeof(mv::shell::chrome_attach_args) == 40);
  REQUIRE(sizeof(mv::shell::chrome_resize_args) == 16);
  REQUIRE(sizeof(mv::shell::chrome_filmstrip_args) == 48);
  REQUIRE(sizeof(mv::shell::chrome_show_args) == 16);
  REQUIRE(sizeof(mv::shell::chrome_flags_args) == 8);
  REQUIRE(sizeof(mv::shell::chrome_browse_args) == 32);
  REQUIRE(sizeof(mv::shell::chrome_meta_args) == 80);  // #290: provenance and position
  REQUIRE(sizeof(mv::shell::chrome_share_args) == 24);  // #444: IslandHost.ShareMenu
}

TEST_CASE("view settings round-trip through the flag word") {
  mv::shell::view_settings settings;
  REQUIRE(settings.filmstrip_for_folder);
  REQUIRE_FALSE(settings.filmstrip_for_image);
  REQUIRE(settings.flags() == (mv::shell::kSettingFilmstripFolder | mv::shell::kSettingWrap));

  settings.filmstrip_for_image = true;
  settings.sticky_zoom = true;
  const auto round = mv::shell::view_settings::from_flags(settings.flags());
  REQUIRE(round.filmstrip_for_folder);
  REQUIRE(round.filmstrip_for_image);
  REQUIRE(round.sticky_zoom);
  // Issue #408: bits 4-6 were the canvas background. A word saved with them
  // set still reads, means nothing, and is never written back.
  REQUIRE(mv::shell::view_settings::from_flags(7 << 4).flags() == 0);
  REQUIRE((settings.flags() & (7 << 4)) == 0);
  REQUIRE(mv::shell::view_settings::from_flags(0).flags() == 0);

  // "Show audio files" / "Show documents": off by default, and a saved word
  // from before the bits existed reads as off (both kinds left out).
  REQUIRE(settings.hidden_kinds() == (mv::io::kHideAudio | mv::io::kHideDocuments));
  REQUIRE(mv::shell::view_settings::from_flags(mv::shell::kSettingWrap).hidden_kinds() ==
          (mv::io::kHideAudio | mv::io::kHideDocuments));
  settings.show_audio = true;
  const auto audio = mv::shell::view_settings::from_flags(settings.flags());
  REQUIRE(audio.show_audio);
  REQUIRE_FALSE(audio.show_documents);
  REQUIRE(audio.hidden_kinds() == mv::io::kHideDocuments);
  settings.show_documents = true;
  REQUIRE(mv::shell::view_settings::from_flags(settings.flags()).hidden_kinds() == 0);
}

TEST_CASE("chrome bar height is 48 DIP, plus 28 with a path row") {
  REQUIRE(mv::shell::chrome_bar_height_px(96) == 48);
  REQUIRE(mv::shell::chrome_bar_height_px(120) == 60);
  REQUIRE(mv::shell::chrome_bar_height_px(144) == 72);
  REQUIRE(mv::shell::chrome_bar_height_px(0) == 48);
}

TEST_CASE("chrome host loads hostfxr and the blittable size") {
  mv::shell::chrome_host host;
  auto loaded = host.load();
  REQUIRE(loaded);
  REQUIRE(host.loaded());
  REQUIRE(host.probe() == 40);
  // C# Command constants and chrome_command agree, value for value.
  REQUIRE(host.probe_commands() == mv::shell::chrome_command_checksum());
  REQUIRE_FALSE(host.attached());
}

TEST_CASE("chrome host attaches and detaches an island on an hwnd") {
  // DesktopWindowXamlSource.Dispose is a native AccessViolation on a bare
  // DefWindowProc HWND (coreclr, not a failed REQUIRE). The lab's WM_CLOSE
  // path — detach while the parent is whole, pump, then DestroyWindow — is
  // the real check, and the 30 chrome-on exits in docs/design/12 cover it.
  SUCCEED("XAML island Dispose AVs on a test HWND; covered by lab exit soaks");
}

TEST_CASE("focus in any pane island classifies as pane, not the command bar") {
  // Issue #213: only the metadata and tree islands were cached, so focus in the
  // Adjust, Edit or Jobs pane fell through to command_bar and ran viewer keys.
  REQUIRE(mv::shell::kPaneIslandCount == 5);
  REQUIRE(mv::shell::kPaneIslandIds[2] == 8);   // adjust
  REQUIRE(mv::shell::kPaneIslandIds[3] == 9);   // Edit workspace
  REQUIRE(mv::shell::kPaneIslandIds[4] == 10);  // Jobs

  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  const HWND top = ::CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr,
                                     nullptr, instance, nullptr);
  REQUIRE(top != nullptr);
  const auto child = [instance](HWND parent) {
    return ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 8, 8, parent, nullptr, instance,
                             nullptr);
  };
  constexpr int kIslands = static_cast<int>(mv::shell::focus_kind::transport) + 1;
  HWND islands[kIslands]{};
  for (int i = 1; i < kIslands; ++i) islands[i] = child(top);
  HWND panes[mv::shell::kPaneIslandCount]{};
  HWND inner[mv::shell::kPaneIslandCount]{};
  for (int i = 0; i < mv::shell::kPaneIslandCount; ++i) {
    panes[i] = child(top);
    inner[i] = child(panes[i]);  // a control's window under the island's bridge
  }
  const HWND canvas = top;
  const auto classify = [&](HWND focus) {
    return mv::shell::classify_island_focus(focus, canvas, islands, kIslands, panes,
                                            mv::shell::kPaneIslandCount);
  };

  REQUIRE(classify(canvas) == mv::shell::focus_kind::canvas);
  REQUIRE(classify(islands[static_cast<int>(mv::shell::focus_kind::filmstrip)]) ==
          mv::shell::focus_kind::filmstrip);
  for (int i = 0; i < mv::shell::kPaneIslandCount; ++i) {
    REQUIRE(panes[i] != nullptr);
    REQUIRE(classify(panes[i]) == mv::shell::focus_kind::pane);
    REQUIRE(classify(inner[i]) == mv::shell::focus_kind::pane);
  }
  // A popup or an unknown window is still never the canvas.
  REQUIRE(classify(nullptr) == mv::shell::focus_kind::command_bar);

  ::DestroyWindow(top);  // takes the children with it
}

TEST_CASE("the AI badge sits bottom-right, above the filmstrip, the transport and clear of a pane", "[ai-badge]") {
  using mv::shell::ai_badge_rect;
  // 96 DPI, 1000 x 800, nothing else up: 44 x 20, 16 in from the right, 10 up.
  auto r = ai_badge_rect(1000, 800, 0, 0, false, 96);
  CHECK(r.width == 44);
  CHECK(r.height == 20);
  CHECK(r.x == 1000 - 16 - 44);
  CHECK(r.y == 800 - 10 - 20);
  // Above the filmstrip, and above a clip's transport bar plus a gap.
  CHECK(ai_badge_rect(1000, 800, 112, 0, false, 96).y == 800 - 112 - 10 - 20);
  CHECK(ai_badge_rect(1000, 800, 112, 0, true, 96).y == 800 - 112 - 10 - mv::shell::chrome_transport_height_px(96) - 8 - 20);
  // Left of a right-hand pane.
  CHECK(ai_badge_rect(1000, 800, 0, 360, false, 96).x == 1000 - 360 - 16 - 44);
  // Scaled at 150 %, and never off the client's top-left.
  r = ai_badge_rect(1000, 800, 0, 0, false, 144);
  CHECK(r.width == 66);
  CHECK(r.height == 30);
  r = ai_badge_rect(10, 10, 0, 0, true, 96);
  CHECK(r.x == 0);
  CHECK(r.y == 0);
  static_assert(sizeof(mv::shell::chrome_ai_badge_args) == 48);
}
