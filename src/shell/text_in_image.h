// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text in images (Live Text, as in Photos; docs/design/16 "Text in images",
// docs/design/12 2026-10-10 and 2026-10-15): the still on screen is read by the
// OS's own on-device text recognition once it has settled under the pointer;
// a drag over its text selects it and Copy puts it on the clipboard.
//
// The recogniser is a port (D9): Vision on the Mac
// (shell/text_recognition_mac.mm), Windows.Media.Ocr in the WinUI chrome on
// Windows (IslandHost.TextRecognition.cs). Neither is reachable from image/ or
// edit/. Everything else -- the pixels the recogniser is handed, the tidy-up,
// the word count and the notice -- is here, shared and tested. Nothing leaves
// the machine (rule 6): both recognisers run on device, and no text or pixel
// is logged.
//
// run_text_recognition is worker-thread only; everything else is pure.
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
  ok = 0,       // ran; the layout may be empty (nothing found)
  no_language,  // Windows: no OCR language is installed for the user's languages
  unavailable,  // the OS (or an old chrome) has no recogniser
  failed,       // the recogniser ran and failed
};

// One word the recogniser read. Its box is in the text image -- the still as
// the canvas shows it -- as fractions of that image's width and height, top
// left origin, so it does not depend on the size the recogniser was handed.
struct text_word {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  std::uint32_t line = 0;  // the line it is on, counted in reading order
  std::string text;        // no whitespace inside
};

// Everything read from one still, words in reading order (line by line, each
// line left to right as the recogniser gave it). Lines never interleave.
struct text_layout {
  std::vector<text_word> words;
  [[nodiscard]] bool empty() const noexcept { return words.empty(); }
};

// The port each host implements with its OS API. Called on a worker thread;
// it may block that thread (it is never the UI or render thread).
class text_recognizer {
 public:
  virtual ~text_recognizer() = default;
  [[nodiscard]] virtual text_recognition recognise(const text_image& image,
                                                   text_layout& layout) noexcept = 0;
};

struct read_text {
  text_recognition outcome = text_recognition::failed;
  bool unreadable = false;  // the file could not be read or decoded
  text_layout layout;       // empty unless outcome is ok and something was read
};

// The whole job: render, recognise. Worker thread only.
[[nodiscard]] read_text run_text_recognition(std::string_view source_path, const edit::geometry& g,
                                             text_recognizer& recognizer,
                                             text_pixel_order order = text_pixel_order::rgba);

// Pure. Appends `line`'s words to `layout`, splitting it at whitespace and
// giving each word a share of `box` by its length in code points. For a
// recogniser that boxes a line but not its words.
void add_line_split_evenly(text_layout& layout, std::uint32_t line, std::string_view utf8,
                           float x0, float y0, float x1, float y1);

// Pure. The chrome's wire form on Windows (IslandHost.RecognizeText): one
// word per line, "line<TAB>left<TAB>top<TAB>width<TAB>height<TAB>text" with the
// box in pixels of an image `width` x `height`. A malformed line is skipped.
[[nodiscard]] text_layout parse_word_records(std::string_view utf8, std::uint32_t width,
                                             std::uint32_t height);

// --- Selecting text on the canvas, as Photos does ---------------------------
// A drag that starts on a word selects from it to the word under the pointer
// (whole words, reading order); a double-click selects the word, a triple
// click the line. Points are in the same fractions of the text image.

// The word under (u, v), -1 when there is none. A box is grown by a quarter of
// its height so a thin line of type is easy to hit. `aspect` is the image's
// width / height, so the growth is the same on screen both ways.
[[nodiscard]] int word_at(const text_layout& layout, float u, float v, float aspect) noexcept;

// The word a drag that has left the text is extending to: on the line whose
// band is nearest to v, the word nearest to u. -1 only for an empty layout.
[[nodiscard]] int nearest_word(const text_layout& layout, float u, float v) noexcept;

// The first and last word of `word`'s line.
void line_bounds(const text_layout& layout, int word, int& first, int& last) noexcept;

// Selected words, `anchor` to `focus` in either order; both -1 when nothing
// is selected.
struct text_selection {
  int anchor = -1;
  int focus = -1;
  [[nodiscard]] bool empty() const noexcept { return anchor < 0 || focus < 0; }
  [[nodiscard]] int first() const noexcept { return anchor < focus ? anchor : focus; }
  [[nodiscard]] int last() const noexcept { return anchor < focus ? focus : anchor; }
};

// The selected words as text: a space between words on one line, a newline
// between lines. Empty for an empty or out-of-range selection.
[[nodiscard]] std::string selected_text(const text_layout& layout, text_selection sel);

struct text_rect {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
};

// The highlight: one box per selected line, spanning its selected words.
void selection_rects(const text_layout& layout, text_selection sel, std::vector<text_rect>& out);

// Pure. CR LF and CR become LF; spaces and tabs at the end of a line and blank
// lines at either end are dropped. Blank lines between paragraphs stay.
[[nodiscard]] std::string tidy_text(std::string_view utf8);

// Pure. Runs of anything but ASCII whitespace (space, tab, CR, LF, FF, VT) and
// U+00A0 / U+3000. A line of CJK with no spaces counts as one word.
[[nodiscard]] std::size_t count_words(std::string_view utf8) noexcept;

// Pure. "Copied 42 words", "Copied 1 word". The host shows it in its notice
// line when the selection goes on the clipboard.
[[nodiscard]] std::string copy_text_notice(std::size_t words);

// Pure. Why nothing can be selected, for a notice; empty when there is nothing
// to say (the recogniser ran, or simply failed on this one image).
[[nodiscard]] std::string text_recognition_notice(const read_text& result);

}  // namespace mv::shell
