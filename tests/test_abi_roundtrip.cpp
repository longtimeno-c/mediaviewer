// SPDX-License-Identifier: GPL-2.0-or-later
//
// The PR 1 ABI deliverable, exercised: "a header, an mv_guard, one call, a
// SafeHandle, and a completion drain — proving the shape end to end before
// anything is built on it" (plan/14-abi.md).
//
// The SafeHandle half lives in src.managed/MediaViewer.AbiSmokeTest, because a
// SafeHandle is a C# construct. This file proves the native half and the
// contract the managed half depends on.

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "abi/native.h"
#include "corpus.h"
#include "gfx/device.h"
#include "mediaviewer/mediaviewer.h"

using namespace std::chrono_literals;

namespace {

struct session_guard {
  mv_session_t handle = nullptr;
  explicit session_guard(uint32_t workers = 2) {
    mv_session_config config{};
    config.worker_count = workers;
    config.enable_etw = 0;
    REQUIRE(mv_session_create(&config, &handle) == MV_OK);
    REQUIRE(handle != nullptr);
  }
  ~session_guard() {
    if (handle) mv_session_release(handle);
  }
  session_guard(const session_guard&) = delete;
  session_guard& operator=(const session_guard&) = delete;
};

}  // namespace

TEST_CASE("the ABI version is what the header promises", "[abi]") {
  // The managed side asserts on this at startup so a mismatched core DLL fails
  // loudly rather than reading garbage out of a changed struct layout.
  const uint32_t version = mv_abi_version();
  REQUIRE((version >> 16) == MV_ABI_VERSION_MAJOR);
  REQUIRE((version & 0xffffu) == MV_ABI_VERSION_MINOR);
}

TEST_CASE("mv_completion layout is fixed", "[abi]") {
  // C# declares this struct field for field. A silent layout change here reads
  // as corrupted job ids on the managed side, which is exactly the class of bug
  // the ABI document exists to prevent.
  REQUIRE(sizeof(mv_completion) == 40);
  REQUIRE(offsetof(mv_completion, kind) == 0);
  REQUIRE(offsetof(mv_completion, status) == 4);
  REQUIRE(offsetof(mv_completion, job_id) == 8);
  REQUIRE(offsetof(mv_completion, correlation_id) == 16);
  REQUIRE(offsetof(mv_completion, generation) == 24);
  REQUIRE(offsetof(mv_completion, payload) == 32);
}

TEST_CASE("every fallible call returns a status, never a bool", "[abi]") {
  REQUIRE(mv_session_create(nullptr, nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_session_retain(nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_session_release(nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_session_current_generation(nullptr, nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_session_echo(nullptr, "x", nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_session_job_stats(nullptr, nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_image_open(nullptr, "x", nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_session_image_info(nullptr, nullptr) == MV_ERR_INVALID_ARG);
}

TEST_CASE("a failure leaves a message and a correlation id", "[abi]") {
  REQUIRE(mv_session_retain(nullptr) == MV_ERR_INVALID_ARG);

  const char* message = mv_last_error_message();
  REQUIRE(message != nullptr);
  REQUIRE(std::strlen(message) > 0);
  // The correlation id is what ties a managed exception to the native minidump
  // that produced it.
  REQUIRE(mv_last_error_correlation_id() != 0);
}

TEST_CASE("last-error detail is per-thread", "[abi]") {
  // The header promises "the calling thread"; if this were process-global, two
  // concurrent decodes would overwrite each other's diagnosis.
  session_guard session;
  REQUIRE(mv_session_retain(nullptr) == MV_ERR_INVALID_ARG);
  const uint64_t main_correlation = mv_last_error_correlation_id();
  REQUIRE(main_correlation != 0);

  uint64_t other_correlation = 0;
  std::thread worker([&] {
    REQUIRE(mv_session_current_generation(nullptr, nullptr) == MV_ERR_INVALID_ARG);
    other_correlation = mv_last_error_correlation_id();
  });
  worker.join();

  REQUIRE(other_correlation != 0);
  REQUIRE(other_correlation != main_correlation);
  // The calling thread's own value survived the other thread's failure.
  REQUIRE(mv_last_error_correlation_id() == main_correlation);
}

TEST_CASE("a successful call clears the previous error message", "[abi]") {
  session_guard session;
  REQUIRE(mv_session_retain(nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(std::strlen(mv_last_error_message()) > 0);

  uint32_t generation = 0;
  REQUIRE(mv_session_current_generation(session.handle, &generation) == MV_OK);
  REQUIRE(std::strlen(mv_last_error_message()) == 0);
}

TEST_CASE("echo round-trips through a worker and a completion drain", "[abi][completion]") {
  session_guard session;

  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  REQUIRE(wait_handle != nullptr);

  // Nothing pending yet.
  REQUIRE(::WaitForSingleObject(wait_handle, 0) == WAIT_TIMEOUT);

  const std::string text = "camera dump";
  uint64_t job_id = 0;
  REQUIRE(mv_session_echo(session.handle, text.c_str(), &job_id) == MV_OK);
  REQUIRE(job_id != 0);

  // The call returned a job id immediately; the answer arrives out of band.
  // This is the shape mv_image_open takes in PR 2, and the reason echo is not
  // written as a synchronous function.
  REQUIRE(::WaitForSingleObject(wait_handle, 5000) == WAIT_OBJECT_0);

  mv_completion completions[8]{};
  const uint32_t count = mv_completion_drain(session.handle, completions, 8);
  REQUIRE(count == 1);
  REQUIRE(completions[0].kind == MV_COMPLETION_ECHO);
  REQUIRE(completions[0].status == MV_OK);
  REQUIRE(completions[0].job_id == job_id);
  REQUIRE(completions[0].correlation_id != 0);
  REQUIRE(completions[0].payload == static_cast<int64_t>(text.size()));

  // Drained empty, so the event is reset and the next drain reports nothing.
  REQUIRE(mv_completion_drain(session.handle, completions, 8) == 0);
  REQUIRE(::WaitForSingleObject(wait_handle, 0) == WAIT_TIMEOUT);
}

TEST_CASE("completions batch rather than arriving one marshalling hop at a time",
          "[abi][completion]") {
  // plan/14: "a folder scan finishing 400 thumbnails is one drain, not 400
  // marshalling hops."
  session_guard session(4);
  constexpr int count = 400;

  for (int i = 0; i < count; ++i) {
    uint64_t job_id = 0;
    REQUIRE(mv_session_echo(session.handle, "thumb", &job_id) == MV_OK);
  }

  std::vector<mv_completion> drained;
  drained.reserve(count);

  const auto deadline = std::chrono::steady_clock::now() + 15s;
  int drain_calls = 0;
  while (static_cast<int>(drained.size()) < count &&
         std::chrono::steady_clock::now() < deadline) {
    mv_completion buffer[512]{};
    const uint32_t n = mv_completion_drain(session.handle, buffer, 512);
    if (n == 0) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    ++drain_calls;
    drained.insert(drained.end(), buffer, buffer + n);
  }

  REQUIRE(static_cast<int>(drained.size()) == count);
  // Far fewer drains than completions. Not one — the workers are genuinely
  // concurrent — but nothing like 400.
  REQUIRE(drain_calls < count / 4);
}

TEST_CASE("a partial drain leaves the event signalled", "[abi][completion]") {
  // Otherwise a caller that drains with a small buffer goes back to sleep with
  // work still pending, and the UI stalls until the next unrelated event.
  session_guard session(1);
  for (int i = 0; i < 8; ++i) {
    uint64_t job_id = 0;
    REQUIRE(mv_session_echo(session.handle, "x", &job_id) == MV_OK);
  }

  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  REQUIRE(::WaitForSingleObject(wait_handle, 5000) == WAIT_OBJECT_0);

  mv_completion one{};
  REQUIRE(mv_completion_drain(session.handle, &one, 1) == 1);
  REQUIRE(::WaitForSingleObject(wait_handle, 0) == WAIT_OBJECT_0);
}

TEST_CASE("the core never retains the caller's string", "[abi][ownership]") {
  // plan/14 ownership table: managed to core, strings — "Caller owns; core
  // copies before returning."
  session_guard session;

  uint64_t job_id = 0;
  {
    std::string scratch = "a string that is about to die";
    REQUIRE(mv_session_echo(session.handle, scratch.c_str(), &job_id) == MV_OK);
    std::memset(scratch.data(), 0, scratch.size());
  }

  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  REQUIRE(::WaitForSingleObject(wait_handle, 5000) == WAIT_OBJECT_0);

  mv_completion completion{};
  REQUIRE(mv_completion_drain(session.handle, &completion, 1) == 1);
  REQUIRE(completion.status == MV_OK);
  REQUIRE(completion.payload == 29);
}

TEST_CASE("generations are visible across the ABI", "[abi][cancellation]") {
  session_guard session;

  uint32_t before = 0;
  REQUIRE(mv_session_current_generation(session.handle, &before) == MV_OK);

  uint32_t bumped = 0;
  REQUIRE(mv_session_bump_generation(session.handle, &bumped) == MV_OK);
  REQUIRE(bumped == before + 1);

  uint32_t after = 0;
  REQUIRE(mv_session_current_generation(session.handle, &after) == MV_OK);
  REQUIRE(after == bumped);
}

TEST_CASE("retain and release balance", "[abi][lifetime]") {
  mv_session_config config{};
  config.worker_count = 1;
  mv_session_t session = nullptr;
  REQUIRE(mv_session_create(&config, &session) == MV_OK);

  REQUIRE(mv_session_retain(session) == MV_OK);
  REQUIRE(mv_session_release(session) == MV_OK);   // back to one reference

  uint32_t generation = 0;
  REQUIRE(mv_session_current_generation(session, &generation) == MV_OK);

  REQUIRE(mv_session_release(session) == MV_OK);   // destroyed here
}

TEST_CASE("job stats report through the ABI", "[abi]") {
  session_guard session(2);

  mv_job_stats stats{};
  REQUIRE(mv_session_job_stats(session.handle, &stats) == MV_OK);
  REQUIRE(stats.worker_count == 2);
  REQUIRE(stats.submitted == 0);

  uint64_t job_id = 0;
  REQUIRE(mv_session_echo(session.handle, "x", &job_id) == MV_OK);

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    REQUIRE(mv_session_job_stats(session.handle, &stats) == MV_OK);
    if (stats.completed == 1) break;
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(stats.submitted == 1);
  REQUIRE(stats.completed == 1);
}

TEST_CASE("status names are stable and never null", "[abi]") {
  REQUIRE(std::string(mv_status_name(MV_OK)) == "OK");
  REQUIRE(std::string(mv_status_name(MV_ERR_CANCELLED)) == "CANCELLED");
  REQUIRE(std::string(mv_status_name(static_cast<mv_status>(9999))) == "UNKNOWN");
}

// MV_COMPLETION_VIDEO_STATE and _VIDEO_ENDED were declared at ABI 0.4 and never
// pushed by anything, so the chrome polled play state on a 150 ms timer and had
// no way at all to learn about a transition the core makes on its own. plan/14:
// the ABI is designed, not retrofitted — declared surface nothing sends is not
// a design, it is a promise the header is making on the core's behalf.
//
// This test is the promise, made falsifiable: open a clip, drain, and require
// that a VIDEO_STATE actually arrives carrying an mv_play_state.
TEST_CASE("declared video completions are pushed, not just declared",
          "[abi][video][integration]") {
  MV_REQUIRE_CLIP(path, "av_transport.mp4");

  mv::gfx::device device;
  REQUIRE(device.create(nullptr));
  session_guard session(2);
  REQUIRE(mv::abi::attach_device(session.handle, device.d3d()) == mv::status::ok);

  uint64_t job = 0;
  REQUIRE(mv_video_open(session.handle, path.c_str(), &job) == MV_OK);

  // poll_video is what pushes the completion, and only the render thread calls
  // it — so this loop is standing in for the present loop.
  mv::player::video_frame frame;
  bool active = false;
  bool saw_state = false;
  int64_t reported = -1;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline && !saw_state) {
    (void)mv::abi::poll_video(session.handle, 16'666'667, frame, active);
    mv_completion drained[32]{};
    const uint32_t n = mv_completion_drain(session.handle, drained, 32);
    for (uint32_t i = 0; i < n; ++i) {
      if (drained[i].kind == MV_COMPLETION_VIDEO_STATE) {
        saw_state = true;
        reported = drained[i].payload;
      }
    }
    std::this_thread::sleep_for(2ms);
  }

  REQUIRE(saw_state);
  // Payload is documented as an mv_play_state. Opening plays, so anything but
  // stopped means the cast survived the trip.
  REQUIRE(reported != MV_PLAY_STOPPED);
  REQUIRE(reported <= MV_PLAY_ENDED);

  // Closing retires the clip, which is a transition the host did not perform on
  // the media_source itself — exactly the case the header says these exist for.
  REQUIRE(mv_video_close(session.handle) == MV_OK);
  bool saw_stopped = false;
  const auto close_deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < close_deadline && !saw_stopped) {
    (void)mv::abi::poll_video(session.handle, 16'666'667, frame, active);
    mv_completion drained[32]{};
    const uint32_t n = mv_completion_drain(session.handle, drained, 32);
    for (uint32_t i = 0; i < n; ++i) {
      if (drained[i].kind == MV_COMPLETION_VIDEO_STATE &&
          drained[i].payload == MV_PLAY_STOPPED) {
        saw_stopped = true;
      }
    }
    std::this_thread::sleep_for(2ms);
  }
  REQUIRE(saw_stopped);

  mv::abi::detach_device(session.handle);
}
