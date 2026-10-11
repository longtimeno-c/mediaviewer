// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The place database's on-disk format (docs/design/06 "Place"). The writer is
// tools/geo/build_places.py; keep the two in step and bump k_version when
// either changes. Everything is little-endian and read in place from a
// memory-mapped file, so every record is a POD with a pinned size.
//
//   header (64 bytes) | section table (24 bytes each) | sections, 8-aligned
//
// The checksum covers every byte after the header. Coordinates are int32
// microdegrees: x = longitude, y = latitude. The index grid is 1 degree:
// band/row r covers latitude [r - 90, r - 89), column c longitude
// [c - 180, c - 179).
#pragma once

#include <cstdint>

namespace mv::geo::blob {

inline constexpr char k_magic[8] = {'M', 'V', 'G', 'E', 'O', '\x1a', '\0', '\0'};
inline constexpr std::uint32_t k_version = 1;
inline constexpr std::int32_t k_q = 1'000'000;  // units per degree
inline constexpr std::uint32_t k_max_sections = 64;
inline constexpr std::uint32_t k_full = 0x8000'0000u;  // candidate flag: cell wholly inside
inline constexpr std::uint16_t k_no_region = 0xFFFF;
inline constexpr std::uint16_t k_no_country = 0xFFFF;

struct header {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_bytes;  // 64
  std::uint64_t file_bytes;
  std::uint64_t checksum;  // of bytes [header_bytes, file_bytes)
  std::uint32_t section_count;
  std::uint32_t flags;
  std::uint8_t reserved[24];
};
static_assert(sizeof(header) == 64);

struct section {
  std::uint32_t id;
  std::uint32_t elem_bytes;
  std::uint64_t offset;
  std::uint64_t bytes;
};
static_assert(sizeof(section) == 24);

enum section_id : std::uint32_t {
  strings = 1,     // UTF-8, NUL-terminated; offset 0 is ""
  countries = 2,   // country[]
  regions = 3,     // region[]
  towns = 4,       // town[], sorted by grid cell
  town_rows = 5,   // row[180]
  town_cells = 6,  // u32 offsets into towns
  meta = 7,        // JSON: inputs, their SHA-256, attribution
  layer_countries = 16,  // + layer_part: admin-0 polygons, owner = country index
  layer_regions = 32,    // + layer_part: admin-1 polygons, owner = region index
};

enum layer_part : std::uint32_t {
  polys = 0,   // polygon[]
  rings = 1,   // u32 ring start (point index), with a trailing sentinel
  points = 2,  // point[], every ring closed (last == first)
  bands = 3,   // u32 offsets into edges: per polygon, one per 1-degree band it spans, + 1
  edges = 4,   // u32 point index i: the edge points[i] -> points[i + 1]
  rows = 5,    // row[180]
  cells = 6,   // u32 offsets into cands
  cands = 7,   // u32 polygon index, | k_full when the cell lies wholly inside it
};

struct country {
  char code[2];  // ISO 3166-1 alpha-2, or two NULs for an area with none
  std::uint16_t flags;
  std::uint32_t name;    // English (GeoNames)
  std::uint32_t native;  // in the country's first language, or "" when the same / unknown
  std::uint32_t aliases;  // '\n'-separated names people type (Natural Earth's ~25 languages)
};
static_assert(sizeof(country) == 16);

struct region {
  std::uint16_t country;  // index, or k_no_country
  std::uint16_t flags;
  std::uint32_t code;  // GeoNames admin-1 code ("PT.14"), "" for a Natural Earth-only region
  std::uint32_t name;
  std::uint32_t aliases;
};
static_assert(sizeof(region) == 16);

struct town {
  std::int32_t y;  // latitude, microdegrees
  std::int32_t x;  // longitude
  std::uint32_t name;
  std::uint32_t population;
  std::uint16_t country;
  std::uint16_t region;  // or k_no_region
};
static_assert(sizeof(town) == 20);

struct row {
  std::uint32_t col_begin;  // columns [col_begin, col_end) have entries
  std::uint32_t col_end;
  std::uint32_t base;  // cells[base + (c - col_begin)] .. cells[... + 1] bound cell c
};
static_assert(sizeof(row) == 12);

struct point {
  std::int32_t x;  // longitude, microdegrees
  std::int32_t y;  // latitude
};
static_assert(sizeof(point) == 8);

struct polygon {
  std::int32_t min_x, min_y, max_x, max_y;
  std::uint32_t first_ring;
  std::uint32_t ring_count;
  std::uint32_t band_base;  // into bands
  std::uint16_t owner;
  std::uint16_t flags;
  std::uint32_t first_point;
  std::uint32_t point_end;
};
static_assert(sizeof(polygon) == 40);

}  // namespace mv::geo::blob
