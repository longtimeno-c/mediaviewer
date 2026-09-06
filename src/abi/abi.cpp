// SPDX-License-Identifier: GPL-2.0-or-later
// The C ABI implementation. Every exported symbol in this file is `noexcept`
// and does its real work inside mv::abi::guard.

#include "mediaviewer/mediaviewer.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "abi/guard.h"
#include "core/job_system.h"
#include "core/spsc_ring.h"
#include "core/status.h"
#include "core/trace.h"

// The enum values on both sides of the line must stay numerically identical.
// If someone reorders mv::status, this stops the build rather than shipping a
// core that reports IO errors as corruption.
static_assert(static_cast<int>(mv::status::ok) == MV_OK, "mv_status drift");
static_assert(static_cast<int>(mv::status::invalid_arg) == MV_ERR_INVALID_ARG, "mv_status drift");
static_assert(static_cast<int>(mv::status::out_of_memory) == MV_ERR_OUT_OF_MEMORY,
              "mv_status drift");
static_assert(static_cast<int>(mv::status::io) == MV_ERR_IO, "mv_status drift");
static_assert(static_cast<int>(mv::status::unsupported_format) == MV_ERR_UNSUPPORTED_FORMAT,
              "mv_status drift");
static_assert(static_cast<int>(mv::status::corrupt) == MV_ERR_CORRUPT, "mv_status drift");
static_assert(static_cast<int>(mv::status::cancelled) == MV_ERR_CANCELLED, "mv_status drift");
static_assert(static_cast<int>(mv::status::device_lost) == MV_ERR_DEVICE_LOST, "mv_status drift");
static_assert(static_cast<int>(mv::status::internal) == MV_ERR_INTERNAL, "mv_status drift");

static_assert(sizeof(mv_completion) == 40, "mv_completion layout is part of the ABI");
static_assert(alignof(mv_completion) == 8, "mv_completion layout is part of the ABI");

namespace mv::abi {

namespace {

constexpr std::size_t last_error_capacity = 512;

struct thread_error_state {
  std::uint64_t current_correlation = 0;
  std::uint64_t failed_correlation = 0;
  char message[last_error_capacity] = {0};
};

thread_local thread_error_state t_error;
std::atomic<std::uint64_t> g_next_correlation{1};

}  // namespace

std::uint64_t next_correlation_id() noexcept {
  return g_next_correlation.fetch_add(1, std::memory_order_relaxed);
}

void begin_call(std::uint64_t correlation_id) noexcept {
  t_error.current_correlation = correlation_id;
  t_error.message[0] = '\0';
}

void set_last_error(std::uint64_t correlation_id, const char* message) noexcept {
  t_error.failed_correlation = correlation_id;
  if (message == nullptr) {
    t_error.message[0] = '\0';
    return;
  }
  const std::size_t n = std::strlen(message);
  const std::size_t copy = n < last_error_capacity - 1 ? n : last_error_capacity - 1;
  std::memcpy(t_error.message, message, copy);
  t_error.message[copy] = '\0';
}

const char* last_error_message() noexcept { return t_error.message; }
std::uint64_t last_error_correlation_id() noexcept { return t_error.failed_correlation; }
std::uint64_t current_correlation_id() noexcept { return t_error.current_correlation; }

}  // namespace mv::abi

// ---------------------------------------------------------------------------
// The session object. Opaque to the caller; `mv_session_t` is a pointer to it.
// ---------------------------------------------------------------------------
struct mv_session {
  std::atomic<std::uint32_t> ref_count{1};
  mv::job_system jobs;

  // Completions are produced by many worker threads and consumed by one
  // draining thread, so the SPSC ring is not the right shape here — this is the
  // one MPMC-ish edge in the design and it takes a short-held mutex rather than
  // pretending otherwise. The lock is never held across a call we do not own,
  // and never touched by the render thread except in the drain.
  std::mutex completion_mutex;
  std::vector<mv_completion> completions;
  HANDLE completion_event = nullptr;

  void push_completion(const mv_completion& c) noexcept {
    {
      std::lock_guard lock(completion_mutex);
      completions.push_back(c);
    }
    if (completion_event) ::SetEvent(completion_event);
  }
};

namespace {

using mv::abi::guard;
using mv::status;

// `session` is validated by every entry point before use. There is no way to
// tell a stale pointer from a live one across an ABI, so this checks only for
// null — the SafeHandle on the managed side is what actually prevents
// use-after-free, which is why plan/14 makes it non-negotiable.
constexpr bool valid(mv_session_t s) noexcept { return s != nullptr; }

}  // namespace

extern "C" {

uint32_t MV_CALL mv_abi_version(void) {
  return (static_cast<uint32_t>(MV_ABI_VERSION_MAJOR) << 16) |
         static_cast<uint32_t>(MV_ABI_VERSION_MINOR);
}

const char* MV_CALL mv_status_name(mv_status s) {
  return mv::status_name(static_cast<mv::status>(s));
}

const char* MV_CALL mv_last_error_message(void) { return mv::abi::last_error_message(); }

uint64_t MV_CALL mv_last_error_correlation_id(void) {
  return mv::abi::last_error_correlation_id();
}

mv_status MV_CALL mv_session_create(const mv_session_config* config, mv_session_t* out_session) {
  return static_cast<mv_status>(guard("mv_session_create", [&]() -> status {
    MV_REQUIRE(out_session != nullptr, "out_session must not be null");
    *out_session = nullptr;

    const uint32_t workers = config ? config->worker_count : 0;
    const bool etw = config ? config->enable_etw != 0 : true;

    if (etw) mv::trace::provider_register();

    auto session = std::make_unique<mv_session>();

    session->completion_event = ::CreateEventW(nullptr, TRUE /*manual reset*/,
                                               FALSE /*non-signalled*/, nullptr);
    if (!session->completion_event) return status::internal;

    session->completions.reserve(256);

    const status started = session->jobs.start(workers);
    if (started != status::ok) {
      ::CloseHandle(session->completion_event);
      return started;
    }

    *out_session = session.release();
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_retain(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_session_retain", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    session->ref_count.fetch_add(1, std::memory_order_relaxed);
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_release(mv_session_t session) {
  return static_cast<mv_status>(guard("mv_session_release", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    if (session->ref_count.fetch_sub(1, std::memory_order_acq_rel) != 1) return status::ok;

    // Last reference. Shut the pool down first: in-flight jobs hold a raw
    // pointer to this session and must all be joined before it dies.
    session->jobs.shutdown();
    if (session->completion_event) ::CloseHandle(session->completion_event);
    delete session;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_bump_generation(mv_session_t session, uint32_t* out_generation) {
  return static_cast<mv_status>(guard("mv_session_bump_generation", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    const uint32_t gen = session->jobs.bump_generation();
    if (out_generation) *out_generation = gen;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_current_generation(mv_session_t session, uint32_t* out_generation) {
  return static_cast<mv_status>(guard("mv_session_current_generation", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_generation != nullptr, "out_generation must not be null");
    *out_generation = session->jobs.current_generation();
    return status::ok;
  }));
}

void* MV_CALL mv_completion_wait_handle(mv_session_t session) {
  if (!valid(session)) return nullptr;
  return session->completion_event;
}

uint32_t MV_CALL mv_completion_drain(mv_session_t session, mv_completion* out, uint32_t capacity) {
  if (!valid(session) || out == nullptr || capacity == 0) return 0;

  std::lock_guard lock(session->completion_mutex);
  const auto available = static_cast<uint32_t>(session->completions.size());
  const uint32_t count = available < capacity ? available : capacity;
  if (count == 0) {
    ::ResetEvent(session->completion_event);
    return 0;
  }

  std::memcpy(out, session->completions.data(), count * sizeof(mv_completion));
  session->completions.erase(session->completions.begin(),
                             session->completions.begin() + static_cast<std::ptrdiff_t>(count));
  // Reset only when the queue is genuinely empty, so a partial drain leaves the
  // caller a reason to come back.
  if (session->completions.empty()) ::ResetEvent(session->completion_event);
  return count;
}

mv_status MV_CALL mv_session_echo(mv_session_t session, const char* utf8_text,
                                  uint64_t* out_job_id) {
  return static_cast<mv_status>(guard("mv_session_echo", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(utf8_text != nullptr, "utf8_text must not be null");

    // The caller owns the string. Copy it before returning; never retain the
    // pointer past this call (plan/14, ownership table).
    std::string owned(utf8_text);
    const auto correlation = mv::abi::current_correlation_id();

    const auto payload = static_cast<std::int64_t>(owned.size());
    const mv::job_id id = session->jobs.submit(
        [text = std::move(owned)](const mv::job_context& ctx) -> status {
          if (ctx.cancelled()) return status::cancelled;
          // Real work goes here in later PRs. In PR 1 the point is the shape:
          // the answer is produced on a worker and delivered as a completion.
          return text.empty() ? status::invalid_arg : status::ok;
        },
        [session, correlation, payload](mv::job_id id, mv::generation gen, status result) {
          mv_completion c{};
          c.kind = MV_COMPLETION_ECHO;
          c.status = static_cast<uint32_t>(result);
          c.job_id = id;
          c.correlation_id = correlation;
          c.generation = gen;
          c.payload = payload;
          session->push_completion(c);
        });

    if (id == mv::invalid_job) return status::internal;
    if (out_job_id) *out_job_id = id;
    return status::ok;
  }));
}

mv_status MV_CALL mv_session_job_stats(mv_session_t session, mv_job_stats* out_stats) {
  return static_cast<mv_status>(guard("mv_session_job_stats", [&]() -> status {
    MV_REQUIRE(valid(session), "session must not be null");
    MV_REQUIRE(out_stats != nullptr, "out_stats must not be null");
    out_stats->submitted = session->jobs.submitted();
    out_stats->completed = session->jobs.completed();
    out_stats->cancelled = session->jobs.cancelled();
    out_stats->queue_depth = session->jobs.queue_depth();
    out_stats->worker_count = session->jobs.worker_count();
    out_stats->generation = session->jobs.current_generation();
    return status::ok;
  }));
}

}  // extern "C"
