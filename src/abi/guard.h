// SPDX-License-Identifier: GPL-2.0-or-later
// mv_guard — the exception boundary.
//
// plan/14-abi.md: "No exception may cross the boundary. An exception escaping
// into managed code through P/Invoke is undefined behaviour, and it will not
// look like the bug it is. The mv_guard wrapper is not optional politeness —
// it is the boundary."
//
// Every exported function in the ABI is exactly this shape:
//
//   extern "C" mv_status MV_CALL mv_thing_do(...) noexcept {
//     return mv::abi::guard("mv_thing_do", [&] { ... return status::ok; });
//   }
//
// The guard also stamps the thread-local last-error slot, which is what ties a
// managed MediaViewerException back to the native minidump that caused it.
#pragma once

#include <cstdint>
#include <exception>
#include <utility>

#include "core/status.h"
#include "core/trace.h"

namespace mv::abi {

// Thread-local last-error detail. Valid until the next ABI call on this
// thread — the ownership rule the C# side must respect by copying immediately.
//
// begin_call() opens a call: it clears the message and installs the correlation
// id that MV_REQUIRE and any failure inside the call will be stamped with.
void begin_call(std::uint64_t correlation_id) noexcept;
void set_last_error(std::uint64_t correlation_id, const char* message) noexcept;
[[nodiscard]] const char* last_error_message() noexcept;
[[nodiscard]] std::uint64_t last_error_correlation_id() noexcept;

// The correlation id of the call currently executing on this thread.
[[nodiscard]] std::uint64_t current_correlation_id() noexcept;

// Monotonic, process-wide. Every ABI call gets one so a managed failure can be
// tied to the native failure that produced it.
[[nodiscard]] std::uint64_t next_correlation_id() noexcept;

// `fn` returns mv::status. `function_literal` must outlive the call — pass a
// string literal, never a computed name.
template <typename Fn>
status guard(const char* function_literal, Fn&& fn) noexcept {
  const std::uint64_t correlation = next_correlation_id();
  begin_call(correlation);
  trace::abi_call(function_literal, correlation);

  status result = status::internal;
  try {
    result = std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    result = status::out_of_memory;
    set_last_error(correlation, "out of memory");
  } catch (const std::exception& e) {
    result = status::internal;
    set_last_error(correlation, e.what());
  } catch (...) {
    // Including SEH translated by the CRT, and anything a dependency throws
    // that is not derived from std::exception. Nothing gets past here.
    result = status::internal;
    set_last_error(correlation, "unknown exception at the ABI boundary");
  }

  if (result != status::ok) {
    if (last_error_message()[0] == '\0') set_last_error(correlation, status_name(result));
    trace::abi_fault(function_literal, correlation, static_cast<std::int32_t>(result));
  }
  return result;
}

// Argument validation that also records why, so mv_last_error_message() says
// something more useful than "INVALID_ARG".
#define MV_REQUIRE(cond, message)                                              \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ::mv::abi::set_last_error(::mv::abi::current_correlation_id(),         \
                                (message));                                    \
      return ::mv::status::invalid_arg;                                        \
    }                                                                          \
  } while (0)

}  // namespace mv::abi
