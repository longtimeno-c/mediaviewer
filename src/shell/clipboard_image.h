// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// New from Clipboard (docs/design/16 "New from Clipboard", issue #289): the image on
// the clipboard opened as an unsaved item, as Preview's File > New from
// Clipboard does. Shared by both hosts; the hosts only read the clipboard
// (Windows: "PNG", CF_DIBV5, CF_DIB; Mac: public.png, public.tiff) on a worker
// and hand the bytes here.
//
// The item is the one io memory file (io/memory_file.h), "clipboard:<n>/Untitled",
// listed alone as a one-item list titled "Clipboard" -- the same list the
// search results and the Photos library use (docs/design/17, 26). Nothing is
// written to disk: the viewer, the edit stack and Save Copy read it from
// memory, every in-place write refuses it, and Save Copy asks for a folder.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "io/memory_file.h"

namespace mv::shell {

inline constexpr const char* kClipboardListTitle = "Clipboard";
inline constexpr const char* kClipboardItemName = "Untitled";  // Preview's name for it
inline constexpr const char* kNoClipboardImageNotice = "There is no image on the clipboard.";
// Any write in place (rating, tags, a lossless turn, Move, Trash) of the item.
inline constexpr const char* kUnsavedItemNotice = "Not saved yet: Save Copy\xE2\x80\xA6 writes it to a folder.";

enum class clipboard_flavor : std::uint8_t {
  file,        // an image file's bytes (PNG, TIFF, JPEG, ...)
  packed_dib,  // CF_DIB / CF_DIBV5: a BMP without its file header
};

// [any thread] Makes `bytes` the unsaved item: a packed DIB gets its BMP file
// header (codec::dib::bmp_from_packed), then the bytes must probe, by magic
// bytes, as a still the viewer decodes (D5: not a RAW, PDF or DOCX). Returns
// the item's key; unsupported_format when the clipboard held no such image,
// and the item before it is kept.
[[nodiscard]] result<std::string> adopt_clipboard_image(std::vector<std::uint8_t> bytes,
                                                       clipboard_flavor flavor);

// The list a New from Clipboard opened (its title), for the path row: it is
// not a search, so it reads "Clipboard", not "Search: Clipboard".
[[nodiscard]] inline bool is_clipboard_list(std::string_view list_title) noexcept {
  return list_title == kClipboardListTitle;
}

// Save Copy's file name for an unsaved item: "Untitled.jpg" / "Untitled.png".
[[nodiscard]] std::string unsaved_copy_name(std::string_view item_path, bool png);

}  // namespace mv::shell
