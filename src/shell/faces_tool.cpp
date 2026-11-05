// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/faces_tool.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "core/json.h"
#include "edit/eyes.h"
#include "image/half.h"
#include "shell/canvas_tool.h"

namespace mv::shell {
namespace {

float json_number(const json::value& v, const char* key, float fallback) {
  const json::value* n = v.find(key);
  if (!n || n->k != json::kind::number) return fallback;
  return static_cast<float>(n->is_integer ? static_cast<double>(n->i) : n->d);
}

std::uint8_t to_srgb8(float c) noexcept {
  c = std::clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
  const float e = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
  return static_cast<std::uint8_t>(std::lround(e * 255.0f));
}

// A square about the face (1.6 times its larger side, held inside the picture
// where it can be), box-averaged to `px` a side, linear → sRGB.
void make_thumb(face_card& c, const image::linear_image& img, std::uint32_t px) {
  if (!img.valid() || px == 0) return;
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  const float cx = (c.box[0] + c.box[2] * 0.5f) * w, cy = (c.box[1] + c.box[3] * 0.45f) * h;
  float side = std::max(c.box[2] * w, c.box[3] * h) * 1.6f;
  side = std::clamp(side, 4.0f, std::min(w, h));
  const float x0 = std::clamp(cx - side * 0.5f, 0.0f, w - side), y0 = std::clamp(cy - side * 0.5f, 0.0f, h - side);
  c.thumb_w = c.thumb_h = px;
  c.thumb.assign(std::size_t{px} * px * 4, 255);
  const float step = side / static_cast<float>(px);
  // Up to 4 x 4 samples a thumbnail pixel: a face is a few hundred pixels.
  const int taps = std::clamp(static_cast<int>(std::ceil(step)), 1, 4);
  for (std::uint32_t y = 0; y < px; ++y) {
    for (std::uint32_t x = 0; x < px; ++x) {
      float r = 0, g = 0, b = 0;
      for (int ty = 0; ty < taps; ++ty) {
        for (int tx = 0; tx < taps; ++tx) {
          const float sx = x0 + (static_cast<float>(x) + (static_cast<float>(tx) + 0.5f) / static_cast<float>(taps)) * step;
          const float sy = y0 + (static_cast<float>(y) + (static_cast<float>(ty) + 0.5f) / static_cast<float>(taps)) * step;
          const auto ix = static_cast<std::size_t>(std::clamp(static_cast<int>(sx), 0, static_cast<int>(img.width) - 1));
          const auto iy = static_cast<std::size_t>(std::clamp(static_cast<int>(sy), 0, static_cast<int>(img.height) - 1));
          const std::uint16_t* p = img.rgba.data() + (iy * img.width + ix) * 4;
          r += image::half_to_float(p[0]);
          g += image::half_to_float(p[1]);
          b += image::half_to_float(p[2]);
        }
      }
      const float n = static_cast<float>(taps * taps);
      std::uint8_t* o = c.thumb.data() + (std::size_t{y} * px + x) * 4;
      o[0] = to_srgb8(r / n);
      o[1] = to_srgb8(g / n);
      o[2] = to_srgb8(b / n);
    }
  }
}

}  // namespace

std::uint32_t actions_of(const face_card& f) noexcept {
  std::uint32_t bits = 0;
  for (const face_action_row& row : kFaceActions) {
    if (row.needs_shut && !f.shut) continue;
    bits |= 1u << static_cast<unsigned>(row.id);
  }
  return bits;
}

faces_scan read_faces(status pack, std::string_view pack_json, const image::linear_image* still,
                      std::uint32_t thumb_px) {
  faces_scan out;
  if (pack == status::not_found) {
    out.status = faces_status::no_pack;
    return out;
  }
  if (pack == status::invalid_arg) {
    out.status = faces_status::people_off;
    return out;
  }
  if (pack != status::ok) return out;
  auto doc = json::parse(pack_json);
  const json::value* faces = doc ? doc->find("faces") : nullptr;
  if (!faces || faces->k != json::kind::array) return out;
  out.status = faces_status::ready;
  const bool have = still && still->valid();
  const float w = have ? static_cast<float>(still->width) : 0.0f, h = have ? static_cast<float>(still->height) : 0.0f;
  for (const json::value& f : faces->a) {
    if (out.faces.size() >= static_cast<std::size_t>(kToolFaces)) break;
    face_card c;
    const json::value* lm = f.find("lm");
    if (!lm || lm->k != json::kind::array || lm->a.size() != c.lm.size()) continue;
    for (std::size_t i = 0; i < c.lm.size(); ++i) c.lm[i] = static_cast<float>(lm->a[i].d);
    c.box = {json_number(f, "x", 0.0f), json_number(f, "y", 0.0f), json_number(f, "w", 0.0f), json_number(f, "h", 0.0f)};
    if (!(c.box[2] > 0.0f && c.box[3] > 0.0f)) continue;
    c.region = edit::face_region(c.box, c.lm, w, h);
    c.person = f.integer("person").value_or(0);
    if (const std::string* n = f.str("name")) c.name = *n;
    c.photos = static_cast<std::uint32_t>(std::max<std::int64_t>(0, f.integer("photos").value_or(0)));
    if (have) {
      c.shut = edit::eyes_look_shut(*still, c.lm);
      make_thumb(c, *still, thumb_px);
    }
    out.faces.push_back(std::move(c));
  }
  return out;
}

void faces_tool::looking(std::uint64_t photo) noexcept {
  item = photo;
  status = faces_status::looking;
  faces.clear();
  chosen = 0;
  hover = -1;
  ++revision;
}

bool faces_tool::landed(std::uint64_t photo, faces_scan scan) {
  if (photo != item) return false;
  status = scan.status;
  faces = std::move(scan.faces);
  chosen = faces.empty() ? 0u : (faces.size() >= 32 ? ~0u : (1u << faces.size()) - 1u);
  hover = -1;
  ++revision;
  return true;
}

void faces_tool::clear() noexcept {
  item = 0;
  status = faces_status::idle;
  faces.clear();
  chosen = 0;
  hover = -1;
  ++revision;
}

bool faces_tool::needs_read(std::uint64_t photo) const noexcept {
  return item != photo || (status != faces_status::looking && status != faces_status::ready);
}

void faces_tool::start_run(int face, int action) noexcept {
  running = true;
  running_face = face;
  running_action = action;
  run_item = item;
  ++revision;
}

void faces_tool::run_done() noexcept {
  running = false;
  run_item = 0;
  ++revision;
}

bool faces_tool::busy_on(int i) const noexcept {
  return running_here() && (running_face == i || (running_face < 0 && is_chosen(i)));
}

int faces_tool::chosen_count() const noexcept {
  const std::uint32_t live = faces.size() >= 32 ? ~0u : (1u << faces.size()) - 1u;
  return std::popcount(chosen & live);
}

bool faces_tool::toggle(int i) noexcept {
  if (i < 0 || i >= count()) return false;
  chosen ^= 1u << static_cast<unsigned>(i);
  ++revision;
  return true;
}

void faces_tool::choose_all() noexcept {
  chosen = faces.empty() ? 0u : (faces.size() >= 32 ? ~0u : (1u << faces.size()) - 1u);
  ++revision;
}

void faces_tool::choose_none() noexcept {
  chosen = 0;
  ++revision;
}

bool faces_tool::set_hover(int i) noexcept {
  if (i < -1 || i >= count()) i = -1;
  if (i == hover) return false;
  hover = i;  // the canvas's alone: the panel does not show it (no revision)
  return true;
}

std::vector<float> faces_tool::run_boxes(int face) const {
  std::vector<float> out;
  for (int i = 0; i < count(); ++i) {
    if (face >= 0 ? i != face : !is_chosen(i)) continue;
    const auto& b = faces[static_cast<std::size_t>(i)].box;
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}

std::array<float, 4> region_to_shown(const edit::placement& p, const std::array<float, 4>& r) noexcept {
  const edit::affine inv = edit::invert(p.map);
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  for (int k = 0; k < 4; ++k) {
    const float sx = r[0] + ((k & 1) ? r[2] : 0.0f), sy = r[1] + ((k & 2) ? r[3] : 0.0f);
    const float u = inv.m[0] * sx + inv.m[1] * sy + inv.m[2];
    const float v = inv.m[3] * sx + inv.m[4] * sy + inv.m[5];
    if (k == 0) {
      x0 = x1 = u;
      y0 = y1 = v;
    } else {
      x0 = std::min(x0, u), x1 = std::max(x1, u);
      y0 = std::min(y0, v), y1 = std::max(y1, v);
    }
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

int face_at(const faces_tool& t, const edit::placement& p, float u, float v) noexcept {
  int best = -1;
  float best_area = 0.0f;
  for (int i = 0; i < t.count(); ++i) {
    const auto r = region_to_shown(p, t.faces[static_cast<std::size_t>(i)].region);
    if (u < r[0] || u > r[0] + r[2] || v < r[1] || v > r[1] + r[3]) continue;
    const float area = r[2] * r[3];
    if (best < 0 || area < best_area) {
      best = i;
      best_area = area;
    }
  }
  return best;
}

void apply_faces(edit_view& v, const faces_tool& t, const edit::placement& p) noexcept {
  v.tool_face_count = 0;
  v.tool_face_chosen = 0;
  v.tool_face_busy = 0;
  v.tool_face_hover = -1;
  if (v.tool != static_cast<std::uint8_t>(canvas_tool::faces)) return;
  const int n = std::min(t.count(), kToolFaces);
  for (int i = 0; i < n; ++i) {
    const face_card& f = t.faces[static_cast<std::size_t>(i)];
    const auto r = region_to_shown(p, f.region);
    for (int k = 0; k < 4; ++k) v.tool_faces[i][k] = r[static_cast<std::size_t>(k)];
    // The name, cut at a whole UTF-8 character.
    std::size_t len = std::min<std::size_t>(f.name.size(), kToolFaceName - 1);
    while (len > 0 && len < f.name.size() && (static_cast<unsigned char>(f.name[len]) & 0xC0u) == 0x80u) --len;
    std::memcpy(v.tool_face_names[i], f.name.data(), len);
    v.tool_face_names[i][len] = '\0';
    if (t.is_chosen(i)) v.tool_face_chosen |= 1u << static_cast<unsigned>(i);
    if (t.busy_on(i)) v.tool_face_busy |= 1u << static_cast<unsigned>(i);
  }
  v.tool_face_count = static_cast<std::uint32_t>(n);
  v.tool_face_hover = static_cast<std::int8_t>(t.hover >= 0 && t.hover < n ? t.hover : -1);
}

std::string faces_status_line(const faces_tool& t) {
  switch (t.status) {
    case faces_status::idle:
    case faces_status::looking: return "Looking for faces…";
    case faces_status::no_pack: return "Faces needs Local search";
    case faces_status::people_off: return "Faces needs People in Local search";
    case faces_status::failed: return "Could not read the faces of this photo";
    case faces_status::ready: break;
  }
  if (t.faces.empty()) return "No faces found";
  std::string s = t.count() == 1 ? "1 face" : std::to_string(t.count()) + " faces";
  s += " · " + std::to_string(t.chosen_count()) + " chosen";
  return s;
}

std::string face_source_line(const face_card& f) {
  if (f.person == 0 || f.photos == 0) return {};
  std::string s = "from " + (f.photos == 1 ? std::string("1 photo") : std::to_string(f.photos) + " photos");
  s += f.name.empty() ? " of this person" : " of " + f.name;
  return s;
}

}  // namespace mv::shell
