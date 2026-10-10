// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Copy Text in Image (Preview's Edit > Copy Text in Image; docs/design/16 View,
// docs/design/12 2026-10-10): the still on screen read by the OS's own
// on-device text recognition, the text put on the clipboard, a notice saying
// how many words.
//
// The recogniser is a port (D9): Vision on the Mac
// (shell/text_recognition_mac.mm), Windows.Media.Ocr in the WinUI chrome on
// Windows (IslandHost.TextRecognition.cs). Neither is reachable from image/ or
// edit/. Everything else -- the pixels the recogniser is handed, the tidy-up,
// the word count and the notice -- is here, shared and tested. Nothing leaves
// the machine (rule 6): both recognisers run on device, and no text or pixel
// is logged.
//
// Worker thread only, except the pure helpers at the bottom.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "edit/edit_stack.h"

namespace mv::shell {

// The long edge the recogniser is handed. Both OCR engines read body text at
// this size; a 50 MP frame would only cost decode memory and recognition time
// (Windows' OcrEngine.MaxImageDimension is also well above it).
inline constexpr std::uint32_t kTextImageMaxEdge = 4096;

enum class text_pixel_order : std::uint8_t { rgba, bgra };

// What the recogniser reads: opaque 8-bit pixels, row stride width * 4. A
// transparent area is flattened over white, so dark text on a transparent
// PNG stays readable (alpha is 255 everywhere).
struct text_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
};

// The still as the canvas shows its geometry -- oriented, rotated, flipped,
// cropped, straightened (`g` is edit_session's export geometry) -- scaled
// down to `max_edge` on the long side if it is larger. Colour adjusts are
// not applied: they change no letter. Reads and decodes the file.
[[nodiscard]] result<text_image> render_text_image(std::string_view source_path,
                                                   const edit::geometry& g,
                                                   text_pixel_order order = text_pixel_order::rgba,
                                                   std::uint32_t max_edge = kTextImageMaxEdge);

enum class text_recognition : std::uint8_t {
  ok = 0,       // ran; `text` may be empty (nothing found)
  no_language,  // Windows: no OCR language is installed for the user's languages
  unavailable,  // the OS (or an old chrome) has no recogniser
  failed,       // the recogniser ran and failed
};

// The port each host implements with its OS API. `text` is the lines in
// reading order, one per line. Called on a worker thread; it may block
// that thread (it is never the UI or render thread).
class text_recognizer {
 public:
  virtual ~text_recognizer() = default;
  [[nodiscard]] virtual text_recognition recognise(const text_image& image,
                                                   std::string& text) noexcept = 0;
};

struct copied_text {
  text_recognition outcome = text_recognition::failed;
  bool unreadable = false;  // the file could not be read or decoded
  std::string text;         // tidy_text()'d; empty when nothing was found
  std::size_t words = 0;
};

// The whole job: render, recognise, tidy, count. Worker thread only.
[[nodiscard]] copied_text run_copy_text(std::string_view source_path, const edit::geometry& g,
                                        text_recognizer& recognizer,
                                        text_pixel_order order = text_pixel_order::rgba);

// Pure. CR LF and CR become LF; spaces and tabs at the end of a line and blank
// lines at either end are dropped. Blank lines between paragraphs stay.
[[nodiscard]] std::string tidy_text(std::string_view utf8);

// Pure. Runs of anything but ASCII whitespace (space, tab, CR, LF, FF, VT) and
// U+00A0 / U+3000. A line of CJK with no spaces counts as one word.
[[nodiscard]] std::size_t count_words(std::string_view utf8) noexcept;

// Pure. "Copied 42 words", "Copied 1 word", "No text found", or why nothing
// was read. The host shows it in its notice line.
[[nodiscard]] std::string copy_text_notice(const copied_text& result);

}  // namespace mv::shell
