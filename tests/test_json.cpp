// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_compat.h"

#include "core/json.h"

#include <string>
#include <string_view>

TEST_CASE("the JSON reader is strict", "[json]") {
  REQUIRE(mv::json::parse(R"({"a":1,"b":[true,false,null],"c":"xé😀"})"));
  REQUIRE_FALSE(mv::json::parse(R"({"a":1,})"));           // trailing comma
  REQUIRE_FALSE(mv::json::parse(R"({"a":1,"a":2})"));      // duplicate key
  REQUIRE_FALSE(mv::json::parse(R"({"a":01})"));           // leading zero
  REQUIRE_FALSE(mv::json::parse(R"(// c
{})"));
  REQUIRE_FALSE(mv::json::parse(R"(["\ud800"])"));         // lone surrogate
  REQUIRE_FALSE(mv::json::parse("[\"a\x01\"]"));            // raw control character
  REQUIRE_FALSE(mv::json::parse("{} {}"));
  REQUIRE_FALSE(mv::json::parse(std::string(100, '[') + std::string(100, ']')));  // depth
  REQUIRE_FALSE(mv::json::parse("[99999999999999999999]"));  // out of int64
  // Malformed UTF-8: another parser would reject it, so this one does too.
  REQUIRE_FALSE(mv::json::parse("[\"\xC3\"]"));              // truncated sequence
  REQUIRE_FALSE(mv::json::parse("[\"\xC0\xAF\"]"));          // overlong '/'
  REQUIRE_FALSE(mv::json::parse("[\"\xED\xA0\x80\"]"));      // encoded surrogate
  REQUIRE_FALSE(mv::json::parse("[\"\xF4\x90\x80\x80\"]"));  // past U+10FFFF
  REQUIRE_FALSE(mv::json::parse("[\"\xFF\"]"));

  const auto v = mv::json::parse(R"({"n":-12,"s":"q\"\\/","f":1.5e2,"t":true})");
  REQUIRE(v);
  REQUIRE(*v->integer("n") == -12);
  REQUIRE(*v->str("s") == "q\"\\/");
  REQUIRE(v->find("f")->d == 150.0);
  REQUIRE_FALSE(v->find("f")->is_integer);
  REQUIRE(*v->boolean("t"));
  REQUIRE(v->find("missing") == nullptr);
  REQUIRE(*mv::json::parse(R"(["é"])")->a[0].s.c_str() == '\xC3');
}

TEST_CASE("the JSON reader rejects a duplicate key in a large object", "[json]") {
  // Past 16 keys the check sorts instead of scanning (a tokenizer vocab.json
  // has ~50k keys); the strictness must be the same on both paths.
  const auto object = [](int n, std::string_view extra) {
    std::string text = "{";
    for (int i = 0; i < n; ++i) text += "\"k" + std::to_string(i) + "\":" + std::to_string(i) + ",";
    text += extra;
    text += "}";
    return text;
  };
  for (const int n : {15, 16, 17, 50000}) {
    const auto ok = mv::json::parse(object(n, R"("last":0)"));
    REQUIRE(ok);
    REQUIRE(ok->o.size() == static_cast<std::size_t>(n) + 1);
    REQUIRE(*ok->integer("last") == 0);
    REQUIRE_FALSE(mv::json::parse(object(n, R"("k0":0)")));               // first key again
    REQUIRE_FALSE(mv::json::parse(object(n, "\"k" + std::to_string(n - 1) + "\":0")));  // last
    REQUIRE_FALSE(mv::json::parse(object(n, R"("\u006b0":0)")));         // same key, escaped
  }
  // A large object nested inside a small one is still checked.
  REQUIRE_FALSE(mv::json::parse("[" + object(100, R"("k42":0)") + "]"));
  REQUIRE(mv::json::parse(R"({"a":)" + object(100, R"("z":0)") + "}"));
}

TEST_CASE("the JSON writer round-trips through the reader", "[json]") {
  mv::json::writer w;
  w.begin_object()
      .key("name").string("a \"quoted\"\n\ttab \x01")
      .key("list").begin_array().integer(1).integer(-2).boolean(false).null().end_array()
      .key("rate").number(110.5)
      .key("nested").begin_object().key("x").raw("[1,2]").end_object()
      .end_object();
  const auto v = mv::json::parse(w.str());
  REQUIRE(v);
  REQUIRE(*v->str("name") == "a \"quoted\"\n\ttab \x01");
  REQUIRE(v->find("list")->a.size() == 4);
  REQUIRE(v->find("rate")->d == 110.5);
  REQUIRE(v->find("nested")->find("x")->a.size() == 2);
}
