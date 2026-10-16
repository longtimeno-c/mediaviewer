// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text layout shared by the document formats codec/ draws itself (DOCX today;
// a PPTX text body is the same model in a box): a paragraph of styled spans,
// broken into lines over a `frame` that says where each line may go, reordered
// right-to-left by a compact Unicode bidi subset, and a display list of words,
// rules, pictures and shapes painted onto a page.
//
// Everything is in points. Where a line breaks depends only on the text and
// the frame, never on the drawing scale.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "codec/raster.h"
#include "codec/text.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::codec::doclayout {

// ---- what a page draws -------------------------------------------------------------

enum class geometry : std::uint8_t { rect, round_rect, ellipse, line };

struct shape_style {
  geometry geom = geometry::rect;
  bool filled = false;
  std::uint32_t fill = 0xFFFFFF;
  double fill_alpha = 1.0;
  bool stroked = false;
  std::uint32_t stroke = 0x000000;
  double stroke_w = 0.75;  // points
  double radius = 0.1667;  // round_rect: corner radius over the shorter side
  bool flip_h = false, flip_v = false;  // line: which diagonal
};

// Layers paint in order; inside a layer, by z and then in the order added.
enum class layer : std::uint8_t { behind, body, front };

enum class field : std::uint8_t { none, page, pages };

class display {
 public:
  struct word {
    text::shaped shaped;
    text::style style;
    double x = 0.0, baseline = 0.0;
    field f = field::none;  // a field drawn with the value known when painting
    double box_w = 0.0;     // the placeholder's advance (fields)
  };
  struct rect {
    double x, y, w, h;
    std::uint32_t rgb;
  };
  struct image {
    std::shared_ptr<const raster> pixels;  // null: drawn as a grey box
    double x, y, w, h;
  };
  struct shape {
    shape_style s;
    double x, y, w, h;
  };

  void add(word w);
  void add(rect r);
  void add(image i);
  void add(shape s);

  // Where the next adds go.
  layer current = layer::body;
  std::int64_t z = 0;

  [[nodiscard]] bool empty() const noexcept { return order_.empty(); }

 private:
  friend status paint(const display&, text::engine&, raster&, double, const std::string&,
                      const std::string&, const job_context*);
  enum class kind : std::uint8_t { word, rect, image, shape };
  struct entry {
    kind k;
    layer l;
    std::int64_t z;
    std::uint32_t index;
  };
  std::vector<word> words_;
  std::vector<rect> rects_;
  std::vector<image> images_;
  std::vector<shape> shapes_;
  std::vector<entry> order_;
};

// Paints `d` onto `page` (RGBA8, opaque) at `scale` px/pt. Field words are
// reshaped with `page_label` / `pages_label`.
status paint(const display& d, text::engine& eng, raster& page, double scale,
             const std::string& page_label, const std::string& pages_label,
             const job_context* ctx);

// One shape, anti-aliased, into `page` (points, `scale` px/pt).
void draw_shape(raster& page, const shape_style& s, double x, double y, double w, double h,
                double scale);

// ---- the paragraph model ---------------------------------------------------------

enum class align : std::uint8_t { start, center, end, justify };  // start is right in RTL
enum class line_rule : std::uint8_t { automatic, exact, at_least };

struct span {
  enum kind : std::uint8_t { text, tab, line_break, column_break, page_break, object } k = text;
  std::string str;
  text::style style;
  double raise = 0.0;  // super/subscript shift, pt (up positive)
  bool rtl = false;    // a right-to-left run: its neutrals read right to left
  field f = field::none;
  int obj = -1;        // the owner's inline object (picture, shape) for `object`
  double obj_w = 0.0, obj_h = 0.0;
  int note = -1;       // a note this span references (placed with its line)
};

struct para {
  align jc = align::start;
  bool rtl = false;                   // paragraph direction
  double before = 0.0, after = 0.0;   // points
  double line = 240.0;                // 240ths for automatic, points otherwise
  line_rule rule = line_rule::automatic;
  double ind_start = 0.0, ind_end = 0.0, ind_first = 0.0;  // first < 0 hangs
  std::string label;                  // a list number or bullet
  text::style label_style;
  text::style mark;                   // the empty line's metrics
  std::vector<span> spans;
};

// ---- where lines go -----------------------------------------------------------------

struct segment {
  double x0 = 0.0, x1 = 0.0;
};
struct slot {
  double y = 0.0;
  segment seg[3];
  int count = 1;  // left to right
};

// What a paragraph is laid into: a body that breaks across columns and pages
// and wraps around floating objects, or a fixed box (a cell, a header, a text
// box, a slide's text body).
class frame {
 public:
  virtual ~frame() = default;
  // The first place at or below `y` where a line `h` tall referencing `notes`
  // fits, inset from the current column by `inset_l` / `inset_r`. May move to
  // the next column or page; the slot's y says where.
  virtual slot fit(double y, double h, double inset_l, double inset_r,
                   std::span<const int> notes) = 0;
  // A line was placed there.
  virtual void placed(double /*y*/, double /*h*/, std::span<const int> /*notes*/) {}
  // A column or page break after a line ending at `y`; returns the new y.
  virtual double hard_break(span::kind /*k*/, double y) { return y; }
  // Where drawing goes; null while only measuring.
  virtual display* out() = 0;
  // An inline object at its box.
  virtual void object(int /*id*/, double /*x*/, double /*y*/, double /*w*/, double /*h*/) {}
  // Whether `y` is the top of a fresh column (space before is dropped there).
  [[nodiscard]] virtual bool at_top(double /*y*/) const { return false; }
  // A field's text, or empty when it is known only when painting.
  virtual std::string field_text(field /*f*/) { return {}; }
  // The current column's span, for tables and pictures wider than it.
  virtual segment column() const = 0;
  [[nodiscard]] virtual double max_object_h() const { return 1e9; }
};

// A fixed column from x0 to x1 that grows downward without limit.
class box_frame : public frame {
 public:
  box_frame(double x0, double x1, display* out) : x0_(x0), x1_(x1), out_(out) {}
  slot fit(double y, double, double inset_l, double inset_r, std::span<const int>) override;
  void placed(double, double, std::span<const int> notes) override;
  display* out() override { return out_; }
  segment column() const override { return {x0_, x1_}; }
  // Notes referenced inside the box, in order.
  std::vector<int> notes;

 protected:
  double x0_, x1_;
  display* out_;
};

// Lays `p` out from `y`; returns the y after it, space after included.
double lay_paragraph(text::engine& eng, const para& p, frame& f, double y);

// ---- bidi -------------------------------------------------------------------------

enum class bidi_class : std::uint8_t { L, R, AL, EN, AN, WS, ON };

// The class of a run of text: its first strong direction, else digits, else
// white space or other neutral.
[[nodiscard]] bidi_class classify(std::string_view utf8) noexcept;

// The visual order (left to right) of a line's pieces in logical order, by
// the paragraph's base level (0 LTR, 1 RTL): weak types (W2, W3, W7), neutrals
// (N1, N2), implicit levels (I1, I2), trailing white space (L1) and reversal
// (L2). `levels` gets each piece's resolved level when not null.
[[nodiscard]] std::vector<std::size_t> visual_order(std::span<const bidi_class> cls, int base,
                                                    std::vector<int>* levels = nullptr);

// Splits text into words and runs of spaces; each CJK character is its own
// word, and a word is split where its direction changes (Arabic then digits).
[[nodiscard]] std::vector<std::string> tokens_of(const std::string& s);

}  // namespace mv::codec::doclayout
