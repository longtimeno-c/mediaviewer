// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #288 Adjust Size (macOS Preview's Tools ▸ Adjust Size…): the size and unit
// maths both hosts' sheets drive, and the stated output resolution the stack
// carries (docs/design/07 "Adjust Size").
//
// Portable and header-only: no platform include, no I/O, no allocation.
// Nothing here touches pixels; `edit_stack.h` turns a finished sheet into one
// `resize` op (`adjust_size_spec`), and the encoders write the resolution.
//
// The rules, stated once:
//   • Units. Width and height in pixels, percent of the size the sheet opened
//     on, inches, centimetres, millimetres or points (1/72 in). Resolution in
//     pixels per inch or per centimetre. One resolution for both axes
//     (square pixels), as Preview's single Resolution field.
//   • Unrounded state. Width and height are kept as doubles; only `output()`
//     rounds (half away from zero, at least 1, at most kMaxPixels). So a
//     round-trip through any unit, or a lock that follows the other side,
//     never drifts by a pixel per edit.
//   • Scale proportionally. The followed side is derived from the *opened*
//     aspect (original.h / original.w), never from the other side's rounded
//     value: 4000×3000 at width 1001 gives 750.75 → 751.
//   • Resample on: pixels change. A physical size is converted at the current
//     resolution; changing the resolution keeps the physical size, so the
//     pixel count follows it (72 → 144 ppi doubles both sides).
//   • Resample off: the pixels are the opened ones and do not change. A
//     physical width or height sets the resolution that makes the picture
//     that big (3000 px at 10 in → 300 ppi); a percent scales the physical
//     size (50 % → twice the resolution); a pixel value is refused. Turning
//     resample off puts the pixels back to the opened size.
//   • Fit into. Scales to fit inside the box with the ratio kept, the box as
//     given (a portrait picture fits a landscape box, it is not turned). It
//     enlarges a smaller picture by default — Preview's "Fit into" scales up
//     or down to the box (AppleInsider's description of it; not verified on a
//     Mac here) — and `enlarge = false` caps the scale at 1. Fit into turns
//     resample and the lock on; the resolution stays, so the physical size
//     follows the pixels.
#pragma once

#include <cstdint>

namespace mv::edit {

struct size2 {
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  friend constexpr bool operator==(const size2&, const size2&) = default;
};

enum class size_unit : std::uint8_t {
  pixels = 0,
  percent,  // of the size the sheet opened on
  inches,
  cm,
  mm,
  points,   // 1/72 inch
};

enum class resolution_unit : std::uint8_t { per_inch = 0, per_cm = 1 };

inline constexpr double kCmPerInch = 2.54;
inline constexpr double kMmPerInch = 25.4;
inline constexpr double kPointsPerInch = 72.0;
inline constexpr double kMetresPerInch = 0.0254;
// What the sheet shows for a source that states no resolution (Preview's and
// ImageIO's 72). Shown only: a file that stated none is still written with
// none unless the user sets one.
inline constexpr double kDefaultPpi = 72.0;
inline constexpr double kMinPpi = 1.0;
inline constexpr double kMaxPpi = 65535.0;    // JFIF's 16-bit density, per inch
inline constexpr std::uint32_t kMaxPixels = 65500;  // edit/encode.h's ceiling

// A stated output resolution. `ppi` 0 = none stated. `unit` is how the user
// gave it, and so how JFIF and EXIF write it (dots/inch or dots/cm).
struct resolution {
  double ppi = 0.0;
  resolution_unit unit = resolution_unit::per_inch;
  [[nodiscard]] constexpr bool stated() const noexcept { return ppi > 0.0; }
  friend constexpr bool operator==(const resolution&, const resolution&) = default;
};

[[nodiscard]] constexpr bool is_physical(size_unit u) noexcept {
  return u == size_unit::inches || u == size_unit::cm || u == size_unit::mm ||
         u == size_unit::points;
}

// Inches in one `u` (physical units only; 0 for pixels and percent).
[[nodiscard]] constexpr double inches_per(size_unit u) noexcept {
  switch (u) {
    case size_unit::inches: return 1.0;
    case size_unit::cm: return 1.0 / kCmPerInch;
    case size_unit::mm: return 1.0 / kMmPerInch;
    case size_unit::points: return 1.0 / kPointsPerInch;
    default: return 0.0;
  }
}

[[nodiscard]] constexpr double ppi_from(double value, resolution_unit u) noexcept {
  return u == resolution_unit::per_cm ? value * kCmPerInch : value;
}
[[nodiscard]] constexpr double resolution_in(double ppi, resolution_unit u) noexcept {
  return u == resolution_unit::per_cm ? ppi / kCmPerInch : ppi;
}

[[nodiscard]] constexpr double clamp_ppi(double ppi) noexcept {
  if (!(ppi >= kMinPpi)) return kMinPpi;  // also NaN
  return ppi > kMaxPpi ? kMaxPpi : ppi;
}

// `value` in `u` → pixels, at `ppi`; `original_px` is what percent is of.
[[nodiscard]] constexpr double to_pixels(double value, size_unit u, double ppi,
                                         double original_px) noexcept {
  if (u == size_unit::pixels) return value;
  if (u == size_unit::percent) return original_px * value / 100.0;
  return value * inches_per(u) * ppi;
}
[[nodiscard]] constexpr double from_pixels(double px, size_unit u, double ppi,
                                           double original_px) noexcept {
  if (u == size_unit::pixels) return px;
  if (u == size_unit::percent) return original_px > 0 ? px * 100.0 / original_px : 0.0;
  const double per = inches_per(u) * ppi;
  return per > 0 ? px / per : 0.0;
}

// Half away from zero (values are positive), at least 1, at most kMaxPixels.
[[nodiscard]] constexpr std::uint32_t round_pixels(double v) noexcept {
  if (!(v >= 1.0)) return 1;  // also NaN
  if (v >= static_cast<double>(kMaxPixels)) return kMaxPixels;
  return static_cast<std::uint32_t>(v + 0.5);
}

// PNG pHYs is pixels per metre, an integer: 300 ppi is 11811 /m, which reads
// back as 299.9994. One step of the integer is 0.0254 ppi, so when a whole
// ppi lies within half a step (it would have been written as this value) it
// reads back as that ppi: 11811 → 300, 5906 → 150.
[[nodiscard]] constexpr std::uint32_t per_metre(double ppi) noexcept {
  const double m = ppi / kMetresPerInch;
  return m < 1.0 ? 1u : static_cast<std::uint32_t>(m + 0.5);
}
[[nodiscard]] constexpr double ppi_from_per_metre(std::uint32_t per_m) noexcept {
  const double ppi = per_m * kMetresPerInch;
  const double whole = static_cast<double>(static_cast<std::uint64_t>(ppi + 0.5));
  const double d = ppi - whole;
  constexpr double kHalfStep = kMetresPerInch / 2;
  return (d <= kHalfStep && d >= -kHalfStep) ? whole : ppi;
}

// The Fit into menu (Preview's list as the issue gives it); Custom is the
// host's own row. Each is a box: width × height.
inline constexpr size2 kFitPresets[] = {
    {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};

// The sheet's state. Open it on the size the stack outputs now (placement::
// output) and the resolution in force (the stack's stated one, else the
// source's, else none → shown as kDefaultPpi).
struct adjust_size {
  size2 original{};
  double original_ppi = kDefaultPpi;
  double width = 0.0;   // pixels, unrounded
  double height = 0.0;
  double ppi = kDefaultPpi;
  resolution_unit unit = resolution_unit::per_inch;  // how the resolution was given/shown
  bool proportional = true;
  bool resample = true;

  [[nodiscard]] static constexpr adjust_size open(size2 current, resolution in_force) noexcept {
    adjust_size s;
    s.original = size2{current.w ? current.w : 1u, current.h ? current.h : 1u};
    s.original_ppi = in_force.stated() ? clamp_ppi(in_force.ppi) : kDefaultPpi;
    s.ppi = s.original_ppi;
    s.unit = in_force.unit;
    s.width = s.original.w;
    s.height = s.original.h;
    return s;
  }

  // Width or height typed in `u`. False (and nothing changes) for a value
  // that is not a positive finite number, or a pixel value with resample off.
  constexpr bool set_width(double value, size_unit u) noexcept { return set_side(value, u, true); }
  constexpr bool set_height(double value, size_unit u) noexcept { return set_side(value, u, false); }

  // Resolution typed in `u`. Resample on keeps the physical size (the pixels
  // follow); off keeps the pixels (the physical size follows).
  constexpr bool set_resolution(double value, resolution_unit u) noexcept {
    if (!(value > 0.0) || value > 1e9) return false;
    const double next = clamp_ppi(ppi_from(value, u));
    if (resample) {
      width = width * next / ppi;
      height = height * next / ppi;
    }
    ppi = next;
    unit = u;
    return true;
  }

  // Turning the lock on re-derives the height from the width.
  constexpr void set_proportional(bool on) noexcept {
    proportional = on;
    if (on) height = width * original.h / original.w;
  }

  // Off puts the pixels back to the opened size: they cannot change.
  constexpr void set_resample(bool on) noexcept {
    resample = on;
    if (!on) {
      width = original.w;
      height = original.h;
    }
  }

  // Fit inside `box`, ratio kept (see the header comment for enlarging).
  constexpr bool fit_into(size2 box, bool enlarge = true) noexcept {
    if (box.w == 0 || box.h == 0) return false;
    const double sx = static_cast<double>(box.w) / original.w;
    const double sy = static_cast<double>(box.h) / original.h;
    double s = sx < sy ? sx : sy;
    if (!enlarge && s > 1.0) s = 1.0;
    resample = true;
    proportional = true;
    width = original.w * s;
    height = original.h * s;
    return true;
  }

  // ---- readouts ----
  [[nodiscard]] constexpr size2 output() const noexcept {
    return size2{round_pixels(width), round_pixels(height)};
  }
  // Width / height in `u` at the current resolution. Percent is of the
  // opened pixels (with resample off, of the opened physical size).
  [[nodiscard]] constexpr double width_in(size_unit u) const noexcept { return side_in(width, original.w, u); }
  [[nodiscard]] constexpr double height_in(size_unit u) const noexcept { return side_in(height, original.h, u); }
  [[nodiscard]] constexpr double resolution_value(resolution_unit u) const noexcept {
    return resolution_in(ppi, u);
  }
  // "Resulting size": the output's pixel count as a percent of the opened one.
  [[nodiscard]] constexpr double percent_of_original() const noexcept {
    const size2 o = output();
    return 100.0 * (static_cast<double>(o.w) * o.h) /
           (static_cast<double>(original.w) * original.h);
  }
  [[nodiscard]] constexpr bool changes_pixels() const noexcept { return !(output() == original); }
  [[nodiscard]] constexpr bool changes_resolution() const noexcept { return ppi != original_ppi; }

 private:
  constexpr bool set_side(double value, size_unit u, bool is_width) noexcept {
    if (!(value > 0.0) || value > 1e9) return false;
    const double orig_side = is_width ? original.w : original.h;
    if (!resample) {
      if (u == size_unit::pixels) return false;
      if (u == size_unit::percent) {
        ppi = clamp_ppi(original_ppi * 100.0 / value);
        return true;
      }
      ppi = clamp_ppi(orig_side / (value * inches_per(u)));
      return true;
    }
    const double px = to_pixels(value, u, ppi, orig_side);
    if (is_width) {
      width = px;
      if (proportional) height = px * original.h / original.w;
    } else {
      height = px;
      if (proportional) width = px * original.w / original.h;
    }
    return true;
  }
  [[nodiscard]] constexpr double side_in(double px, double orig_side, size_unit u) const noexcept {
    if (u == size_unit::percent && !resample) return original_ppi * 100.0 / ppi;
    return from_pixels(px, u, ppi, orig_side);
  }
};

// Preview's "N MB (was M MB)": the file's bytes per pixel times the output's
// pixel count. A proportional estimate, stated as one: real JPEG/PNG sizes are
// not linear in pixels (a smaller picture costs more per pixel, an enlarged
// one less, as its detail does not grow), and a different export format or
// quality changes them. With no pixel change it is the file's own size.
[[nodiscard]] constexpr std::uint64_t estimated_bytes(std::uint64_t file_bytes, size2 file_pixels,
                                                      size2 output) noexcept {
  if (file_pixels.w == 0 || file_pixels.h == 0) return 0;
  if (output == file_pixels) return file_bytes;
  const double bpp = static_cast<double>(file_bytes) /
                     (static_cast<double>(file_pixels.w) * file_pixels.h);
  return static_cast<std::uint64_t>(bpp * output.w * output.h + 0.5);
}

}  // namespace mv::edit
