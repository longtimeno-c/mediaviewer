// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/remove.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <thread>

#include "infer/ort.h"

namespace mv::enhance {

namespace {

// Along one axis of a w x h plane: each value the max (or the sum) of its
// window of 2r + 1, clipped at the edges.
template <typename T, typename Op>
std::vector<T> pass(const std::vector<T>& in, std::uint32_t w, std::uint32_t h, std::uint32_t r, bool across,
                    Op op) {
  std::vector<T> out(in.size());
  const std::uint32_t n = across ? w : h, lines = across ? h : w;
  for (std::uint32_t l = 0; l < lines; ++l) {
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t lo = i >= r ? i - r : 0, hi = std::min(n - 1, i + r);
      T acc{};
      for (std::uint32_t j = lo; j <= hi; ++j) {
        const std::size_t at = across ? std::size_t{l} * w + j : std::size_t{j} * w + l;
        acc = op(acc, in[at]);
      }
      out[across ? std::size_t{l} * w + i : std::size_t{i} * w + l] = acc;
    }
  }
  return out;
}

}  // namespace

namespace {

// Brushed bounds, inclusive.
struct bounds {
  std::uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
  [[nodiscard]] bool empty() const noexcept { return x0 > x1 || y0 > y1; }
  void add(std::uint32_t x, std::uint32_t y) noexcept {
    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
  }
  void add(const bounds& b) noexcept {
    x0 = std::min(x0, b.x0), y0 = std::min(y0, b.y0), x1 = std::max(x1, b.x1), y1 = std::max(y1, b.y1);
  }
};

// The bounds grown for context: half their longer side each way (at least
// kRemoveMinContext), square where the photo allows, inside it.
pixel_box grown_box(const bounds& r, std::uint32_t width, std::uint32_t height) {
  const std::uint32_t margin = std::max((std::max(r.x1 - r.x0, r.y1 - r.y0) + 1) / 2, kRemoveMinContext);
  // One axis: [lo, hi) grown by `margin`, then to `side` where the photo
  // allows, shifted back inside it at an edge.
  const auto grow = [](std::uint32_t lo, std::uint32_t hi, std::uint32_t by, std::uint32_t limit,
                       std::uint32_t side) {
    std::int64_t a = static_cast<std::int64_t>(lo) - by, b = static_cast<std::int64_t>(hi) + 1 + by;
    if (side > b - a) {
      const std::int64_t extra = side - (b - a);
      a -= extra / 2;
      b += extra - extra / 2;
    }
    if (a < 0) b -= a, a = 0;
    if (b > limit) a -= b - limit, b = limit;
    a = std::max<std::int64_t>(a, 0);
    return std::pair<std::uint32_t, std::uint32_t>(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
  };
  const std::uint32_t side = std::max(r.x1 - r.x0, r.y1 - r.y0) + 1 + 2 * margin;
  const auto [ax, bx] = grow(r.x0, r.x1, margin, width, side);
  const auto [ay, by] = grow(r.y0, r.y1, margin, height, side);
  return pixel_box{ax, ay, bx - ax, by - ay};
}

std::uint64_t area(const pixel_box& b) noexcept { return std::uint64_t{b.width} * b.height; }

pixel_box union_of(const pixel_box& a, const pixel_box& b) noexcept {
  const std::uint32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  const std::uint32_t x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
  return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

std::optional<pixel_box> remove_box(const mask8& mask) {
  bounds b;
  for (std::uint32_t y = 0; y < mask.height; ++y) {
    for (std::uint32_t x = 0; x < mask.width; ++x) {
      if (mask.a[std::size_t{y} * mask.width + x] > 127) b.add(x, y);
    }
  }
  if (b.empty()) return std::nullopt;
  return grown_box(b, mask.width, mask.height);
}

remove_plan remove_boxes(const mask8& mask) {
  remove_plan plan;
  const std::uint32_t w = mask.width, h = mask.height;
  if (w == 0 || h == 0) return plan;
  // The grid: at most kRemoveGrid cells a side, a cell brushed when any of
  // its pixels is.
  const std::uint32_t cell = std::max(1u, (std::max(w, h) + kRemoveGrid - 1) / kRemoveGrid);
  const std::uint32_t gw = (w + cell - 1) / cell, gh = (h + cell - 1) / cell;
  std::vector<std::uint32_t> grid(std::size_t{gw} * gh, 0);
  bool any = false;
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* row = mask.a.data() + std::size_t{y} * w;
    std::uint32_t* g = grid.data() + std::size_t{y / cell} * gw;
    for (std::uint32_t x = 0; x < w; ++x) {
      if (row[x] > 127) g[x / cell] = 1, any = true;
    }
  }
  if (!any) return plan;
  // Dilated 2 cells, then 8-connected blobs.
  const auto max_op = [](std::uint32_t a, std::uint32_t b) { return std::max(a, b); };
  grid = pass(pass(grid, gw, gh, 2, true, max_op), gw, gh, 2, false, max_op);
  std::vector<std::uint32_t> label(grid.size(), 0);  // blob + 1
  std::uint32_t blobs = 0;
  std::vector<std::uint32_t> stack;
  for (std::size_t start = 0; start < grid.size(); ++start) {
    if (!grid[start] || label[start]) continue;
    label[start] = ++blobs;
    stack.assign(1, static_cast<std::uint32_t>(start));
    while (!stack.empty()) {
      const std::uint32_t at = stack.back();
      stack.pop_back();
      const std::uint32_t cx = at % gw, cy = at / gw;
      for (std::uint32_t ny = cy > 0 ? cy - 1 : 0; ny <= std::min(gh - 1, cy + 1); ++ny) {
        for (std::uint32_t nx = cx > 0 ? cx - 1 : 0; nx <= std::min(gw - 1, cx + 1); ++nx) {
          const std::size_t n = std::size_t{ny} * gw + nx;
          if (grid[n] && !label[n]) label[n] = blobs, stack.push_back(static_cast<std::uint32_t>(n));
        }
      }
    }
  }
  // Each blob's exact bounds, from the full-resolution mask.
  std::vector<bounds> blob(blobs);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* row = mask.a.data() + std::size_t{y} * w;
    const std::uint32_t* l = label.data() + std::size_t{y / cell} * gw;
    for (std::uint32_t x = 0; x < w; ++x) {
      if (row[x] > 127) blob[l[x / cell] - 1].add(x, y);
    }
  }
  // Groups of blobs, one box each, merged while a union is cheap.
  struct group {
    bounds b;
    pixel_box box;
    bool live = true;
  };
  std::vector<group> groups;
  std::vector<std::uint32_t> group_of(blobs);  // blob -> group (followed to a live one)
  if (blobs <= kRemoveMaxBlobs) {
    groups.reserve(blobs);
    for (std::uint32_t i = 0; i < blobs; ++i) {
      groups.push_back({blob[i], grown_box(blob[i], w, h)});
      group_of[i] = i;
    }
    for (bool merged = true; merged;) {
      merged = false;
      for (std::size_t i = 0; i < groups.size() && !merged; ++i) {
        if (!groups[i].live) continue;
        for (std::size_t j = i + 1; j < groups.size(); ++j) {
          if (!groups[j].live) continue;
          const double together = static_cast<double>(area(union_of(groups[i].box, groups[j].box)));
          if (together >= 1.3 * static_cast<double>(area(groups[i].box) + area(groups[j].box))) continue;
          groups[i].b.add(groups[j].b);
          groups[i].box = grown_box(groups[i].b, w, h);
          groups[j].live = false;
          for (std::uint32_t& g : group_of) {
            if (g == j) g = static_cast<std::uint32_t>(i);
          }
          merged = true;
          break;
        }
      }
    }
  }
  std::vector<std::uint32_t> order;  // live groups, in fill order
  for (std::uint32_t i = 0; i < groups.size(); ++i) {
    if (groups[i].live) order.push_back(i);
  }
  std::vector<std::uint8_t> box_of(blobs, 1);  // blob -> box index + 1
  if (order.empty() || order.size() > kRemoveMaxBoxes) {
    // Too many to fill one by one: one box over everything, as before.
    bounds all;
    for (const bounds& b : blob) all.add(b);
    plan.boxes.push_back(grown_box(all, w, h));
  } else {
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
      const pixel_box &p = groups[a].box, &q = groups[b].box;
      return p.y != q.y ? p.y < q.y : p.x < q.x;
    });
    std::vector<std::uint8_t> index(groups.size(), 0);
    for (std::size_t k = 0; k < order.size(); ++k) {
      plan.boxes.push_back(groups[order[k]].box);
      index[order[k]] = static_cast<std::uint8_t>(k + 1);
    }
    for (std::uint32_t i = 0; i < blobs; ++i) box_of[i] = index[group_of[i]];
  }
  plan.cell = cell;
  plan.grid_w = gw;
  plan.grid_h = gh;
  plan.owner.resize(label.size());
  for (std::size_t i = 0; i < label.size(); ++i) {
    plan.owner[i] = label[i] ? box_of[label[i] - 1] : std::uint8_t{0};
  }
  return plan;
}

mask8 box_mask(const mask8& mask, const remove_plan& plan, std::size_t i, bool open) {
  const pixel_box& b = plan.boxes[i];
  const auto me = static_cast<std::uint8_t>(i + 1);
  mask8 out;
  out.width = b.width;
  out.height = b.height;
  out.a.assign(std::size_t{b.width} * b.height, 0);
  for (std::uint32_t y = 0; y < b.height; ++y) {
    const std::uint8_t* row = mask.a.data() + std::size_t{b.y + y} * mask.width + b.x;
    std::uint8_t* o = out.a.data() + std::size_t{y} * b.width;
    for (std::uint32_t x = 0; x < b.width; ++x) {
      if (row[x] <= 127) continue;
      const std::uint8_t who = plan.owner_at(b.x + x, b.y + y);
      if (open ? (who == 0 || who >= me) : who == me) o[x] = 255;
    }
  }
  return out;
}

rgb8 crop_rgb(const rgb8& src, const pixel_box& box) {
  rgb8 out;
  out.width = box.width;
  out.height = box.height;
  out.rgb.resize(std::size_t{box.width} * box.height * 3);
  for (std::uint32_t y = 0; y < box.height; ++y) {
    const std::uint8_t* row = src.rgb.data() + (std::size_t{box.y + y} * src.width + box.x) * 3;
    std::copy(row, row + std::size_t{box.width} * 3, out.rgb.data() + std::size_t{y} * box.width * 3);
  }
  return out;
}

mask8 crop_mask(const mask8& src, const pixel_box& box, std::uint32_t width, std::uint32_t height) {
  mask8 out;
  out.width = width;
  out.height = height;
  out.a.resize(std::size_t{width} * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t sy = box.y + std::min(box.height - 1, static_cast<std::uint32_t>((y + 0.5) * box.height / height));
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t sx = box.x + std::min(box.width - 1, static_cast<std::uint32_t>((x + 0.5) * box.width / width));
      out.a[std::size_t{y} * width + x] = src.a[std::size_t{sy} * src.width + sx] > 127 ? 255 : 0;
    }
  }
  return out;
}

namespace {

// `m` (the box's size) 1 where brushed.
void paste(rgb8& photo, const pixel_box& box, const rgb8& filled, std::vector<std::uint32_t> m,
           std::uint32_t feather) {
  const std::uint32_t w = box.width, h = box.height;
  // Grown by `feather`, then averaged over the same radius: 1 on every
  // brushed pixel, falling to 0 at 2 * feather outside.
  const auto max_op = [](std::uint32_t a, std::uint32_t b) { return std::max(a, b); };
  const auto sum_op = [](std::uint32_t a, std::uint32_t b) { return a + b; };
  m = pass(pass(m, w, h, feather, true, max_op), w, h, feather, false, max_op);
  const std::vector<std::uint32_t> sum = pass(pass(m, w, h, feather, true, sum_op), w, h, feather, false, sum_op);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint32_t cy = std::min(h - 1, y + feather) - (y >= feather ? y - feather : 0) + 1;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint32_t cx = std::min(w - 1, x + feather) - (x >= feather ? x - feather : 0) + 1;
      const std::uint32_t s = sum[std::size_t{y} * w + x], count = cx * cy;
      if (s == 0) continue;  // the photo's own pixel, untouched
      std::uint8_t* p = photo.rgb.data() + (std::size_t{box.y + y} * photo.width + box.x + x) * 3;
      const std::uint8_t* f = filled.rgb.data() + (std::size_t{y} * w + x) * 3;
      for (int c = 0; c < 3; ++c) {
        p[c] = s == count ? f[c]
                          : static_cast<std::uint8_t>((p[c] * (count - s) + f[c] * s + count / 2) / count);
      }
    }
  }
}

}  // namespace

std::uint32_t remove_feather(const pixel_box& box) noexcept {
  return std::clamp<std::uint32_t>(std::max(box.width, box.height) / 256, 2, 16);
}

bool needs_refine(const pixel_box& box, std::uint32_t side) noexcept {
  return side > 0 && std::uint64_t{std::max(box.width, box.height)} * 2 > std::uint64_t{side} * 3;
}

namespace {

// Starts of tiles `t` long covering [lo, hi) inside [0, limit): one centred
// when it fits, else half overlapping from lo, the last ending at hi.
std::vector<std::uint32_t> starts_over(std::uint32_t lo, std::uint32_t hi, std::uint32_t t, std::uint32_t limit) {
  std::vector<std::uint32_t> out;
  const std::uint32_t len = hi - lo, last = limit - t;
  if (len <= t) {
    const std::int64_t c = static_cast<std::int64_t>(lo) + len / 2 - t / 2;
    out.push_back(static_cast<std::uint32_t>(std::clamp<std::int64_t>(c, 0, last)));
    return out;
  }
  const std::uint32_t step = std::max(1u, t / 2);
  for (std::uint32_t at = lo;; at += step) {
    const std::uint32_t s = std::min(std::min(at, hi - t), last);
    out.push_back(s);
    if (s + t >= hi) break;
  }
  return out;
}

// The how-much-was-known weight of a tile: nothing under a tenth of its
// picture known (the model has nothing to continue), all from two fifths.
float known_weight(double known) noexcept {
  return static_cast<float>(std::clamp((known - 0.1) / 0.3, 0.0, 1.0));
}

// Where the tiles over a pixel barely count (their tents' edges, little
// known), their detail fades out against this.
inline constexpr float kRefineBase = 0.02f;

}  // namespace

std::vector<pixel_box> refine_tiles(const mask8& mask, const remove_plan& plan, std::size_t i, std::uint32_t side) {
  std::vector<pixel_box> out;
  const pixel_box& box = plan.boxes[i];
  if (!needs_refine(box, side) || mask.width == 0 || mask.height == 0) return out;
  const mask8 own = box_mask(mask, plan, i, false);
  // What it pastes back: its own brushed pixels' bounds, and an integral
  // image of them to keep only the tiles that hold some.
  const std::uint32_t bw = box.width, bh = box.height;
  std::vector<std::uint32_t> sat(std::size_t{bw + 1} * (bh + 1), 0);
  bounds b;
  for (std::uint32_t y = 0; y < bh; ++y) {
    std::uint32_t row = 0;
    for (std::uint32_t x = 0; x < bw; ++x) {
      const bool on = own.a[std::size_t{y} * bw + x] > 127;
      if (on) b.add(x, y);
      row += on ? 1u : 0u;
      sat[std::size_t{y + 1} * (bw + 1) + x + 1] = sat[std::size_t{y} * (bw + 1) + x + 1] + row;
    }
  }
  if (b.empty()) return out;
  // Every pixel the paste can change is within `reach` of one it fills; the
  // tiles reach past them by at least a quarter tile for context.
  const std::uint32_t reach = 2 * remove_feather(box) + 1, pad = std::max(reach, side / 4);
  const std::uint32_t w = mask.width, h = mask.height;
  const std::uint32_t tw = std::min(side, w), th = std::min(side, h);
  const auto lo = [&](std::uint32_t v) { return v > pad ? v - pad : 0u; };
  const std::uint32_t gx0 = lo(box.x + b.x0), gy0 = lo(box.y + b.y0);
  const std::uint32_t gx1 = std::min(w, box.x + b.x1 + 1 + pad), gy1 = std::min(h, box.y + b.y1 + 1 + pad);
  const auto sum = [&](std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1) {  // box-local, clipped
    x0 = std::clamp<std::int64_t>(x0, 0, bw), x1 = std::clamp<std::int64_t>(x1, 0, bw);
    y0 = std::clamp<std::int64_t>(y0, 0, bh), y1 = std::clamp<std::int64_t>(y1, 0, bh);
    if (x0 >= x1 || y0 >= y1) return std::uint32_t{0};
    const auto at = [&](std::int64_t x, std::int64_t y) { return sat[static_cast<std::size_t>(y * (bw + 1) + x)]; };
    return at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0);
  };
  for (const std::uint32_t y : starts_over(gy0, gy1, th, h)) {
    for (const std::uint32_t x : starts_over(gx0, gx1, tw, w)) {
      const std::int64_t lx = static_cast<std::int64_t>(x) - box.x, ly = static_cast<std::int64_t>(y) - box.y;
      if (sum(lx - reach, ly - reach, lx + tw + reach, ly + th + reach) == 0) continue;
      out.push_back({x, y, tw, th});
    }
  }
  if (out.empty()) return out;
  // Only the tiles refine_fill runs: some hole (box i's open mask) and more
  // than a tenth known, so progress counts fills and the time left is the
  // work left. An integral image of the holes over the tiles' union.
  pixel_box u = out[0];
  for (const pixel_box& t : out) u = union_of(u, t);
  const auto me = static_cast<std::uint8_t>(i + 1);
  std::vector<std::uint32_t> holes(std::size_t{u.width + 1} * (u.height + 1), 0);
  for (std::uint32_t y = 0; y < u.height; ++y) {
    const std::uint8_t* row = mask.a.data() + std::size_t{u.y + y} * w + u.x;
    std::uint32_t run = 0;
    for (std::uint32_t x = 0; x < u.width; ++x) {
      if (row[x] > 127) {
        const std::uint8_t who = plan.owner_at(u.x + x, u.y + y);
        run += who == 0 || who >= me ? 1u : 0u;
      }
      holes[std::size_t{y + 1} * (u.width + 1) + x + 1] = holes[std::size_t{y} * (u.width + 1) + x + 1] + run;
    }
  }
  std::erase_if(out, [&](const pixel_box& t) {
    const std::size_t x0 = t.x - u.x, y0 = t.y - u.y, x1 = x0 + t.width, y1 = y0 + t.height, s = u.width + 1;
    const std::uint32_t n = holes[y1 * s + x1] - holes[y0 * s + x1] - holes[y1 * s + x0] + holes[y0 * s + x0];
    const double area = static_cast<double>(t.width) * t.height;
    return n == 0 || known_weight(1.0 - static_cast<double>(n) / area) <= 0.0f;
  });
  return out;
}

expected refine_fill(const rgb8& photo, const mask8& mask, const remove_plan& plan, std::size_t i,
                     const std::vector<pixel_box>& tiles, inpaint_filler& model, rgb8& filled,
                     const std::function<bool()>& next) {
  if (tiles.empty()) return {};
  const pixel_box& box = plan.boxes[i];
  const std::uint32_t side = model.side();
  const auto me = static_cast<std::uint8_t>(i + 1);
  // How much the first pass shrank the box: the detail it could not keep is
  // what a tile has above that scale.
  const double sx = static_cast<double>(box.width) / side, sy = static_cast<double>(box.height) / side;
  // Sums over the box's pixels the tiles cover (nothing outside the box is
  // pasted): `r`, in the photo.
  pixel_box r = tiles[0];
  for (const pixel_box& t : tiles) r = union_of(r, t);
  {
    const std::uint32_t x0 = std::max(r.x, box.x), y0 = std::max(r.y, box.y);
    const std::uint32_t x1 = std::min(r.x + r.width, box.x + box.width), y1 = std::min(r.y + r.height, box.y + box.height);
    if (x0 >= x1 || y0 >= y1) return {};
    r = {x0, y0, x1 - x0, y1 - y0};
  }
  std::vector<float> sum(std::size_t{r.width} * r.height * 3, 0.0f), weight(std::size_t{r.width} * r.height, 0.0f);
  for (const pixel_box& t : tiles) {
    if (next && !next()) return err(status::cancelled);
    // The tile's holes: box i's open mask (its own pixels and the later
    // boxes'; the earlier boxes' are filled in the photo).
    const std::size_t area = std::size_t{t.width} * t.height;
    mask8 m;
    m.width = t.width;
    m.height = t.height;
    m.a.assign(area, 0);
    std::size_t holes = 0;
    for (std::uint32_t y = 0; y < t.height; ++y) {
      const std::uint8_t* row = mask.a.data() + std::size_t{t.y + y} * mask.width + t.x;
      for (std::uint32_t x = 0; x < t.width; ++x) {
        if (row[x] <= 127) continue;
        const std::uint8_t who = plan.owner_at(t.x + x, t.y + y);
        if (who != 0 && who < me) continue;
        m.a[std::size_t{y} * t.width + x] = 255;
        ++holes;
      }
    }
    if (holes == 0) continue;
    const float conf = known_weight(1.0 - static_cast<double>(holes) / static_cast<double>(area));
    if (conf <= 0.0f) continue;  // nothing to continue: the first pass alone there
    const bool exact = t.width == side && t.height == side;
    const rgb8 in = crop_rgb(photo, t);
    MV_TRY(rgb8 out, model.fill(exact ? in : resize_rgb(in, side, side),
                                exact ? m : crop_mask(m, pixel_box{0, 0, t.width, t.height}, side, side)));
    if (!exact) out = resize_rgb(out, t.width, t.height);
    // Its detail: the fill less the fill at the first pass's scale.
    const auto lw = static_cast<std::uint32_t>(std::max(1.0, std::round(t.width / sx)));
    const auto lh = static_cast<std::uint32_t>(std::max(1.0, std::round(t.height / sy)));
    const rgb8 low = resize_rgb(resize_rgb(out, lw, lh), t.width, t.height);
    // A tent across the tile, never 0 inside it, times how much it knew.
    for (std::uint32_t y = 0; y < t.height; ++y) {
      const std::uint32_t py = t.y + y;
      if (py < r.y || py >= r.y + r.height) continue;
      const float wy = (std::min(y, t.height - 1 - y) + 1.0f) / (t.height * 0.5f + 1.0f);
      for (std::uint32_t x = 0; x < t.width; ++x) {
        const std::uint32_t px = t.x + x;
        if (px < r.x || px >= r.x + r.width) continue;
        const float wt = conf * wy * (std::min(x, t.width - 1 - x) + 1.0f) / (t.width * 0.5f + 1.0f);
        const std::size_t at = std::size_t{py - r.y} * r.width + (px - r.x), from = (std::size_t{y} * t.width + x) * 3;
        for (int c = 0; c < 3; ++c) {
          sum[at * 3 + c] += wt * (static_cast<float>(out.rgb[from + c]) - static_cast<float>(low.rgb[from + c]));
        }
        weight[at] += wt;
      }
    }
  }
  for (std::uint32_t y = 0; y < r.height; ++y) {
    for (std::uint32_t x = 0; x < r.width; ++x) {
      const std::size_t at = std::size_t{y} * r.width + x;
      if (weight[at] <= 0.0f) continue;
      const float all = kRefineBase + weight[at];
      std::uint8_t* f = filled.rgb.data() + (std::size_t{r.y - box.y + y} * box.width + (r.x - box.x + x)) * 3;
      for (int c = 0; c < 3; ++c) {
        const float v = static_cast<float>(f[c]) + sum[at * 3 + c] / all;
        f[c] = static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  return {};
}

void paste_back(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& mask, std::uint32_t feather) {
  const std::uint32_t w = box.width, h = box.height;
  std::vector<std::uint32_t> m(std::size_t{w} * h);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      m[std::size_t{y} * w + x] = mask.a[std::size_t{box.y + y} * mask.width + box.x + x] > 127 ? 1 : 0;
    }
  }
  paste(photo, box, filled, std::move(m), feather);
}

void paste_back_box(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& own, std::uint32_t feather) {
  std::vector<std::uint32_t> m(own.a.size());
  for (std::size_t i = 0; i < m.size(); ++i) m[i] = own.a[i] > 127 ? 1 : 0;
  paste(photo, box, filled, std::move(m), feather);
}

mask8 mask_from_dabs(const std::vector<float>& dabs, std::uint32_t width, std::uint32_t height) {
  mask8 out;
  out.width = width;
  out.height = height;
  out.a.assign(std::size_t{width} * height, 0);
  if (width == 0 || height == 0) return out;
  const float l = static_cast<float>(std::max(width, height));
  for (std::size_t i = 0; i + 2 < dabs.size(); i += 3) {
    const float cx = dabs[i] * static_cast<float>(width), cy = dabs[i + 1] * static_cast<float>(height);
    const float r = dabs[i + 2] * l;
    if (!(r > 0.0f) || !std::isfinite(cx) || !std::isfinite(cy)) continue;
    const auto lo = [](float v) { return static_cast<std::int64_t>(std::floor(v)); };
    const std::int64_t x0 = std::max<std::int64_t>(0, lo(cx - r));
    const std::int64_t x1 = std::min<std::int64_t>(std::int64_t{width} - 1, lo(cx + r));
    const std::int64_t y0 = std::max<std::int64_t>(0, lo(cy - r));
    const std::int64_t y1 = std::min<std::int64_t>(std::int64_t{height} - 1, lo(cy + r));
    for (std::int64_t y = y0; y <= y1; ++y) {
      for (std::int64_t x = x0; x <= x1; ++x) {
        const float px = static_cast<float>(x) + 0.5f - cx, py = static_cast<float>(y) + 0.5f - cy;
        if (px * px + py * py <= r * r) out.a[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = 255;
      }
    }
  }
  return out;
}

extend_margins extend_preset(std::string_view preset, std::uint32_t w, std::uint32_t h) noexcept {
  extend_margins m;
  if (w == 0 || h == 0) return m;
  const auto to_aspect = [&](double aspect) {  // aspect = width / height, grown on one axis only
    const double now = static_cast<double>(w) / h;
    if (aspect > now) {
      const auto extra = static_cast<std::uint32_t>(h * aspect - w);
      m.left = extra / 2;
      m.right = extra - m.left;
    } else if (aspect < now) {
      const auto extra = static_cast<std::uint32_t>(w / aspect - h);
      m.top = extra / 2;
      m.bottom = extra - m.top;
    }
  };
  if (preset == "wider") {
    m.left = m.right = w / 8;
  } else if (preset == "taller") {
    m.top = m.bottom = h / 8;
  } else if (preset == "all") {
    m.left = m.right = w / 10;
    m.top = m.bottom = h / 10;
  } else if (preset == "square") {
    to_aspect(1.0);
  } else if (preset == "16x9") {
    to_aspect(16.0 / 9.0);
  } else if (preset == "4x5") {
    to_aspect(4.0 / 5.0);
  }
  // At most twice the size each way, and 16384 a side.
  const auto cap = [](std::uint32_t& a, std::uint32_t& b, std::uint32_t side) {
    const std::uint32_t most = std::min(side, 16384u > side ? 16384u - side : 0u);
    if (a + b > most) {
      const double k = static_cast<double>(most) / (a + b);
      a = static_cast<std::uint32_t>(a * k);
      b = static_cast<std::uint32_t>(b * k);
    }
  };
  cap(m.left, m.right, w);
  cap(m.top, m.bottom, h);
  return m;
}

void extend_canvas(const rgb8& photo, const extend_margins& m, rgb8& out, mask8& mask) {
  out.width = photo.width + m.left + m.right;
  out.height = photo.height + m.top + m.bottom;
  out.rgb.assign(std::size_t{out.width} * out.height * 3, 0);
  mask.width = out.width;
  mask.height = out.height;
  mask.a.assign(std::size_t{out.width} * out.height, 255);
  // The margins start as the photo mirrored at its edge (texture for the
  // model to continue; a stretched edge pixel gave streaks, 2026-11-04).
  const auto mirror = [](std::int64_t v, std::uint32_t n) {
    const std::int64_t period = 2 * static_cast<std::int64_t>(n);
    std::int64_t r = ((v % period) + period) % period;
    if (r >= n) r = period - 1 - r;
    return static_cast<std::uint32_t>(r);
  };
  for (std::uint32_t y = 0; y < out.height; ++y) {
    const std::uint32_t sy = mirror(static_cast<std::int64_t>(y) - m.top, photo.height);
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint32_t sx = mirror(static_cast<std::int64_t>(x) - m.left, photo.width);
      const std::uint8_t* s = photo.rgb.data() + (std::size_t{sy} * photo.width + sx) * 3;
      std::uint8_t* d = out.rgb.data() + (std::size_t{y} * out.width + x) * 3;
      d[0] = s[0], d[1] = s[1], d[2] = s[2];
      const bool inside = x >= m.left && x < m.left + photo.width && y >= m.top && y < m.top + photo.height;
      if (inside) mask.a[std::size_t{y} * out.width + x] = 0;
    }
  }
}

std::vector<pixel_box> extend_tiles(std::uint32_t w, std::uint32_t h, const extend_margins& m) {
  std::vector<pixel_box> out;
  if (w == 0 || h == 0) return out;
  const std::uint32_t shorter = std::min(w, h);
  // One margin: `depth` deep across a picture `across` wide there, `length`
  // long. Tiles are (a, b, size) with `a` the start across from the margin's
  // outer edge, `b` along it.
  struct tile {
    std::uint32_t a, b, size;
  };
  const auto margin = [&](std::uint32_t depth, std::uint32_t across, std::uint32_t length) {
    std::vector<tile> ts;
    // Three times the depth (as deep into the photo as the margin is, each
    // side), at least kExtendMinTile, at most the picture's shorter side.
    const std::uint32_t t = std::min(std::max(depth * 3, kExtendMinTile), shorter);
    // Along: two-thirds overlapping (every pixel a third of a tile past the
    // ends in three), and at each end two more anchored there, of other
    // sizes, which give the end pixels their three: larger (a quarter and a
    // half again) where the picture has room, else smaller (three quarters,
    // a half).
    std::vector<std::uint32_t> along;
    for (std::uint32_t at = 0, step = std::max(1u, t / 3);; at += step) {
      const std::uint32_t start = std::min(at, length - t);
      along.push_back(start);
      if (start + t >= length) break;
    }
    // Across: from the outer edge, as many as reach past the margin into
    // the photo (one, unless the picture is thinner than the margin is deep).
    const auto across_starts = [&](std::uint32_t size) {
      std::vector<std::uint32_t> as;
      const std::uint32_t reach = std::min(across, depth + std::max(1u, size / 3));
      const std::uint32_t step = std::max(1u, size / 3);
      for (std::uint32_t at = 0;; at += step) {
        const std::uint32_t start = std::min(at, across - size);
        as.push_back(start);
        if (start + size >= reach) break;
      }
      return as;
    };
    for (const std::uint32_t a : across_starts(t)) {
      for (const std::uint32_t b : along) ts.push_back({a, b, t});
    }
    const std::uint32_t b1 = std::min(shorter, t + t / 4), b2 = std::min(shorter, t + t / 2);
    std::uint32_t ends[2] = {(t * 3 + 3) / 4, (t + 1) / 2};
    if (b1 > t) ends[0] = b1;
    if (b1 > t && b2 > b1) ends[1] = b2;
    for (const std::uint32_t size : ends) {
      if (size == 0 || size == t) continue;
      for (const std::uint32_t a : across_starts(size)) {
        ts.push_back({a, 0, size});
        ts.push_back({a, length - size, size});
      }
    }
    return ts;
  };
  const auto add = [&](pixel_box b) {
    for (const pixel_box& o : out) {
      if (o.x == b.x && o.y == b.y && o.width == b.width && o.height == b.height) return;
    }
    out.push_back(b);
  };
  if (m.left) {
    for (const tile& t : margin(m.left, w, h)) add({t.a, t.b, t.size, t.size});
  }
  if (m.right) {
    for (const tile& t : margin(m.right, w, h)) add({w - t.a - t.size, t.b, t.size, t.size});
  }
  if (m.top) {
    for (const tile& t : margin(m.top, h, w)) add({t.b, t.a, t.size, t.size});
  }
  if (m.bottom) {
    for (const tile& t : margin(m.bottom, h, w)) add({t.b, h - t.a - t.size, t.size, t.size});
  }
  return out;
}

expected extend_fill(rgb8& canvas, const mask8& mask, const std::vector<pixel_box>& tiles, inpaint_filler& model,
                     const std::function<bool(std::size_t, std::size_t)>& progress) {
  const std::uint32_t w = canvas.width, h = canvas.height, side = model.side();
  std::vector<float> sum(std::size_t{w} * h * 3, 0.0f), weight(std::size_t{w} * h, 0.0f);
  for (std::size_t i = 0; i < tiles.size(); ++i) {
    if (progress && !progress(i, tiles.size())) return err(status::cancelled);
    const pixel_box& b = tiles[i];
    MV_TRY(rgb8 filled, model.fill(resize_rgb(crop_rgb(canvas, b), side, side), crop_mask(mask, b, side, side)));
    const rgb8 back = resize_rgb(filled, b.width, b.height);
    for (std::uint32_t y = 0; y < b.height; ++y) {
      // A tent across the tile, never 0 inside it (a tile at the picture's
      // edge is the only one there).
      const float wy = (std::min(y, b.height - 1 - y) + 1.0f) / (b.height * 0.5f + 1.0f);
      for (std::uint32_t x = 0; x < b.width; ++x) {
        const std::size_t at = std::size_t{b.y + y} * w + b.x + x;
        if (mask.a[at] <= 127) continue;
        const float wt = wy * (std::min(x, b.width - 1 - x) + 1.0f) / (b.width * 0.5f + 1.0f);
        const std::uint8_t* f = back.rgb.data() + (std::size_t{y} * b.width + x) * 3;
        for (int c = 0; c < 3; ++c) sum[at * 3 + c] += wt * f[c];
        weight[at] += wt;
      }
    }
  }
  for (std::size_t at = 0; at < weight.size(); ++at) {
    if (mask.a[at] <= 127 || weight[at] <= 0.0f) continue;
    for (int c = 0; c < 3; ++c) {
      canvas.rgb[at * 3 + c] = static_cast<std::uint8_t>(std::clamp(sum[at * 3 + c] / weight[at] + 0.5f, 0.0f, 255.0f));
    }
  }
  if (progress) (void)progress(tiles.size(), tiles.size());
  return {};
}

struct inpaint_model::impl {
  std::unique_ptr<infer::session> s;
  inpaint_io io;
};

inpaint_model::inpaint_model(std::unique_ptr<impl> p) : p_(std::move(p)) {}
inpaint_model::~inpaint_model() = default;
std::uint32_t inpaint_model::side() const noexcept { return p_->io.side; }

result<std::unique_ptr<inpaint_model>> inpaint_model::open(infer::runtime& rt, const std::string& file,
                                                           const inpaint_io& io) {
  auto p = std::make_unique<impl>();
  p->io = io;
  // CPU: the model is small (about a second a fill at 512 px), and its FFT
  // layers are not ones Core ML is known to take.
  infer::session_options o;
  o.on = infer::backend::cpu;
  o.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 9u) - 1);
  MV_TRY(p->s, infer::session::open(rt, file, o));
  return std::unique_ptr<inpaint_model>(new inpaint_model(std::move(p)));
}

result<rgb8> inpaint_model::fill(const rgb8& image, const mask8& mask) {
  const std::uint32_t n = p_->io.side;
  if (image.width != n || image.height != n || mask.width != n || mask.height != n) return err(status::invalid_arg);
  const std::size_t px = std::size_t{n} * n;
  std::vector<float> pic(3 * px), m(px);
  for (std::size_t i = 0; i < px; ++i) {
    for (std::size_t k = 0; k < 3; ++k) pic[k * px + i] = image.rgb[i * 3 + k] / 255.0f;
    m[i] = mask.a[i] > 127 ? 1.0f : 0.0f;
  }
  const std::int64_t side = n;
  const infer::session::named_input in[2] = {
      {p_->io.image, {1, 3, side, side}, infer::session::element::f32, pic.data(), nullptr, nullptr, pic.size()},
      {p_->io.mask, {1, 1, side, side}, infer::session::element::f32, m.data(), nullptr, nullptr, m.size()},
  };
  MV_TRY(auto out, p_->s->run_named(std::span(in, 2)));
  if (out.empty() || out[0].data.size() != 3 * px) return err(status::internal);
  rgb8 r;
  r.width = n;
  r.height = n;
  r.rgb.resize(3 * px);
  const float scale = p_->io.out_scale;
  for (std::size_t i = 0; i < px; ++i) {
    for (std::size_t k = 0; k < 3; ++k) {
      const float v = out[0].data[k * px + i] * scale;
      r.rgb[i * 3 + k] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0l, 255l));
    }
  }
  return r;
}

}  // namespace mv::enhance
