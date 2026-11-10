// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Unicode NFC for the names a phone sees (docs/plans/mobile.md N7): macOS over
// SMB, Windows and iOS can each hand back another form of one accented name,
// and matching is exact. Our own table (nfc_table.inc), so every OS gives the
// same bytes; an OS normaliser would differ by its Unicode version.
#pragma once

#include <string>
#include <string_view>

namespace mv::ai {

// `s` in NFC. Bytes that are not valid UTF-8 are not text: the whole string
// comes back unchanged, never altered and never dropped.
[[nodiscard]] std::string nfc(std::string_view s);

}  // namespace mv::ai
