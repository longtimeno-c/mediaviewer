// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

#include "core/result.h"

using mv::err;
using mv::result;
using mv::status;

TEST_CASE("result<T> carries a value", "[core][result]") {
  result<int> r{42};
  REQUIRE(r.has_value());
  REQUIRE(static_cast<bool>(r));
  REQUIRE(r.value() == 42);
  REQUIRE(r.error() == status::ok);
}

TEST_CASE("result<T> carries an error and no value", "[core][result]") {
  result<int> r = err(status::io);
  REQUIRE_FALSE(r.has_value());
  REQUIRE_FALSE(static_cast<bool>(r));
  REQUIRE(r.error() == status::io);
}

TEST_CASE("err(ok) is rejected rather than silently accepted", "[core][result]") {
  // A result that reports success with no value is the bug this guards
  // against: every caller would then read uninitialised storage.
  result<int> r = err(status::ok);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == status::internal);
}

TEST_CASE("result<T> destroys its value exactly once", "[core][result]") {
  struct counter {
    int* destructions;
    explicit counter(int* d) noexcept : destructions(d) {}
    counter(counter&& other) noexcept : destructions(other.destructions) {
      other.destructions = nullptr;
    }
    counter& operator=(counter&&) = delete;
    ~counter() {
      if (destructions) ++*destructions;
    }
  };

  int destructions = 0;
  {
    result<counter> a{counter{&destructions}};
    result<counter> b = std::move(a);
    REQUIRE(b.has_value());
  }
  REQUIRE(destructions == 1);
}

TEST_CASE("result<T> moves a move-only value out", "[core][result]") {
  result<std::unique_ptr<int>> r{std::make_unique<int>(7)};
  REQUIRE(r.has_value());
  auto owned = std::move(r).value();
  REQUIRE(owned != nullptr);
  REQUIRE(*owned == 7);
}

TEST_CASE("result<void> is a status with a better name", "[core][result]") {
  mv::expected ok_case;
  REQUIRE(ok_case.has_value());
  REQUIRE(ok_case.error() == status::ok);

  mv::expected fail = err(status::device_lost);
  REQUIRE_FALSE(fail.has_value());
  REQUIRE(fail.error() == status::device_lost);
}

namespace {

result<int> parse_positive(int value) {
  if (value <= 0) return err(mv::status::invalid_arg);
  return value * 2;
}

result<std::string> describe(int value) {
  MV_TRY(const int doubled, parse_positive(value));
  return std::string("ok:") + std::to_string(doubled);
}

}  // namespace

TEST_CASE("MV_TRY propagates the error and binds the value", "[core][result]") {
  auto good = describe(21);
  REQUIRE(good.has_value());
  REQUIRE(good.value() == "ok:42");

  auto bad = describe(-1);
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error() == status::invalid_arg);
}

TEST_CASE("MV_TRY evaluates its expression exactly once", "[core][result]") {
  // A macro that expands its argument twice turns a single decode into two.
  int calls = 0;
  const auto counted = [&]() -> result<int> {
    ++calls;
    return 1;
  };

  const auto run = [&]() -> result<int> {
    MV_TRY(const int v, counted());
    return v;
  };

  REQUIRE(run().value() == 1);
  REQUIRE(calls == 1);
}
