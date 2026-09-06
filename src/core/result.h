// SPDX-License-Identifier: GPL-2.0-or-later
// result<T> — the no-exceptions return type for the hot path.
//
// CLAUDE.md: "No exceptions on the hot path (std::expected-style results)."
// std::expected is C++23; this is the C++20 subset we actually use. It is a
// deliberately small surface: construct, test, unwrap, map the error out. If
// you find yourself wanting monadic chaining, take std::expected instead when
// the toolchain floor moves.
#pragma once

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "core/status.h"

namespace mv {

// Tag for constructing an error result without naming T.
struct error_t {
  status code;
};

[[nodiscard]] constexpr error_t err(status code) noexcept { return error_t{code}; }

template <typename T>
class result {
  static_assert(!std::is_reference_v<T>, "result<T&> is not supported; use a pointer");
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow-destructible");

 public:
  using value_type = T;

  // Value construction.
  result(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) : code_(status::ok) {
    ::new (storage()) T(std::move(v));
  }
  result(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) : code_(status::ok) {
    ::new (storage()) T(v);
  }

  // Error construction: `return mv::err(mv::status::io);`
  result(error_t e) noexcept : code_(e.code) {
    // A result must never hold status::ok with no value.
    if (e.code == status::ok) code_ = status::internal;
  }

  result(result&& other) noexcept(std::is_nothrow_move_constructible_v<T>) : code_(other.code_) {
    if (has_value()) ::new (storage()) T(std::move(*other.storage()));
  }

  result& operator=(result&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
    if (this == &other) return *this;
    destroy();
    code_ = other.code_;
    if (has_value()) ::new (storage()) T(std::move(*other.storage()));
    return *this;
  }

  result(const result&) = delete;
  result& operator=(const result&) = delete;

  ~result() { destroy(); }

  [[nodiscard]] bool has_value() const noexcept { return code_ == status::ok; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] status error() const noexcept { return code_; }

  // Precondition: has_value(). Checked in debug builds only — this is the hot path.
  [[nodiscard]] T& value() & noexcept { return *storage(); }
  [[nodiscard]] const T& value() const& noexcept { return *storage(); }
  [[nodiscard]] T&& value() && noexcept { return std::move(*storage()); }

  T* operator->() noexcept { return storage(); }
  const T* operator->() const noexcept { return storage(); }
  T& operator*() & noexcept { return *storage(); }
  T&& operator*() && noexcept { return std::move(*storage()); }

  [[nodiscard]] T value_or(T fallback) && noexcept {
    return has_value() ? std::move(*storage()) : std::move(fallback);
  }

 private:
  T* storage() noexcept { return reinterpret_cast<T*>(&buf_); }
  const T* storage() const noexcept { return reinterpret_cast<const T*>(&buf_); }

  void destroy() noexcept {
    if (has_value()) storage()->~T();
    code_ = status::internal;
  }

  alignas(T) std::byte buf_[sizeof(T)];
  status code_;
};

// result<void> — a status with a nicer name at the call site.
template <>
class result<void> {
 public:
  result() noexcept : code_(status::ok) {}
  result(error_t e) noexcept : code_(e.code) {}

  [[nodiscard]] bool has_value() const noexcept { return code_ == status::ok; }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] status error() const noexcept { return code_; }

 private:
  status code_;
};

using expected = result<void>;

// `MV_TRY(decl, expr)` — propagate the error, bind the value. Statement form,
// because C++20 has no expression-level try. Deliberately not a fancy macro:
// it expands once, evaluates `expr` once, and is greppable.
#define MV_DETAIL_CAT2(a, b) a##b
#define MV_DETAIL_CAT(a, b) MV_DETAIL_CAT2(a, b)
#define MV_TRY(decl, expr)                                                auto&& MV_DETAIL_CAT(mv_try_, __LINE__) = (expr);                       if (!MV_DETAIL_CAT(mv_try_, __LINE__))                                    return ::mv::err(MV_DETAIL_CAT(mv_try_, __LINE__).error());           decl = std::move(MV_DETAIL_CAT(mv_try_, __LINE__)).value()

// `MV_TRY_VOID(expr)` — propagate the error of a result<void>.
#define MV_TRY_VOID(expr)                                                 do {                                                                      auto&& MV_DETAIL_CAT(mv_tryv_, __LINE__) = (expr);                      if (!MV_DETAIL_CAT(mv_tryv_, __LINE__))                                   return ::mv::err(MV_DETAIL_CAT(mv_tryv_, __LINE__).error());        } while (0)

}  // namespace mv
