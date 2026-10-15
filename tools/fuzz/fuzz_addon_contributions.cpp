// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: what an open add-on's manifest and theme files carry
// as data (docs/design/25 PR 56) — the settings page schema, keymap packs and the
// theme parser with its shape — each from a stranger's bytes. The page is
// also rendered as the hosts read it, so the value folding is covered too.
//
// Built from the sources (no mv_addon, so no libsodium in the fuzz build).
// On a Mac with Homebrew's LLVM, for a quick local run:
//   /opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -Isrc tools/fuzz/fuzz_addon_contributions.cpp src/addon/settings_schema.cpp \
//     src/addon/keymap.cpp src/addon/theme.cpp -o build/fuzz_addon_contributions
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "addon/keymap.h"
#include "addon/settings_schema.h"
#include "addon/theme.h"
#include "core/json.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);

  std::string detail;
  if (auto page = mv::addon::parse_settings_page_text(text, &detail)) {
    // Every item's default must fit its own rule, and read back as JSON.
    for (const auto& section : page->sections) {
      for (const auto& item : section.items) {
        const std::string def = mv::addon::setting_default_json(item);
        const auto parsed = mv::json::parse(def, 2);
        if (!parsed || !mv::addon::setting_value_fits(item, *parsed)) std::abort();
        if (page->find(item.key) == nullptr) std::abort();
      }
    }
  }

  if (auto packs = mv::addon::parse_keymaps_text(text, &detail)) {
    // What the core writes back for the shell parses again, the same.
    const auto again = mv::addon::parse_keymaps_text(mv::addon::keymaps_to_json(*packs));
    if (!again || again->size() != packs->size()) std::abort();
  }

  const auto theme = mv::addon::parse_theme(text);
  if (theme.ok()) {
    const auto& s = theme.t.shape;
    if (s.radius < mv::addon::kShapeRadiusMin || s.radius > mv::addon::kShapeRadiusMax ||
        s.density < mv::addon::kShapeDensityMin || s.density > mv::addon::kShapeDensityMax ||
        s.type_scale < mv::addon::kShapeTypeMin || s.type_scale > mv::addon::kShapeTypeMax) {
      std::abort();
    }
    if (!mv::json::parse(mv::addon::theme_to_json(theme.t))) std::abort();
  }
  return 0;
}
