// SPDX-License-Identifier: GPL-2.0-or-later
#include "catch_compat.h"

#include "core/json.h"

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
