// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/edit_session.h"

#include <algorithm>
#include <cmath>

#include "codec/format.h"
#include "edit/lossless_jpeg.h"
#include "io/collision_name.h"
#include "io/file.h"
#include "io/replace.h"
#include "meta/meta.h"

namespace mv::shell {
namespace {

using edit::op;
using edit::op_kind;

std::uint64_t fnv1a(std::uint64_t h, const void* data, std::size_t n) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// The shortest rotate/flip sequence for a D4 element.
std::vector<op> ops_for(codec::d4 g) {
  const op_kind turns[] = {op_kind::rotate_cw, op_kind::rotate_ccw, op_kind::flip_h, op_kind::flip_v};
  auto d4_of = [](op_kind k) {
    switch (k) {
      case op_kind::rotate_cw: return codec::kRotateCw;
      case op_kind::rotate_ccw: return codec::kRotateCcw;
      case op_kind::flip_h: return codec::kFlipH;
      default: return codec::kFlipV;
    }
  };
  if (g.identity()) return {};
  for (op_kind a : turns) {
    if (d4_of(a) == g) return {op{a}};
  }
  for (op_kind a : turns) {
    for (op_kind b : turns) {
      if (codec::compose(d4_of(a), d4_of(b)) == g) return {op{a}, op{b}};
    }
  }
  return {};  // unreachable: every element is at most two of these
}

bool same_rect(const edit::rect& a, const edit::rect& b) noexcept {
  constexpr float kEps = 1e-5f;
  return std::abs(a.x - b.x) < kEps && std::abs(a.y - b.y) < kEps && std::abs(a.w - b.w) < kEps &&
         std::abs(a.h - b.h) < kEps;
}

}  // namespace

std::uint64_t edit_session::key() const noexcept {
  std::uint64_t h = 1469598103934665603ull;
  h = fnv1a(h, item_.path.data(), item_.path.size());
  h = fnv1a(h, &item_.size, sizeof(item_.size));
  h = fnv1a(h, &item_.mtime, sizeof(item_.mtime));
  if (const auto it = epochs_.find(item_.path); it != epochs_.end()) {
    h = fnv1a(h, &it->second, sizeof(it->second));
  }
  return h;
}

edit::edit_stack& edit_session::current() {
  const std::uint64_t k = key();
  auto it = stacks_.find(k);
  if (it != stacks_.end()) return it->second;
  // Bound the session: drop the oldest stack that is not in use.
  while (stacks_.size() >= kCapacity && !order_.empty()) {
    const std::uint64_t victim = order_.front();
    order_.erase(order_.begin());
    if (victim == k || (in_flight_ && in_flight_->key == victim) ||
        (carry_ && carry_->old_key == victim)) {
      continue;
    }
    stacks_.erase(victim);
  }
  order_.push_back(k);
  edit::edit_stack& s = stacks_[k];
  s.source_id = k;
  return s;
}

const edit::edit_stack* edit_session::stack() const {
  if (!has_item_) return nullptr;
  const auto it = stacks_.find(key());
  return it == stacks_.end() ? nullptr : &it->second;
}

edit::size2 edit_session::frame() const noexcept {
  const edit::edit_stack* s = stack();
  const codec::d4 o = s ? edit::fold(*s).orient : codec::d4{};
  return o.transposes() ? edit::size2{item_.height, item_.width}
                        : edit::size2{item_.width, item_.height};
}

bool edit_session::wants_write() const {
  if (!has_item_ || !item_.jpeg || write_failed_ || crop_) return false;
  const edit::edit_stack* s = stack();
  if (!s) return false;
  const edit::geometry g = edit::fold(*s);
  // PR 11: a colour op makes the stack an export, never a file rewrite.
  return g.orientation_only() && !g.orient.identity() && edit::fold_colour(s->ops).identity();
}

bool edit_session::set_item(const edit_item& item) {
  const bool same_path = has_item_ && item.path == item_.path;
  // A relist that reopens the same bytes is not a new item: crop mode and a
  // failed-write latch stay. Anything else leaves crop mode.
  const bool same_file = same_path && item.size == item_.size && item.mtime == item_.mtime;
  if (!same_file) crop_ = false;
  if (!same_path) write_failed_ = false;
  const std::uint32_t keep_w = item_.width, keep_h = item_.height;
  item_ = item;
  if (same_file && item_.width == 0) {  // the host may not know the size yet
    item_.width = keep_w;
    item_.height = keep_h;
  }
  has_item_ = true;
  const std::uint64_t k = key();  // before any landing bumps the epoch

  // A rotation landed on disk and this is the rewritten file: the turns
  // pressed while it was in flight become the new file's stack. Any open of
  // that path after the job succeeded is the rewritten file.
  if (carry_ && carry_->path == item_.path) {
    const carry c = *carry_;
    carry_.reset();
    land(c.old_key, c.applied);
  } else if (in_flight_ && !in_flight_->landed && in_flight_->path == item_.path &&
             in_flight_->key != k) {
    // A folder relist reopened the file after the swap but before the job
    // reported back: it has landed.
    in_flight_->landed = true;
    land(in_flight_->key, in_flight_->op);
  }
  return wants_write();
}

void edit_session::land(std::uint64_t old_key, codec::d4 applied) {
  codec::d4 remaining{};
  std::vector<op> colour_ops;  // PR 11: colour set while the write was in flight
  if (const auto it = stacks_.find(old_key); it != stacks_.end()) {
    const edit::geometry g = edit::fold(it->second);
    if (g.orientation_only()) remaining = codec::compose(codec::inverse(applied), g.orient);
    for (const op& o : it->second.ops) {
      if (o.kind == op_kind::adjust) colour_ops.push_back(o);
    }
    stacks_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), old_key), order_.end());
  }
  ++epochs_[item_.path];
  write_failed_ = false;
  edit::edit_stack& s = current();
  s.ops = ops_for(remaining);
  s.ops.insert(s.ops.end(), colour_ops.begin(), colour_ops.end());
}

void edit_session::clear_item() noexcept {
  has_item_ = false;
  crop_ = false;
  item_ = {};
}

edit::geometry edit_session::preview() const {
  const edit::edit_stack* s = stack();
  edit::geometry g = s ? edit::fold(*s) : edit::geometry{};
  if (crop_) {
    g.crop = edit::rect{};
    g.straighten = draft_angle_;
  }
  g.resize = {};  // the canvas previews the crop; resize is an export setting
  return g;
}

edit::placement edit_session::preview_placement() const {
  return edit::place(preview(), edit::size2{item_.width, item_.height}, {}, crop_);
}

edit::geometry edit_session::export_geometry() const {
  const edit::edit_stack* s = stack();
  return s ? edit::fold(*s) : edit::geometry{};
}

edit::colour edit_session::colour() const {
  const edit::edit_stack* s = stack();
  return s ? edit::fold_colour(s->ops) : edit::colour{};
}

edit_effect edit_session::set_adjust(edit::adjust_param p, float value) {
  if (!has_item_ || static_cast<int>(p) >= edit::kAdjustParamCount) return edit_effect::refused;
  value = edit::clamp_param(p, value);
  edit::edit_stack& s = current();
  // A drag (or a held arrow key) is one op: replace a trailing set of the
  // same parameter instead of stacking hundreds of them.
  if (!s.ops.empty() && s.ops.back().kind == op_kind::adjust && s.ops.back().param == p) {
    if (s.ops.back().value == value) return edit_effect::none;
    s.ops.back().value = value;
    // A drag back to where the parameter stood before it is no op at all.
    const std::span<const op> before(s.ops.data(), s.ops.size() - 1);
    if (edit::fold_colour(before).get(p) == value) s.ops.pop_back();
    return edit_effect::redraw;
  }
  if (edit::fold_colour(s.ops).get(p) == value) return edit_effect::none;
  op o{op_kind::adjust};
  o.param = p;
  o.value = value;
  s.push(o);
  return edit_effect::redraw;
}

edit_effect edit_session::reset_adjust() {
  if (!has_item_) return edit_effect::refused;
  if (colour().identity()) return edit_effect::none;
  op o{op_kind::adjust};
  o.param = edit::adjust_param::count;  // every colour parameter, one undo step
  current().push(o);
  return edit_effect::redraw;
}

void edit_session::begin_crop() {
  const edit::geometry g = export_geometry();
  draft_angle_ = g.straighten;
  draft_rect_ = edit::constrain_crop(g.crop, draft_angle_, frame());
  draft_auto_ = g.crop == edit::rect{};
  crop_ = true;
}

void edit_session::cancel_crop() noexcept { crop_ = false; }

bool edit_session::awaiting_disk() const noexcept {
  return (in_flight_ && !in_flight_->landed) || (carry_ && has_item_ && carry_->path == item_.path);
}

edit_effect edit_session::turn(op_kind k) {
  if (!has_item_) return edit_effect::refused;
  if (crop_) {
    // Carry the draft through the turn exactly as fold() carries a crop.
    edit::op c{op_kind::crop};
    c.crop = draft_rect_;
    edit::op s{op_kind::straighten};
    s.degrees = draft_angle_;
    const op seq[] = {c, s, op{k}};
    const edit::geometry turned = edit::fold(seq);
    draft_rect_ = turned.crop;
    draft_angle_ = turned.straighten;
    current().push(op{k});
    return edit_effect::redraw;
  }
  current().push(op{k});
  return wants_write() ? edit_effect::write_rotation : edit_effect::redraw;
}

edit_effect edit_session::nudge(float dx, float dy, float dw, float dh) {
  if (!crop_) return edit_effect::none;
  edit::rect r = draft_rect_;
  r.w = std::clamp(r.w + dw, kMinCrop, 1.0f);
  r.h = std::clamp(r.h + dh, kMinCrop, 1.0f);
  r.x = std::clamp(r.x + dx, 0.0f, 1.0f - r.w);
  r.y = std::clamp(r.y + dy, 0.0f, 1.0f - r.h);
  // With an angle, a step that would uncover a corner is refused rather than
  // shrinking the rect under the user.
  if (draft_angle_ != 0.0f && !same_rect(edit::constrain_crop(r, draft_angle_, frame()), r)) {
    return edit_effect::none;
  }
  if (same_rect(r, draft_rect_)) return edit_effect::none;
  draft_rect_ = r;
  draft_auto_ = false;
  return edit_effect::redraw;
}

edit_effect edit_session::run(command_id id) {
  using enum command_id;
  switch (id) {
    case rotate_ccw: return turn(op_kind::rotate_ccw);
    case rotate_cw: return turn(op_kind::rotate_cw);
    case flip_horizontal: return turn(op_kind::flip_h);
    case flip_vertical: return turn(op_kind::flip_v);

    case crop_mode:
      // A crop joining a stack whose turns are still being written would
      // have to be re-expressed against the rewritten file; wait instead.
      if (!has_item_ || awaiting_disk()) return edit_effect::refused;
      if (crop_) return edit_effect::none;
      begin_crop();
      return edit_effect::redraw;

    case crop_commit: {
      if (!crop_) return edit_effect::none;
      const edit::geometry g = export_geometry();
      crop_ = false;
      edit::edit_stack& s = current();
      if (draft_angle_ != g.straighten) {
        op o{op_kind::straighten};
        o.degrees = draft_angle_;
        s.push(o);
      }
      const edit::rect committed = edit::constrain_crop(draft_rect_, draft_angle_, frame());
      // An untouched rect under a new angle is what place() derives anyway.
      if (!same_rect(committed, edit::constrain_crop(g.crop, draft_angle_, frame()))) {
        op o{op_kind::crop};
        o.crop = committed;
        s.push(o);
      }
      return edit_effect::redraw;
    }

    case crop_move_left: return nudge(-kNudge, 0, 0, 0);
    case crop_move_right: return nudge(kNudge, 0, 0, 0);
    case crop_move_up: return nudge(0, -kNudge, 0, 0);
    case crop_move_down: return nudge(0, kNudge, 0, 0);
    case crop_narrower: return nudge(0, 0, -kNudge, 0);
    case crop_wider: return nudge(0, 0, kNudge, 0);
    case crop_shorter: return nudge(0, 0, 0, -kNudge);
    case crop_taller: return nudge(0, 0, 0, kNudge);

    case straighten_ccw:
    case straighten_cw: {
      if (!crop_) return edit_effect::none;
      const float step = id == straighten_cw ? kStraightenStep : -kStraightenStep;
      float a = std::clamp(draft_angle_ + step, -edit::kMaxStraighten, edit::kMaxStraighten);
      if (std::abs(a) < 1e-4f) a = 0.0f;
      if (a == draft_angle_) return edit_effect::none;
      draft_angle_ = a;
      // An untouched rect follows the angle (largest fit); a placed one is
      // only shrunk as far as the new angle needs.
      draft_rect_ = draft_auto_ ? edit::auto_crop(a, frame())
                                : edit::constrain_crop(draft_rect_, a, frame());
      return edit_effect::redraw;
    }

    case export_image:
      // Until the rewritten file is back on the canvas the stack still holds
      // turns the file already has: exporting now would apply them twice.
      if (!has_item_ || awaiting_disk() || crop_) return edit_effect::refused;
      return edit_effect::export_image;

    case undo_edit: {
      if (!has_item_) return edit_effect::refused;
      if (!current().undo()) return edit_effect::refused;
      if (crop_) begin_crop();
      return wants_write() ? edit_effect::write_rotation : edit_effect::redraw;
    }

    case reset_edits: {
      if (!has_item_) return edit_effect::refused;
      edit::edit_stack& s = current();
      if (s.empty() && !crop_) return edit_effect::none;
      s.reset();
      if (crop_) begin_crop();
      return edit_effect::redraw;
    }

    default:
      return edit_effect::none;
  }
}

std::optional<rotation_write> edit_session::take_pending_write() {
  if (in_flight_ || !wants_write()) return std::nullopt;
  const codec::d4 op = edit::fold(*stack()).orient;
  in_flight_ = flight{item_.path, key(), op};
  return rotation_write{item_.path, item_.size, op};
}

void edit_session::write_finished(bool ok) {
  if (!in_flight_) return;
  if (in_flight_->landed) {
    in_flight_.reset();  // already carried when the rewritten file opened
    return;
  }
  if (ok) {
    carry_ = carry{in_flight_->path, in_flight_->key, in_flight_->op};
  } else if (has_item_ && key() == in_flight_->key) {
    // Keep the preview turned (an export can still bake it), stop retrying.
    write_failed_ = true;
  }
  in_flight_.reset();
}

// ---- export dialog ------------------------------------------------------------

std::int32_t pack_export(const edit::export_options& opt) noexcept {
  const int quality = std::clamp(opt.encode.quality, 1, 100);
  int edge = 0;
  if (opt.long_edge != 0) {
    for (int i = 1; i < kExportLongEdgeCount; ++i) {
      if (kExportLongEdges[i] == opt.long_edge) edge = i;
    }
  }
  return quality | (opt.encode.format == edit::image_format::png ? 1 << 7 : 0) |
         (static_cast<int>(opt.policy) & 3) << 8 | edge << 10;
}

edit::export_options unpack_export(std::int32_t packed) noexcept {
  edit::export_options opt;
  const int quality = packed & 0x7F;
  opt.encode.quality = quality >= 1 && quality <= 100 ? quality : 92;
  opt.encode.format = (packed >> 7) & 1 ? edit::image_format::png : edit::image_format::jpeg;
  const int policy = (packed >> 8) & 3;
  opt.policy = policy <= 2 ? static_cast<edit::metadata_policy>(policy) : edit::metadata_policy::all;
  const int edge = (packed >> 10) & 7;
  opt.long_edge = edge < kExportLongEdgeCount ? kExportLongEdges[edge] : 0;
  return opt;
}

// ---- jobs --------------------------------------------------------------------

expected run_rotation_write(const rotation_write& w) {
  MV_TRY(std::vector<std::uint8_t> bytes, io::read_all(w.path));
  // The file moved on under us (another app, a second window): do not write
  // an op made against bytes that are gone.
  if (bytes.size() != w.size) return err(status::io);
  MV_TRY(std::vector<std::uint8_t> rotated, edit::rotate_in_viewer(bytes, w.op));
  return io::replace_atomic(w.path, rotated);
}

result<std::string> run_export(std::string_view source_path, const edit::geometry& g,
                               const edit::export_options& opt, const edit::colour& c) {
  MV_TRY(std::vector<std::uint8_t> bytes, io::read_all(source_path));
  // HEIC, TIFF, RAW, WebP: the metadata comes from Exiv2 (meta/), which edit/
  // cannot call; JPEG and PNG are read by edit/ itself.
  edit::metadata_blobs carried;
  const codec::format_family family = codec::probe(bytes);
  if (opt.policy != edit::metadata_policy::none && family != codec::format_family::jpeg &&
      family != codec::format_family::png) {
    meta::carried_metadata m = meta::read_carried(bytes);
    carried.exif = std::move(m.exif);
    carried.xmp = std::move(m.xmp);
  }
  MV_TRY(edit::export_result r, edit::export_image(bytes, g, c, opt, nullptr, &carried));

  const std::size_t sep = source_path.find_last_of("/\\");
  const std::string dir = sep == std::string_view::npos ? std::string() : std::string(source_path.substr(0, sep + 1));
  const std::string_view name = sep == std::string_view::npos ? source_path : source_path.substr(sep + 1);
  const std::string wanted = edit::export_file_name(name, opt.encode.format);
  // write_new refuses an existing name, so a race between the check and the
  // write moves on to the next number instead of overwriting.
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::string file = io::unique_name(wanted, [&](std::string_view candidate) {
      return io::file_exists(dir + std::string(candidate));
    });
    if (file.empty()) return err(status::io);
    const std::string out = dir + file;
    if (io::write_new(out, r.bytes)) return out;
    if (!io::file_exists(out)) return err(status::io);
  }
  return err(status::io);
}

}  // namespace mv::shell
