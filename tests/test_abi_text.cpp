// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/abi_text.h: a core call that changes something is never repeated to size a buffer.
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "shell/abi_text.h"

using mv::shell::abi_text_once;

TEST_CASE("abi_text: the answer of one call, whole", "[abi_text]") {
  int calls = 0;
  const auto r = abi_text_once([&](char* buf, std::uint32_t cap, std::uint32_t* need) -> std::int32_t {
    ++calls;
    const char* key = "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/Trip/a.jpg";
    *need = static_cast<std::uint32_t>(std::strlen(key) + 1);
    if (cap < *need) return 1;
    std::memcpy(buf, key, *need);
    return 0;
  });
  CHECK(calls == 1);
  CHECK(r.status == 0);
  CHECK_FALSE(r.cut);
  CHECK(r.text == "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/Trip/a.jpg");
}

TEST_CASE("abi_text: an answer longer than the buffer is never asked for again", "[abi_text]") {
  int uploads = 0;
  // As mv_remote_add since 0.45: the change is done, the key cut to fit, MV_OK all the same.
  const auto r = abi_text_once([&](char* buf, std::uint32_t cap, std::uint32_t* need) -> std::int32_t {
    ++uploads;
    const std::string key(mv::shell::kAbiTextBytes + 500, 'k');
    *need = static_cast<std::uint32_t>(key.size() + 1);
    std::memcpy(buf, key.data(), cap - 1);
    buf[cap - 1] = '\0';
    return 0;
  });
  CHECK(uploads == 1);
  CHECK(r.status == 0);
  CHECK(r.cut);
  CHECK(r.text.empty());  // not a key: the caller knows it went, not where
}

TEST_CASE("abi_text: a refusal is passed on, with no text and no second call", "[abi_text]") {
  int calls = 0;
  const auto r = abi_text_once([&](char*, std::uint32_t, std::uint32_t* need) -> std::int32_t {
    ++calls;
    *need = 0;
    return 11;  // MV_ERR_PERMISSION_DENIED
  });
  CHECK(calls == 1);
  CHECK(r.status == 11);
  CHECK(r.text.empty());
}
