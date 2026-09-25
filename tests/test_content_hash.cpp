// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_compat.h"

#include <string>
#include <vector>

#include "io/content_hash.h"

using mv::io::content_hash;

TEST_CASE("BLAKE3-256 matches the reference vectors", "[io][hash]") {
  // BLAKE3 test_vectors.json: input_len 0 and 1 (byte 0x00), default hash.
  REQUIRE(mv::io::hash_bytes({}).hex() ==
          "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
  const std::uint8_t zero[1] = {0};
  REQUIRE(mv::io::hash_bytes(zero).hex() ==
          "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213");
}

TEST_CASE("incremental hashing equals one-shot, and finish resets", "[io][hash]") {
  std::vector<std::uint8_t> data(100000);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i % 251);
  mv::io::hasher h;
  h.update(std::span<const std::uint8_t>(data.data(), 33333));
  h.update(std::span<const std::uint8_t>(data.data() + 33333, data.size() - 33333));
  const content_hash a = h.finish();
  REQUIRE(a == mv::io::hash_bytes(data));
  h.update(data);
  REQUIRE(h.finish() == a);
}

TEST_CASE("hex round-trips and rejects anything but 64 hex digits", "[io][hash]") {
  const content_hash a = mv::io::hash_bytes({});
  content_hash b;
  REQUIRE(content_hash::from_hex(a.hex(), b));
  REQUIRE(a == b);
  REQUIRE_FALSE(content_hash::from_hex("abc", b));
  REQUIRE_FALSE(content_hash::from_hex(std::string(63, 'a') + "g", b));
}
