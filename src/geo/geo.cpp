// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/06 "Place". The format is blob.h; the writer tools/geo/build_places.py.
#include "geo/geo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "geo/fold.h"

namespace mv::geo {
namespace {

using namespace blob;

constexpr std::uint32_t k_none = UINT32_MAX;
constexpr double k_km_per_deg = 111.32;
constexpr std::int64_t k_q64 = k_q;

int band_of(std::int64_t y) noexcept {
  const std::int64_t b = (y + 90 * k_q64) / k_q64;
  return static_cast<int>(std::clamp<std::int64_t>(b, 0, 179));
}

int col_of(std::int64_t x) noexcept {
  const std::int64_t c = (x + 180 * k_q64) / k_q64;
  return static_cast<int>(std::clamp<std::int64_t>(c, 0, 359));
}

std::int64_t wrap_dx(std::int64_t d) noexcept {
  if (d > 180 * k_q64) d -= 360 * k_q64;
  if (d < -180 * k_q64) d += 360 * k_q64;
  return d;
}

// Distance from the origin to segment a-b, all in km on a local plane.
double seg_km(double ax, double ay, double bx, double by) noexcept {
  const double dx = bx - ax, dy = by - ay;
  const double l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? -(ax * dx + ay * dy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const double ex = ax + t * dx, ey = ay + t * dy;
  return std::sqrt(ex * ex + ey * ey);
}

std::uint64_t checksum(const std::uint8_t* p, std::size_t n) noexcept {
  // The writer's checksum() in build_places.py: four independent lanes so the
  // multiplies overlap; ~5 ms for the full 17 MB blob.
  std::uint64_t lane[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull,
                           0x082EFA98EC4E6C89ull};
  constexpr std::uint64_t k = 0x9E3779B97F4A7C15ull;
  const std::size_t words = n / 8;
  std::size_t i = 0;
  for (; i + 4 <= words; i += 4) {
    for (int l = 0; l < 4; ++l) {
      std::uint64_t w;
      std::memcpy(&w, p + (i + static_cast<std::size_t>(l)) * 8, 8);
      std::uint64_t h = (lane[l] ^ w) * k;
      lane[l] = h ^ (h >> 29);
    }
  }
  for (; i < words; ++i) {
    std::uint64_t w;
    std::memcpy(&w, p + i * 8, 8);
    const int l = static_cast<int>(i & 3);
    std::uint64_t h = (lane[l] ^ w) * k;
    lane[l] = h ^ (h >> 29);
  }
  std::uint64_t r = 0;
  for (std::uint64_t h : lane) {
    r = (r ^ h) * 0xBF58476D1CE4E5B9ull;
    r ^= r >> 31;
  }
  return r;
}

// Collapses runs of white space to one space and trims, after folding.
std::string key_of(std::string_view s) {
  const std::string f = fold(s);
  std::string out;
  out.reserve(f.size());
  bool space = false;
  for (char c : f) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

class reader {
 public:
  reader(std::span<const std::uint8_t> bytes, std::span<const section> table) : bytes_(bytes), table_(table) {}

  template <typename T>
  bool get(std::uint32_t id, std::span<const T>& out, bool required = true) const {
    for (const section& s : table_) {
      if (s.id != id) continue;
      if (s.elem_bytes != sizeof(T) || s.offset % 8 != 0 || s.offset > bytes_.size() ||
          s.bytes > bytes_.size() - s.offset || s.bytes % sizeof(T) != 0) {
        return false;
      }
      out = {reinterpret_cast<const T*>(bytes_.data() + s.offset), static_cast<std::size_t>(s.bytes / sizeof(T))};
      return true;
    }
    out = {};
    return !required;
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::span<const section> table_;
};

bool valid_grid(std::span<const row> rows, std::span<const std::uint32_t> cells, std::size_t target) {
  if (rows.size() != 180) return false;
  for (const row& r : rows) {
    if (r.col_begin > r.col_end || r.col_end > 360) return false;
    if (r.col_begin == r.col_end) continue;
    const std::uint64_t n = r.col_end - r.col_begin;
    if (r.base > cells.size() || n + 1 > cells.size() - r.base) return false;
    for (std::uint64_t k = r.base; k < r.base + n; ++k) {
      if (cells[k] > cells[k + 1]) return false;
    }
    if (cells[r.base + n] > target) return false;
  }
  return true;
}

}  // namespace

// ---- open ---------------------------------------------------------------------------------

result<database> database::from_bytes(std::span<const std::uint8_t> bytes, bool verify_checksum) {
  if (bytes.size() < sizeof(header) || reinterpret_cast<std::uintptr_t>(bytes.data()) % 8 != 0) {
    return err(status::corrupt);
  }
  header h;
  std::memcpy(&h, bytes.data(), sizeof h);
  if (std::memcmp(h.magic, k_magic, sizeof h.magic) != 0) return err(status::corrupt);
  if (h.version != k_version) return err(h.version > k_version ? status::unsupported_format : status::corrupt);
  if (h.header_bytes != sizeof(header) || h.file_bytes != bytes.size() || (bytes.size() - sizeof(header)) % 8 != 0 ||
      h.section_count == 0 || h.section_count > k_max_sections ||
      sizeof(header) + std::uint64_t{h.section_count} * sizeof(section) > bytes.size()) {
    return err(status::corrupt);
  }
  if (verify_checksum && checksum(bytes.data() + sizeof(header), bytes.size() - sizeof(header)) != h.checksum) {
    return err(status::corrupt);
  }
  const std::span<const section> table{reinterpret_cast<const section*>(bytes.data() + sizeof(header)),
                                       h.section_count};
  const reader rd(bytes, table);

  database db;
  std::span<const char> strings;
  std::span<const char> meta;
  if (!rd.get(section_id::strings, strings) || strings.empty() || strings.back() != '\0' ||
      !rd.get(section_id::countries, db.countries_) || db.countries_.empty() ||
      db.countries_.size() >= k_no_country || !rd.get(section_id::regions, db.regions_) ||
      db.regions_.size() >= k_no_region || !rd.get(section_id::towns, db.towns_) ||
      !rd.get(section_id::town_rows, db.town_grid_.rows) || !rd.get(section_id::town_cells, db.town_grid_.cells) ||
      !rd.get(section_id::meta, meta, false)) {
    return err(status::corrupt);
  }
  db.strings_ = strings;
  db.meta_ = std::string_view(meta.data(), meta.size());
  const auto str_ok = [&](std::uint32_t o) { return o < strings.size(); };

  for (const country& c : db.countries_) {
    if (!str_ok(c.name) || !str_ok(c.native) || !str_ok(c.aliases)) return err(status::corrupt);
  }
  for (const region& r : db.regions_) {
    if ((r.country != k_no_country && r.country >= db.countries_.size()) || !str_ok(r.code) || !str_ok(r.name) ||
        !str_ok(r.aliases)) {
      return err(status::corrupt);
    }
  }
  for (const town& t : db.towns_) {
    if (t.country >= db.countries_.size() || (t.region != k_no_region && t.region >= db.regions_.size()) ||
        !str_ok(t.name)) {
      return err(status::corrupt);
    }
  }
  if (!valid_grid(db.town_grid_.rows, db.town_grid_.cells, db.towns_.size())) return err(status::corrupt);

  for (int li = 0; li < 2; ++li) {
    poly_layer& l = li == 0 ? db.l0_ : db.l1_;
    const std::uint32_t base = li == 0 ? section_id::layer_countries : section_id::layer_regions;
    if (!rd.get(base + layer_part::polys, l.polys) || !rd.get(base + layer_part::rings, l.rings) ||
        !rd.get(base + layer_part::points, l.points) || !rd.get(base + layer_part::bands, l.bands) ||
        !rd.get(base + layer_part::edges, l.edges) || !rd.get(base + layer_part::rows, l.index.rows) ||
        !rd.get(base + layer_part::cells, l.index.cells) || !rd.get(base + layer_part::cands, l.cands) ||
        l.polys.size() >= k_full) {
      return err(status::corrupt);
    }
    // Every point on the globe, so the exact 64-bit crossing test cannot
    // overflow (differences stay within 360 degrees of microdegrees).
    for (const point& pt : l.points) {
      if (pt.x < -180 * k_q || pt.x > 180 * k_q || pt.y < -90 * k_q || pt.y > 90 * k_q) return err(status::corrupt);
    }
    const std::size_t owners = li == 0 ? db.countries_.size() : db.regions_.size();
    for (const polygon& p : l.polys) {
      if (p.min_x > p.max_x || p.min_y > p.max_y || p.owner >= owners || p.ring_count == 0 ||
          p.first_ring >= l.rings.size() || p.ring_count >= l.rings.size() - p.first_ring ||
          p.first_point >= p.point_end || p.point_end > l.points.size() || l.rings[p.first_ring] != p.first_point ||
          l.rings[p.first_ring + p.ring_count] != p.point_end) {
        return err(status::corrupt);
      }
      for (std::uint32_t r = p.first_ring; r < p.first_ring + p.ring_count; ++r) {
        if (l.rings[r] > l.rings[r + 1] || l.rings[r + 1] - l.rings[r] < 2) return err(status::corrupt);
      }
      const std::uint64_t nb = static_cast<std::uint64_t>(band_of(p.max_y) - band_of(p.min_y)) + 1;
      if (p.band_base > l.bands.size() || nb + 1 > l.bands.size() - p.band_base) return err(status::corrupt);
      for (std::uint64_t b = p.band_base; b < p.band_base + nb; ++b) {
        if (l.bands[b] > l.bands[b + 1]) return err(status::corrupt);
      }
      const std::uint32_t e0 = l.bands[p.band_base];
      const std::uint32_t e1 = l.bands[p.band_base + nb];
      if (e1 > l.edges.size()) return err(status::corrupt);
      for (std::uint32_t e = e0; e < e1; ++e) {
        const std::uint32_t i = l.edges[e];
        if (i < p.first_point || std::uint64_t{i} + 1 >= p.point_end) return err(status::corrupt);
      }
    }
    for (std::uint32_t c : l.cands) {
      if ((c & ~k_full) >= l.polys.size()) return err(status::corrupt);
    }
    if (!valid_grid(l.index.rows, l.index.cells, l.cands.size())) return err(status::corrupt);
  }

  // Names people type, folded once. ~25 k names; a few milliseconds.
  try {
    const auto add = [&](std::uint32_t off, name_kind kind, std::size_t index) {
      std::string_view list = db.str(off);
      while (!list.empty()) {
        const std::size_t nl = list.find('\n');
        const std::string_view one = list.substr(0, nl);
        if (!one.empty()) db.names_.push_back({key_of(one), kind, static_cast<std::uint16_t>(index)});
        if (nl == std::string_view::npos) break;
        list.remove_prefix(nl + 1);
      }
    };
    for (std::size_t i = 0; i < db.countries_.size(); ++i) {
      const country& c = db.countries_[i];
      add(c.name, name_kind::country, i);
      add(c.native, name_kind::country, i);
      add(c.aliases, name_kind::country, i);  // codes ("PT") are not indexed: they match words
    }
    for (std::size_t i = 0; i < db.regions_.size(); ++i) {
      add(db.regions_[i].name, name_kind::region, i);
      add(db.regions_[i].aliases, name_kind::region, i);
    }
    std::sort(db.names_.begin(), db.names_.end(), [](const name_entry& a, const name_entry& b) {
      if (a.folded != b.folded) return a.folded < b.folded;
      if (a.kind != b.kind) return a.kind < b.kind;
      return a.index < b.index;
    });
    db.names_.erase(std::unique(db.names_.begin(), db.names_.end(),
                                [](const name_entry& a, const name_entry& b) {
                                  return a.folded == b.folded && a.kind == b.kind && a.index == b.index;
                                }),
                    db.names_.end());
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  return db;
}

// ---- helpers ------------------------------------------------------------------------------

std::string_view database::str(std::uint32_t offset) const noexcept {
  if (offset >= strings_.size()) return {};
  return std::string_view(strings_.data() + offset);  // validated: the pool ends in NUL
}

std::string database::country_code(std::uint16_t c) const {
  if (c >= countries_.size() || countries_[c].code[0] == '\0') return {};
  return std::string(countries_[c].code, 2);
}

std::span<const std::uint32_t> database::cell(const grid& g, int r, int c) const noexcept {
  const row& rw = g.rows[static_cast<std::size_t>(r)];
  const auto uc = static_cast<std::uint32_t>(c);
  if (uc < rw.col_begin || uc >= rw.col_end) return {};
  const std::size_t k = rw.base + (uc - rw.col_begin);
  return {g.cells.data() + k, 2};
}

bool database::contains(const poly_layer& l, std::uint32_t pi, std::int32_t x, std::int32_t y) const noexcept {
  const polygon& p = l.polys[pi];
  if (x < p.min_x || x > p.max_x || y < p.min_y || y > p.max_y) return false;
  const std::size_t b = p.band_base + static_cast<std::size_t>(band_of(y) - band_of(p.min_y));
  const std::uint32_t e0 = l.bands[b], e1 = l.bands[b + 1];
  const point* pts = l.points.data();
  const std::int64_t px = x, py = y;
  bool inside = false;
  for (std::uint32_t e = e0; e < e1; ++e) {
    const point a = pts[l.edges[e]];
    const point c = pts[l.edges[e] + 1];
    if ((a.y > y) == (c.y > y)) continue;
    // Exact in 64-bit: does the edge cross the ray from (x, y) towards +x?
    const std::int64_t lhs = (px - a.x) * (std::int64_t{c.y} - a.y);
    const std::int64_t rhs = (std::int64_t{c.x} - a.x) * (py - a.y);
    if (c.y > a.y ? lhs < rhs : lhs > rhs) inside = !inside;
  }
  return inside;
}

std::uint32_t database::find(const poly_layer& l, std::int32_t x, std::int32_t y) const noexcept {
  const auto run = cell(l.index, band_of(y), col_of(x));
  if (run.empty()) return k_none;
  for (std::uint32_t k = run[0]; k < run[1]; ++k) {
    const std::uint32_t c = l.cands[k];
    if (c & k_full) return c & ~k_full;
    if (contains(l, c, x, y)) return c;
  }
  return k_none;
}

database::nearest_hit database::nearest(const poly_layer& l, std::int32_t x, std::int32_t y, double km) const {
  nearest_hit best;
  const double lat = static_cast<double>(y) / k_q;
  const double kx = std::cos(lat * 3.14159265358979323846 / 180.0);
  const double ks = k_km_per_deg / k_q;  // km per unit, north-south
  const auto dy = static_cast<std::int64_t>(km / k_km_per_deg * k_q) + 1;
  const int r0 = band_of(y - dy), r1 = band_of(y + dy);
  int c0 = 0, ncols = 360;
  if (kx > 1e-3) {
    const double dx_deg = km / (k_km_per_deg * kx);
    if (dx_deg < 179) {
      c0 = static_cast<int>(std::floor((static_cast<double>(x) / k_q) - dx_deg + 180.0));
      const int c1 = static_cast<int>(std::floor((static_cast<double>(x) / k_q) + dx_deg + 180.0));
      ncols = std::min(360, c1 - c0 + 1);
    }
  }
  std::uint32_t seen[64];
  std::size_t nseen = 0;
  std::vector<std::uint32_t> overflow;
  const auto visit = [&](std::uint32_t pi) {
    for (std::size_t i = 0; i < nseen; ++i) {
      if (seen[i] == pi) return;
    }
    if (std::find(overflow.begin(), overflow.end(), pi) != overflow.end()) return;
    if (nseen < 64) {
      seen[nseen++] = pi;
    } else {
      overflow.push_back(pi);
    }
    const polygon& p = l.polys[pi];
    const int pb0 = band_of(p.min_y);
    const int b0 = std::max(r0, pb0), b1 = std::min(r1, band_of(p.max_y));
    const point* pts = l.points.data();
    for (int b = b0; b <= b1; ++b) {
      const std::size_t bi = p.band_base + static_cast<std::size_t>(b - pb0);
      for (std::uint32_t e = l.bands[bi]; e < l.bands[bi + 1]; ++e) {
        const point a = pts[l.edges[e]];
        const point c = pts[l.edges[e] + 1];
        const double ay = static_cast<double>(std::int64_t{a.y} - y) * ks;
        const double cy = static_cast<double>(std::int64_t{c.y} - y) * ks;
        if ((ay > km && cy > km) || (ay < -km && cy < -km)) continue;
        const double ax = static_cast<double>(wrap_dx(std::int64_t{a.x} - x)) * ks * kx;
        const double cx = static_cast<double>(wrap_dx(std::int64_t{c.x} - x)) * ks * kx;
        if ((ax > km && cx > km) || (ax < -km && cx < -km)) continue;
        const double d = seg_km(ax, ay, cx, cy);
        if (d <= km && (best.polygon == k_none || d < best.km)) {
          best.polygon = pi;
          best.km = d;
        }
      }
    }
  };
  for (int r = r0; r <= r1; ++r) {
    for (int k = 0; k < ncols; ++k) {
      const int c = ((c0 + k) % 360 + 360) % 360;
      const auto run = cell(l.index, r, c);
      if (run.empty()) continue;
      for (std::uint32_t i = run[0]; i < run[1]; ++i) visit(l.cands[i] & ~k_full);
    }
  }
  return best;
}

const town* database::pick_town(std::int32_t x, std::int32_t y, double km, std::uint16_t country_index,
                                std::uint16_t region_index, double& out_km) const noexcept {
  const town* nearest_t = nullptr;
  double nearest_km = km;
  const town* urban_t = nullptr;
  double urban_km = -1;
  double urban_depth = 2;  // d / reach of urban_t: smaller is deeper inside
  const double lat = static_cast<double>(y) / k_q;
  const double kx = std::cos(lat * 3.14159265358979323846 / 180.0);
  const double ks = k_km_per_deg / k_q;
  const auto dy = static_cast<std::int64_t>(km / k_km_per_deg * k_q) + 1;
  const int r0 = band_of(y - dy), r1 = band_of(y + dy);
  int c0 = 0, ncols = 360;
  if (kx > 1e-3) {
    const double dx_deg = km / (k_km_per_deg * kx);
    if (dx_deg < 179) {
      c0 = static_cast<int>(std::floor((static_cast<double>(x) / k_q) - dx_deg + 180.0));
      const int c1 = static_cast<int>(std::floor((static_cast<double>(x) / k_q) + dx_deg + 180.0));
      ncols = std::min(360, c1 - c0 + 1);
    }
  }
  constexpr double k_pi = 3.14159265358979323846;
  for (int r = r0; r <= r1; ++r) {
    for (int k = 0; k < ncols; ++k) {
      const int c = ((c0 + k) % 360 + 360) % 360;
      const auto run = cell(town_grid_, r, c);
      if (run.empty()) continue;
      for (std::uint32_t i = run[0]; i < run[1]; ++i) {
        const town& t = towns_[i];
        if (country_index != k_no_country && t.country != country_index) continue;
        if (region_index != k_no_region && t.region != k_no_region && t.region != region_index) continue;
        const double dyk = static_cast<double>(std::int64_t{t.y} - y) * ks;
        if (dyk > km || dyk < -km) continue;
        const double dxk = static_cast<double>(wrap_dx(std::int64_t{t.x} - x)) * ks * kx;
        const double d = std::sqrt(dxk * dxk + dyk * dyk);
        if (d > km) continue;
        // Ties (twin entries for one place) go to the larger population, which
        // the writer sorts first within a cell.
        if (d < nearest_km || (nearest_t == nullptr && d <= nearest_km)) {
          nearest_t = &t;
          nearest_km = d;
        }
        const double reach = std::sqrt(static_cast<double>(t.population) / (k_pi * k_urban_people_per_km2));
        // Inside several towns' reach (a metro area of adjacent cities): the
        // one the point is deepest inside, so central Manila is Manila, not
        // the larger Quezon City next door.
        if (reach > 0 && d <= reach && d / reach < urban_depth) {
          urban_t = &t;
          urban_km = d;
          urban_depth = d / reach;
        }
      }
    }
  }
  if (urban_t) {
    out_km = urban_km;
    return urban_t;
  }
  out_km = nearest_t ? nearest_km : -1;
  return nearest_t;
}

// ---- lookup -------------------------------------------------------------------------------

place database::lookup(double lat, double lon, const lookup_options& opts) const {
  place out;
  if (!valid() || !std::isfinite(lat) || !std::isfinite(lon)) return out;
  lat = std::clamp(lat, -90.0, 90.0);
  lon = std::fmod(lon + 180.0, 360.0);
  if (lon < 0) lon += 360.0;
  lon -= 180.0;
  const auto x = static_cast<std::int32_t>(std::clamp<long long>(std::llround(lon * k_q), -180LL * k_q,
                                                                 180LL * k_q - 1));
  const auto y = static_cast<std::int32_t>(std::clamp<long long>(std::llround(lat * k_q), -90LL * k_q,
                                                                 90LL * k_q));

  // Country: the admin-0 polygon; else, just off it, the nearest one (a
  // beach, a pier, an atoll); else at sea, off the nearest within 25 km.
  std::uint16_t ci = k_no_country;
  bool land = false;
  const std::uint32_t p0 = find(l0_, x, y);
  if (p0 != k_none) {
    ci = l0_.polys[p0].owner;
    land = true;
  } else {
    const nearest_hit hit = nearest(l0_, x, y, k_off_coast_km);
    if (hit.polygon != k_none) {
      ci = l0_.polys[hit.polygon].owner;
      land = hit.km <= k_coast_snap_km;
    }
    if (!land && hit.polygon != k_none) {
      double km = -1;
      if (const town* t = pick_town(x, y, k_town_land_km, k_no_country, k_no_region, km)) {
        ci = t->country;
        land = true;
      }
    }
  }

  // Region: the admin-1 polygon, or the nearest one just off it. A region
  // flagged k_region_country names the country too (Svalbard, Réunion).
  std::uint16_t ri = k_no_region;
  if (land) {
    std::uint32_t p1 = find(l1_, x, y);
    if (p1 == k_none) p1 = nearest(l1_, x, y, k_coast_snap_km).polygon;
    if (p1 != k_none) {
      const std::uint16_t cand = l1_.polys[p1].owner;
      const region& r = regions_[cand];
      if ((r.flags & k_region_country) && r.country != k_no_country) ci = r.country;
      if (r.country == ci) ri = cand;
    }
  }

  if (ci != k_no_country) {
    const country& c = countries_[ci];
    const std::string_view native = str(c.native);
    out.country = std::string(opts.native_names && !native.empty() ? native : str(c.name));
    out.country_code = country_code(ci);
  }

  if (!land) {
    out.at_sea = true;
    out.label = out.country.empty() ? "At sea" : "At sea, off " + out.country;
    return out;
  }

  if (ri != k_no_region) {
    out.region = std::string(str(regions_[ri].name));
    out.region_code = std::string(str(regions_[ri].code));
  }

  if (opts.town_radius_km > 0 && ci != k_no_country) {
    // Towns of the point's own region only, where the region is a GeoNames
    // one (a Natural Earth-only region has no towns filed under it).
    const std::uint16_t town_region = ri != k_no_region && !out.region_code.empty() ? ri : k_no_region;
    // Capped at half the globe: pick_town turns the radius into a grid span,
    // and an infinite or huge setting must not overflow that conversion.
    const double radius = std::min(opts.town_radius_km, 20'000.0);
    double km = -1;
    if (const town* t = pick_town(x, y, radius, ci, town_region, km)) {
      out.town = std::string(str(t->name));
      out.town_km = km;
      // No region polygon (an atoll narrower than the data): the town's own.
      if (ri == k_no_region && t->region != k_no_region && regions_[t->region].country == ci) {
        out.region = std::string(str(regions_[t->region].name));
        out.region_code = std::string(str(regions_[t->region].code));
      }
    }
  }

  // "Town, Region, Country", skipping what is empty or repeats its neighbour
  // ("Madrid, Spain", not "Madrid, Madrid, Spain"; "Singapore", once).
  const std::string_view parts[3] = {out.town, out.region, out.country};
  std::string last;
  for (std::string_view part : parts) {
    if (part.empty()) continue;
    std::string key = key_of(part);
    if (key == last) continue;
    if (!out.label.empty()) out.label += ", ";
    out.label += part;
    last = std::move(key);
  }
  return out;
}

// ---- names --------------------------------------------------------------------------------

std::vector<name_match> database::match(std::string_view text, bool prefix) const {
  std::vector<name_match> out;
  const std::string q = key_of(text);
  if (q.empty() || names_.empty()) return out;
  auto it = std::lower_bound(names_.begin(), names_.end(), q,
                             [](const name_entry& e, const std::string& v) { return e.folded < v; });
  struct seen_t {
    name_kind kind;
    std::uint16_t index;
  };
  std::vector<seen_t> seen;
  for (; it != names_.end(); ++it) {
    const bool exact = it->folded == q;
    if (!exact && !(prefix && it->folded.compare(0, q.size(), q) == 0)) break;
    const bool dup = std::any_of(seen.begin(), seen.end(),
                                 [&](const seen_t& s) { return s.kind == it->kind && s.index == it->index; });
    // Sorted by folded name, so every exact hit comes before the prefix hits.
    if (dup) continue;
    seen.push_back({it->kind, it->index});
    name_match m;
    m.kind = it->kind;
    m.exact = exact;
    if (it->kind == name_kind::country) {
      m.country_code = country_code(it->index);
      m.name = std::string(str(countries_[it->index].name));
    } else {
      const region& r = regions_[it->index];
      m.country_code = country_code(r.country);
      m.region_code = std::string(str(r.code));
      m.name = std::string(str(r.name));
    }
    out.push_back(std::move(m));
  }
  // Exact matches first, then countries before regions.
  std::stable_sort(out.begin(), out.end(), [](const name_match& a, const name_match& b) {
    if (a.exact != b.exact) return a.exact;
    return a.kind < b.kind;
  });
  return out;
}

// ---- polygons -----------------------------------------------------------------------------

std::vector<std::uint32_t> database::polygons_in(layer which, const box& b) const {
  std::vector<std::uint32_t> out;
  const poly_layer& l = layer_of(which);
  if (!std::isfinite(b.min_lat) || !std::isfinite(b.max_lat) || !std::isfinite(b.min_lon) ||
      !std::isfinite(b.max_lon)) {
    return out;
  }
  // Clamped before the integer conversion, which is undefined out of range.
  const auto q = [](double v, double lim) { return std::clamp(v, -lim, lim) * k_q; };
  const auto qy0 = static_cast<std::int64_t>(std::floor(q(b.min_lat, 90)));
  const auto qy1 = static_cast<std::int64_t>(std::ceil(q(b.max_lat, 90)));
  const auto qx0 = static_cast<std::int64_t>(std::floor(q(b.min_lon, 180)));
  const auto qx1 = static_cast<std::int64_t>(std::ceil(q(b.max_lon, 180)));
  const bool wraps = b.min_lon > b.max_lon;
  for (std::uint32_t i = 0; i < l.polys.size(); ++i) {
    const polygon& p = l.polys[i];
    if (p.max_y < qy0 || p.min_y > qy1) continue;
    const bool hit_x = wraps ? (p.max_x >= qx0 || p.min_x <= qx1) : (p.max_x >= qx0 && p.min_x <= qx1);
    if (hit_x) out.push_back(i);
  }
  return out;
}

std::size_t database::polygon_count(layer which) const noexcept { return layer_of(which).polys.size(); }

std::size_t database::ring_count(layer which, std::uint32_t pi) const noexcept {
  const poly_layer& l = layer_of(which);
  return pi < l.polys.size() ? l.polys[pi].ring_count : 0;
}

std::span<const point> database::ring(layer which, std::uint32_t pi, std::size_t i) const noexcept {
  const poly_layer& l = layer_of(which);
  if (pi >= l.polys.size() || i >= l.polys[pi].ring_count) return {};
  const std::size_t r = l.polys[pi].first_ring + i;
  return l.points.subspan(l.rings[r], l.rings[r + 1] - l.rings[r]);
}

std::string_view database::polygon_country_code(layer which, std::uint32_t pi) const noexcept {
  const poly_layer& l = layer_of(which);
  if (pi >= l.polys.size()) return {};
  std::uint16_t c = l.polys[pi].owner;
  if (which == layer::regions) c = regions_[c].country;
  if (c >= countries_.size() || countries_[c].code[0] == '\0') return {};
  return std::string_view(countries_[c].code, 2);
}

}  // namespace mv::geo
