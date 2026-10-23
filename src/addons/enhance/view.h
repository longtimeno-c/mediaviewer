// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Models page (#297, docs/plans/enhance.md E1) as a view tree
// (docs/design/25 §10): Settings -> Add-ons -> Enhance, drawn by both hosts with
// their own controls. This machine in one line, then per kind its models,
// largest first, each a card: what it is, what it downloads and needs, whether
// it fits (or how its download stands), and the buttons that apply. Every
// control carries a label (screen.h). Pure: the caller passes the catalogue,
// this machine and the store's status; nothing here touches the disk, and no
// path appears in a tree.
//
// Node ids carry the clicks: "dl.<kind>.<model>", "cancel.<kind>",
// "resume.<kind>", "delete.<kind>" (action_of reads them back).
//
// Sizes: downloads and disk in decimal units, as src.managed/Shared/ByteSize.cs
// writes them ("412 MB of 1.08 GB"); memory in the binary units it is sold in,
// written GB ("16 GB of memory", "12 GB" of VRAM), so a 16 GB Mac reads 16.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "addons/enhance/catalog.h"
#include "addons/enhance/downloads.h"
#include "addons/enhance/machine.h"

namespace mv::enhance {

// ByteSize.Text, ported rule for rule: "850 bytes", "8.42 MB", "42.1 MB",
// "412 MB", "1.08 GB" (the decimal point is always '.').
[[nodiscard]] std::string size_text(std::uint64_t bytes);
// "16 GB", "2.1 GB", "256 MB": bytes / 2^30 (2^20), whole from 10 GB.
[[nodiscard]] std::string memory_text(std::uint64_t bytes);

// "This Mac: Apple M2 Pro, 32 GB of memory." and the like.
[[nodiscard]] std::string machine_line(const machine& here);

// The settings screen "models": the canonical tree (a stack at the root).
[[nodiscard]] std::string models_tree(const catalog& c, const machine& here, std::span<const kind_status> status);

// When only bytes moved (each kind's model and state as before), the diff
// that sets the progress bars and their lines ("[]" when nothing changed);
// empty when the tree's shape changed and the whole tree must be posted.
[[nodiscard]] std::string progress_diff(std::span<const kind_status> before, std::span<const kind_status> now);

enum class verb : std::uint8_t { none, download, cancel, resume, remove };

struct action {
  verb what = verb::none;
  kind k = kind::still;
  std::string model;  // download
};

// A clicked node id back to what it asks; verb::none for any other id.
[[nodiscard]] action action_of(std::string_view node_id);

}  // namespace mv::enhance
