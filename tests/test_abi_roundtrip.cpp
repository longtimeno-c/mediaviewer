// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PR 1 ABI deliverable, exercised: "a header, an mv_guard, one call, a
// SafeHandle, and a completion drain — proving the shape end to end before
// anything is built on it" (docs/design/14-abi.md).
//
// The SafeHandle half lives in src.managed/MediaViewer.AbiSmokeTest, because a
// SafeHandle is a C# construct. This file proves the native half and the
// contract the managed half depends on.

#include <windows.h>
#include <d3d11.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "abi/native.h"
#include "abi/test_hooks.h"
#include "corpus.h"
#include "gfx/device.h"
#include "io/paths.h"
#include "mediaviewer/mediaviewer.h"
#include "mediaviewer/mediaviewer_addon.h"
#include "mediaviewer/mediaviewer_clip.h"
#include "mediaviewer/mediaviewer_crash.h"
#include "clip_fixture.h"
#include "fixtures.h"
#include "fixtures_tiff_ico.h"
#include "import_fixture.h"

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
  // docs/design/14: "a folder scan finishing 400 thumbnails is one drain, not 400
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

  // The event means "at least one completion", so it is signalled as soon as
  // the first job finishes. Draining one then only tests a *partial* drain if
  // more than one is queued — otherwise the drain empties the queue, the reset
  // is correct, and the test fails for being early rather than for the
  // behaviour it names. Wait for the work first: the worker counts a job
  // complete just before it pushes the completion, so eight counted means at
  // least seven queued.
  mv_job_stats stats{};
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    REQUIRE(mv_session_job_stats(session.handle, &stats) == MV_OK);
    if (stats.completed == 8) break;
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(stats.completed == 8);

  HANDLE wait_handle = static_cast<HANDLE>(mv_completion_wait_handle(session.handle));
  REQUIRE(::WaitForSingleObject(wait_handle, 5000) == WAIT_OBJECT_0);

  mv_completion one{};
  REQUIRE(mv_completion_drain(session.handle, &one, 1) == 1);
  REQUIRE(::WaitForSingleObject(wait_handle, 0) == WAIT_OBJECT_0);
}

TEST_CASE("the core never retains the caller's string", "[abi][ownership]") {
  // docs/design/14 ownership table: managed to core, strings — "Caller owns; core
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
// no way at all to learn about a transition the core makes on its own. docs/design/14:
// the ABI is designed, not retrofitted — declared surface nothing sends is not
// a design, it is a promise the header is making on the core's behalf.
//
// This test is the promise, made falsifiable: open a clip, drain, and require
// that a VIDEO_STATE actually arrives carrying an mv_play_state.
TEST_CASE("declared video completions are pushed, not just declared",
          "[abi][video][integration]") {
  MV_REQUIRE_CLIP(path, "av_transport.mp4");

  mv::gfx::device device;
  if (!device.create(nullptr)) SKIP("no D3D11 device on this machine (hosted CI has no GPU)");
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

TEST_CASE("mv_folder_forget validates and tolerates an uncached path (ABI 0.7)", "[abi][folder]") {
  REQUIRE(mv_folder_forget(nullptr, "x.jpg") == MV_ERR_INVALID_ARG);
  session_guard session;
  REQUIRE(mv_folder_forget(session.handle, nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_folder_forget(session.handle, "") == MV_ERR_INVALID_ARG);
  REQUIRE(mv_folder_forget(session.handle, "C:/not/cached.jpg") == MV_OK);
}

TEST_CASE("clip index and a keyframe trim through the exports (ABI 0.10)", "[abi][clip]") {
  REQUIRE(mv_clip_submit(nullptr, "x", nullptr, nullptr) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_clip_index_request(nullptr, "x", nullptr) == MV_ERR_INVALID_ARG);
  session_guard s;
  mv::test::scratch_dir dir("abi_clip");
  const std::string src = mv::test::utf8(dir / "clip.mp4");
  REQUIRE(mv::test::clipfx::make(src, {}));

  const auto wait = [&](uint32_t kind, uint64_t id, int64_t payload) {
    const auto until = std::chrono::steady_clock::now() + 20s;
    mv_completion c[16];
    while (std::chrono::steady_clock::now() < until) {
      const uint32_t n = mv_completion_drain(s.handle, c, 16);
      for (uint32_t i = 0; i < n; ++i) {
        if (c[i].kind == kind && c[i].job_id == id && (payload < 0 || c[i].payload == payload)) return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  };

  uint64_t idx = 0;
  REQUIRE(mv_clip_index_request(s.handle, src.c_str(), &idx) == MV_OK);
  REQUIRE(wait(MV_COMPLETION_CLIP_INDEX, idx, 8));
  int64_t kf[8] = {};
  uint32_t count = 0;
  int64_t duration = 0;
  REQUIRE(mv_clip_index_get(s.handle, idx, kf, 8, &count, &duration) == MV_OK);
  REQUIRE(count == 8);

  mv_clip_request r{};
  r.struct_size = sizeof(r);
  r.op = MV_CLIP_TRIM_KEYFRAME;
  r.in_ns = kf[2];
  r.out_ns = kf[4];
  uint64_t job = 0;
  REQUIRE(mv_clip_submit(s.handle, src.c_str(), &r, &job) == MV_OK);
  REQUIRE(wait(MV_COMPLETION_CLIP_JOB, job, MV_CLIP_JOB_DONE));
  mv_clip_progress p{};
  REQUIRE(mv_clip_job_progress(s.handle, job, &p) == MV_OK);
  REQUIRE(p.output_count == 1);
  char out[1024];
  uint32_t bytes = 0;
  REQUIRE(mv_clip_job_output(s.handle, job, 0, out, sizeof(out), &bytes) == MV_OK);
  REQUIRE(std::string(out).find("clip_trimmed.mp4") != std::string::npos);
  REQUIRE(mv_clip_clear_finished(s.handle) == MV_OK);
  REQUIRE(mv_clip_jobs(s.handle, nullptr, 0, &count) == MV_OK);
  REQUIRE(count == 0);
}

// ---------------------------------------------------------------------------
// Issue #151: exports no test called. A result list (how Windows opens AI-search
// results), a page turn, the video transport's argument checks, the clip
// queue's unknown ids, the crash-context table and the volume watch.
// ---------------------------------------------------------------------------

namespace {

// Writes a 2 x 2 BMP: the smallest still every build decodes.
void write_small_bmp(const std::filesystem::path& p) {
  const std::uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
  mv::test::write_bytes(p, fixtures::bmp_rgba(2, 2, rgba));
}

// Drains until a completion of `kind` arrives. Other completions are dropped,
// so callers poll state (counts, info, thumb paths) rather than wait twice.
bool wait_for_kind(mv_session_t s, uint32_t kind, mv_completion* out = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  mv_completion buf[32]{};
  while (std::chrono::steady_clock::now() < deadline) {
    const uint32_t n = mv_completion_drain(s, buf, 32);
    for (uint32_t i = 0; i < n; ++i) {
      if (buf[i].kind == kind) {
        if (out) *out = buf[i];
        return true;
      }
    }
    std::this_thread::sleep_for(2ms);
  }
  return false;
}

// Polls the session's image until `w` x `h` is on screen.
bool wait_for_image(mv_session_t s, uint32_t w, uint32_t h, mv_image_info* out = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  mv_image_info info{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (mv_session_image_info(s, &info) == MV_OK && info.width == w && info.height == h) {
      if (out) *out = info;
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return false;
}

// Every job the session submitted has finished or been cancelled.
bool wait_for_idle(mv_session_t s) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  mv_job_stats stats{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (mv_session_job_stats(s, &stats) != MV_OK) return false;
    if (stats.queue_depth == 0 && stats.completed + stats.cancelled >= stats.submitted) return true;
    std::this_thread::sleep_for(2ms);
  }
  return false;
}

using index_string_fn = mv_status(MV_CALL*)(mv_session_t, uint32_t, char*, uint32_t, uint32_t*);
using session_string_fn = mv_status(MV_CALL*)(mv_session_t, char*, uint32_t, uint32_t*);

std::string folder_string(mv_session_t s, uint32_t index, index_string_fn fn) {
  char buf[1024]{};
  uint32_t bytes = 0;
  REQUIRE(fn(s, index, buf, sizeof(buf), &bytes) == MV_OK);
  return std::string(buf);
}

std::string session_string(mv_session_t s, session_string_fn fn) {
  char buf[1024]{};
  uint32_t bytes = 0;
  REQUIRE(fn(s, buf, sizeof(buf), &bytes) == MV_OK);
  return std::string(buf);
}

// The thumbnail cache goes to the scratch folder, not the user's profile. The
// DLL has its own copy of mv_io, so its override is set through test_hooks.h.
struct thumb_cache_override {
  explicit thumb_cache_override(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    mv::io::set_thumb_cache_dir_override(mv::test::utf8(dir));
    mv::abi::set_user_dirs_for_tests(mv::test::utf8(dir).c_str(), nullptr, nullptr);
  }
  ~thumb_cache_override() {
    mv::io::set_thumb_cache_dir_override({});
    mv::abi::set_user_dirs_for_tests("", nullptr, nullptr);
  }
  thumb_cache_override(const thumb_cache_override&) = delete;
  thumb_cache_override& operator=(const thumb_cache_override&) = delete;
};

}  // namespace

TEST_CASE("mv_folder_open_list keeps order, drops missing paths, reports moments and title",
          "[abi][folder][list]") {
  mv::test::scratch_dir dir("abi_list");
  thumb_cache_override thumbs(dir / "thumbs");
  // Names out of name order: a list keeps the order it was given (best match
  // first), never the folder sort.
  const std::string zebra = mv::test::utf8(dir / "zebra.bmp");
  const std::string gone = mv::test::utf8(dir / "deleted_since_indexed.bmp");
  const std::string apple = mv::test::utf8(dir / "apple.bmp");
  write_small_bmp(dir / "zebra.bmp");
  write_small_bmp(dir / "apple.bmp");

  session_guard s;
  const char* paths[] = {zebra.c_str(), gone.c_str(), apple.c_str()};
  const int64_t moments[] = {-1, 500, 1200};

  CHECK(mv_folder_open_list(nullptr, "t", paths, moments, 3, 0, nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_folder_open_list(s.handle, "t", nullptr, nullptr, 3, 0, nullptr) == MV_ERR_INVALID_ARG);
  const char* with_empty[] = {zebra.c_str(), ""};
  CHECK(mv_folder_open_list(s.handle, "t", with_empty, nullptr, 2, 0, nullptr) ==
        MV_ERR_INVALID_ARG);

  uint64_t job = 0;
  // Select "apple" (index 2 as given); the missing file before it drops out.
  REQUIRE(mv_folder_open_list(s.handle, "guy on a skateboard", paths, moments, 3, 2, &job) == MV_OK);
  REQUIRE(job != 0);
  mv_completion ready{};
  REQUIRE(wait_for_kind(s.handle, MV_COMPLETION_FOLDER_READY, &ready));
  REQUIRE(ready.status == MV_OK);
  REQUIRE(ready.payload == 2);

  uint32_t count = 0;
  REQUIRE(mv_folder_count(s.handle, &count) == MV_OK);
  REQUIRE(count == 2);
  CHECK(folder_string(s.handle, 0, mv_folder_item_name) == "zebra.bmp");
  CHECK(folder_string(s.handle, 1, mv_folder_item_name) == "apple.bmp");

  uint32_t selected = 99;
  REQUIRE(mv_folder_selected(s.handle, &selected) == MV_OK);
  CHECK(selected == 1);

  int64_t moment = 0;
  REQUIRE(mv_folder_item_moment(s.handle, 0, &moment) == MV_OK);
  CHECK(moment == -1);
  REQUIRE(mv_folder_item_moment(s.handle, 1, &moment) == MV_OK);
  CHECK(moment == 1200);
  CHECK(mv_folder_item_moment(s.handle, 2, &moment) == MV_ERR_INVALID_ARG);
  CHECK(mv_folder_item_moment(s.handle, 0, nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_folder_item_moment(nullptr, 0, &moment) == MV_ERR_INVALID_ARG);

  CHECK(session_string(s.handle, mv_folder_list_title) == "guy on a skateboard");
  // A list is not a directory and has no folder tiles.
  CHECK(session_string(s.handle, mv_folder_directory).empty());
  uint32_t subfolders = 99;
  REQUIRE(mv_folder_subfolder_count(s.handle, &subfolders) == MV_OK);
  CHECK(subfolders == 0);
  char buf[64]{};
  uint32_t bytes = 0;
  CHECK(mv_folder_subfolder_path(s.handle, 0, buf, sizeof(buf), &bytes) == MV_ERR_INVALID_ARG);
  CHECK(mv_folder_summary_cover_thumb_path(s.handle, 0, buf, sizeof(buf), &bytes) ==
        MV_ERR_INVALID_ARG);

  // Thumbs land for both items through the same cache as a folder.
  CHECK(mv_folder_thumbs_visible(nullptr, 0, 2) == MV_ERR_INVALID_ARG);
  REQUIRE(mv_folder_thumbs_visible(s.handle, 0, 2) == MV_OK);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  std::string thumb0;
  std::string thumb1;
  while (std::chrono::steady_clock::now() < deadline && (thumb0.empty() || thumb1.empty())) {
    thumb0 = folder_string(s.handle, 0, mv_folder_item_thumb_path);
    thumb1 = folder_string(s.handle, 1, mv_folder_item_thumb_path);
    std::this_thread::sleep_for(5ms);
  }
  CHECK_FALSE(thumb0.empty());
  CHECK_FALSE(thumb1.empty());
  CHECK(thumb0 != thumb1);
  CHECK(mv_folder_item_thumb_path(s.handle, 2, buf, sizeof(buf), &bytes) == MV_ERR_INVALID_ARG);

  // Opening a directory ends the list: no title, the directory reported, no moments.
  REQUIRE(mv_folder_open(s.handle, mv::test::utf8(dir.root()).c_str(), nullptr, &job) == MV_OK);
  REQUIRE(wait_for_kind(s.handle, MV_COMPLETION_FOLDER_READY));
  CHECK(session_string(s.handle, mv_folder_list_title).empty());
  CHECK_FALSE(session_string(s.handle, mv_folder_directory).empty());
  REQUIRE(mv_folder_count(s.handle, &count) == MV_OK);
  REQUIRE(count == 2);
  for (uint32_t i = 0; i < count; ++i) {
    REQUIRE(mv_folder_item_moment(s.handle, i, &moment) == MV_OK);
    CHECK(moment == -1);
  }

  REQUIRE(mv_folder_close(s.handle) == MV_OK);
  CHECK(wait_for_idle(s.handle));
}

TEST_CASE("mv_folder_select_page shows page 2 of a 3-page TIFF and ignores page 5",
          "[abi][folder][pages]") {
  mv::test::scratch_dir dir("abi_pages");
  thumb_cache_override thumbs(dir / "thumbs");
  // Each page a different size, so the image info says which page is shown.
  const auto tiff = fixtures::tiff_pages({
      {.grey = 40, .reduced = false, .width = 8, .height = 8},
      {.grey = 80, .reduced = false, .width = 16, .height = 4},
      {.grey = 120, .reduced = false, .width = 4, .height = 12},
  });
  REQUIRE_FALSE(tiff.empty());
  const std::string path = mv::test::utf8(dir / "scan.tif");
  mv::test::write_bytes(dir / "scan.tif", tiff);

  session_guard s;
  CHECK(mv_folder_select_page(nullptr, 1, nullptr) == MV_ERR_INVALID_ARG);
  // Nothing listed, so nothing is selected.
  CHECK(mv_folder_select_page(s.handle, 1, nullptr) == MV_ERR_INVALID_ARG);

  const char* paths[] = {path.c_str()};
  uint64_t job = 0;
  REQUIRE(mv_folder_open_list(s.handle, "scan", paths, nullptr, 1, 0, &job) == MV_OK);
  REQUIRE(wait_for_kind(s.handle, MV_COMPLETION_FOLDER_READY));
  mv_image_info info{};
  REQUIRE(wait_for_image(s.handle, 8, 8, &info));
  CHECK(info.page_count == 3);

  REQUIRE(mv_folder_select_page(s.handle, 1, &job) == MV_OK);
  REQUIRE(wait_for_image(s.handle, 16, 4, &info));
  CHECK(info.page_count == 3);

  // A page past the end leaves the page on screen as it is.
  REQUIRE(mv_folder_select_page(s.handle, 5, &job) == MV_OK);
  REQUIRE(wait_for_idle(s.handle));
  REQUIRE(mv_session_image_info(s.handle, &info) == MV_OK);
  CHECK(info.width == 16);
  CHECK(info.height == 4);

  // Page 0 is the stop itself, as mv_folder_select.
  REQUIRE(mv_folder_select_page(s.handle, 0, &job) == MV_OK);
  REQUIRE(wait_for_image(s.handle, 8, 8));

  REQUIRE(mv_folder_close(s.handle) == MV_OK);
  CHECK(wait_for_idle(s.handle));
}

TEST_CASE("the video transport rejects a null session and is harmless with no clip",
          "[abi][video]") {
  CHECK(mv_video_pause(nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_seek(nullptr, 0, 1) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_step(nullptr, 1) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_set_rate(nullptr, 1.0) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_set_volume(nullptr, 0.5f) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_select_audio_track(nullptr, 0) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_set_loop(nullptr, 0, -1) == MV_ERR_INVALID_ARG);
  int64_t position = -1;
  CHECK(mv_video_position(nullptr, &position) == MV_ERR_INVALID_ARG);
  mv_video_stats stats{};
  CHECK(mv_video_get_stats(nullptr, &stats) == MV_ERR_INVALID_ARG);

  session_guard s;
  CHECK(mv_video_position(s.handle, nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_video_get_stats(s.handle, nullptr) == MV_ERR_INVALID_ARG);

  // No clip open: the commands queue for nothing and succeed. An out-of-range
  // rate is clamped to 0.25 .. 4.0 by the player (mediaviewer.h), not rejected.
  CHECK(mv_video_pause(s.handle) == MV_OK);
  CHECK(mv_video_seek(s.handle, 1'000'000'000, 0) == MV_OK);
  CHECK(mv_video_seek(s.handle, 1'000'000'000, 1) == MV_OK);
  CHECK(mv_video_step(s.handle, -1) == MV_OK);
  CHECK(mv_video_set_rate(s.handle, 64.0) == MV_OK);
  CHECK(mv_video_set_volume(s.handle, 0.25f) == MV_OK);
  CHECK(mv_video_select_audio_track(s.handle, 3) == MV_OK);
  CHECK(mv_video_set_loop(s.handle, 0, 1'000'000'000) == MV_OK);
  CHECK(mv_video_set_loop(s.handle, 0, -1) == MV_OK);

  REQUIRE(mv_video_position(s.handle, &position) == MV_OK);
  CHECK(position == 0);
  REQUIRE(mv_video_get_stats(s.handle, &stats) == MV_OK);
  CHECK(stats.position_ns == 0);
  CHECK(stats.frames_presented == 0);
}

namespace {

// A device with video support, as a hosted runner has one: with no GPU the
// hardware driver type is the Basic Render Driver, which accepts
// VIDEO_SUPPORT and has no video decoder, so the clip decodes in software
// (test_video_ring.cpp). That is enough to drive the transport.
struct video_device {
  ID3D11Device* device = nullptr;
  video_device() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr,
                                   nullptr))) {
      device = nullptr;
      return;
    }
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
      mt->SetMultithreadProtected(TRUE);
      mt->Release();
    }
  }
  ~video_device() {
    if (device) device->Release();
  }
  video_device(const video_device&) = delete;
  video_device& operator=(const video_device&) = delete;
};

// Stands in for the present loop: poll_video is what applies the transport
// commands and takes frames, and only the render thread calls it.
struct video_pump {
  explicit video_pump(mv_session_t s) : session(s) {}
  mv_session_t session;
  mv::player::video_frame frame;
  bool active = false;
  int shown = 0;
  bool ended = false;
  void poll() {
    if (mv::abi::poll_video(session, 16'666'667, frame, active)) ++shown;
    mv_completion drained[32]{};
    const uint32_t n = mv_completion_drain(session, drained, 32);
    for (uint32_t i = 0; i < n; ++i) ended = ended || drained[i].kind == MV_COMPLETION_VIDEO_ENDED;
  }
  template <class Pred>
  bool until(Pred pred, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      poll();
      if (pred()) return true;
      std::this_thread::sleep_for(4ms);
    }
    return false;
  }
  void run_for(std::chrono::milliseconds span) {
    (void)until([] { return false; }, span);
  }
  int64_t position() {
    int64_t p = -1;
    REQUIRE(mv_video_position(session, &p) == MV_OK);
    return p;
  }
  uint32_t state() {
    uint32_t s = 99;
    REQUIRE(mv_video_state(session, &s) == MV_OK);
    return s;
  }
  mv_video_stats stats() {
    mv_video_stats s{};
    REQUIRE(mv_video_get_stats(session, &s) == MV_OK);
    return s;
  }
};

constexpr int64_t kFrameNs = 33'333'333;  // the fixture's 30 fps
constexpr int64_t kSecondNs = 1'000'000'000;

}  // namespace

TEST_CASE("the video transport drives a playing synthetic clip", "[abi][video]") {
  // Issue #151: every transport call against a clip that is really playing,
  // with what each one is for observed through the ABI. A synthetic clip
  // (clip_fixture.h: 4 s, 30 fps, MPEG-4 + MP2), so no corpus.
  mv::test::scratch_dir dir("abi_transport");
  thumb_cache_override thumbs(dir / "thumbs");  // the player's resume note goes beside it
  const std::string path = mv::test::utf8(dir / "clip.mp4");
  REQUIRE(mv::test::clipfx::make(path, {}));

  video_device device;
  if (!device.device) SKIP("no D3D11 device with video support on this machine");
  session_guard s(2);
  REQUIRE(mv::abi::attach_device(s.handle, device.device) == mv::status::ok);
  REQUIRE(mv_video_set_muted(s.handle, 1) == MV_OK);  // the audio clock still runs at gain 0
  uint64_t job = 0;
  REQUIRE(mv_video_open(s.handle, path.c_str(), &job) == MV_OK);
  video_pump pump(s.handle);
  REQUIRE(pump.until([&] { return pump.shown > 0; }));
  mv_video_info info{};
  REQUIRE(mv_video_get_info(s.handle, &info) == MV_OK);
  CHECK(info.duration_ns == 4 * kSecondNs);
  CHECK(info.audio_tracks == 1);

  // Play: frames are presented and the position moves.
  REQUIRE(mv_video_play(s.handle) == MV_OK);
  REQUIRE(pump.until([&] {
    return pump.state() == MV_PLAY_PLAYING && pump.stats().frames_presented >= 10;
  }));
  CHECK(pump.position() > 0);

  // Pause: the position holds.
  REQUIRE(mv_video_pause(s.handle) == MV_OK);
  REQUIRE(pump.until([&] { return pump.state() == MV_PLAY_PAUSED; }));
  const int64_t paused_at = pump.position();
  pump.run_for(300ms);
  CHECK(pump.position() == paused_at);

  // Exact seek while paused shows the frame at the target.
  int before = pump.shown;
  REQUIRE(mv_video_seek(s.handle, 2 * kSecondNs, 1) == MV_OK);
  REQUIRE(pump.until([&] { return pump.shown > before; }));
  const int64_t at_seek = pump.position();
  CHECK(std::llabs(at_seek - 2 * kSecondNs) <= kFrameNs / 2);

  // A step forward is the next frame; a step back returns to this one.
  before = pump.shown;
  REQUIRE(mv_video_step(s.handle, 1) == MV_OK);
  REQUIRE(pump.until([&] { return pump.shown > before; }));
  const int64_t stepped = pump.position();
  CHECK(std::llabs(stepped - at_seek - kFrameNs) <= kFrameNs / 2);
  before = pump.shown;
  REQUIRE(mv_video_step(s.handle, -1) == MV_OK);
  REQUIRE(pump.until([&] { return pump.shown > before && pump.position() < stepped; }));
  CHECK(std::llabs(pump.position() - at_seek) <= kFrameNs / 2);
  CHECK(pump.state() == MV_PLAY_PAUSED);

  // Rate: taken, and clamped to 0.25 .. 4.0 rather than refused (mediaviewer.h).
  for (const double rate : {2.0, 64.0, 0.1, 1.0}) {
    const double want = rate > 4.0 ? 4.0 : (rate < 0.25 ? 0.25 : rate);
    REQUIRE(mv_video_set_rate(s.handle, rate) == MV_OK);
    CHECK(pump.until([&] { return pump.stats().playback_rate == want; }));
  }
  CHECK(std::llabs(pump.position() - at_seek) <= kFrameNs / 2);

  // Volume moves nothing but the gain.
  pump.run_for(100ms);
  const int64_t before_volume = pump.position();
  REQUIRE(mv_video_set_volume(s.handle, 0.5f) == MV_OK);
  pump.run_for(200ms);
  CHECK(pump.position() == before_volume);
  CHECK(pump.state() == MV_PLAY_PAUSED);

  // Choosing the (only) audio track re-reads from where it is; a track that
  // is not there is ignored.
  before = pump.shown;
  REQUIRE(mv_video_select_audio_track(s.handle, 0) == MV_OK);
  CHECK(pump.until([&] { return pump.shown > before; }));
  CHECK(std::llabs(pump.position() - before_volume) <= kFrameNs / 2);
  REQUIRE(mv_video_select_audio_track(s.handle, 5) == MV_OK);
  pump.run_for(200ms);
  CHECK(std::llabs(pump.position() - before_volume) <= kFrameNs / 2);
  CHECK(pump.state() == MV_PLAY_PAUSED);

  // An A-B loop over 1.0 .. 1.5 s keeps playback inside it and wraps.
  REQUIRE(mv_video_set_loop(s.handle, kSecondNs, 3 * kSecondNs / 2) == MV_OK);
  REQUIRE(mv_video_seek(s.handle, kSecondNs, 1) == MV_OK);
  REQUIRE(mv_video_play(s.handle) == MV_OK);
  int64_t highest = 0;
  int64_t last = 0;
  int wraps = 0;
  (void)pump.until(
      [&] {
        const int64_t p = pump.position();
        highest = std::max(highest, p);
        if (p + 200'000'000 < last) ++wraps;
        last = p;
        return false;
      },
      1500ms);
  CHECK(wraps >= 1);
  CHECK(highest <= 3 * kSecondNs / 2 + 250'000'000);

  // Loop off: playback runs past B to the end.
  REQUIRE(mv_video_set_loop(s.handle, 0, -1) == MV_OK);
  REQUIRE(mv_video_seek(s.handle, 3 * kSecondNs + kSecondNs / 2, 1) == MV_OK);
  REQUIRE(mv_video_play(s.handle) == MV_OK);
  CHECK(pump.until([&] { return pump.state() == MV_PLAY_ENDED; }));
  CHECK(pump.ended);

  // The stats and the position agree, and say what happened.
  const mv_video_stats stats = pump.stats();
  CHECK(stats.position_ns == pump.position());
  CHECK(stats.position_ns > 3 * kSecondNs + kSecondNs / 2);
  CHECK(stats.frames_presented >= 10);
  CHECK(stats.playback_rate == 1.0);

  REQUIRE(mv_video_close(s.handle) == MV_OK);
  pump.poll();
  CHECK(pump.state() == MV_PLAY_STOPPED);
  mv::abi::detach_device(s.handle);
}

TEST_CASE("mv_probe_is_video answers by magic bytes", "[abi][video][probe]") {
  mv::test::scratch_dir dir("abi_probe");
  const std::string clip = mv::test::utf8(dir / "clip.mp4");
  REQUIRE(mv::test::clipfx::make(clip, {}));
  // A still named like a clip is still a still.
  const std::string fake = mv::test::utf8(dir / "still.mp4");
  write_small_bmp(dir / "still.mp4");

  session_guard s;
  int32_t is_video = -1;
  CHECK(mv_probe_is_video(nullptr, clip.c_str(), &is_video) == MV_ERR_INVALID_ARG);
  CHECK(mv_probe_is_video(s.handle, nullptr, &is_video) == MV_ERR_INVALID_ARG);
  CHECK(mv_probe_is_video(s.handle, clip.c_str(), nullptr) == MV_ERR_INVALID_ARG);

  REQUIRE(mv_probe_is_video(s.handle, clip.c_str(), &is_video) == MV_OK);
  CHECK(is_video == 1);
  REQUIRE(mv_probe_is_video(s.handle, fake.c_str(), &is_video) == MV_OK);
  CHECK(is_video == 0);
  // A file that is not there is an error, not "not a video".
  const std::string missing = mv::test::utf8(dir / "missing.mp4");
  CHECK(mv_probe_is_video(s.handle, missing.c_str(), &is_video) != MV_OK);
}

TEST_CASE("the clip queue rejects null sessions and ids it never issued", "[abi][clip]") {
  uint64_t retried = 0;
  CHECK(mv_clip_set_helper(nullptr, "helper.exe") == MV_ERR_INVALID_ARG);
  CHECK(mv_clip_cancel(nullptr, 1) == MV_ERR_INVALID_ARG);
  CHECK(mv_clip_retry(nullptr, 1, &retried) == MV_ERR_INVALID_ARG);

  session_guard s;
  CHECK(mv_clip_set_helper(s.handle, nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_clip_set_helper(s.handle, "") == MV_ERR_INVALID_ARG);
  CHECK(mv_clip_set_helper(s.handle, MV_CLIPJOB_PATH) == MV_OK);
  CHECK(mv_clip_cancel(s.handle, 424242) == MV_ERR_INVALID_ARG);
  CHECK(mv_clip_retry(s.handle, 424242, nullptr) == MV_ERR_INVALID_ARG);
  CHECK(mv_clip_retry(s.handle, 424242, &retried) == MV_ERR_INVALID_ARG);
  CHECK(retried == 0);
}

TEST_CASE("mv_crash_context hands out a stable table", "[abi][crash]") {
  CHECK(mv_crash_context(nullptr) == MV_ERR_INVALID_ARG);
  mv_crash_context_info first{};
  mv_crash_context_info second{};
  REQUIRE(mv_crash_context(&first) == MV_OK);
  REQUIRE(mv_crash_context(&second) == MV_OK);
  CHECK(first.slots != nullptr);
  CHECK(first.slot_count > 0);
  CHECK(first.slot_bytes > 0);
  CHECK(first.last_call_correlation_id != nullptr);
  // The crash handler reads these addresses after the fact: they never move.
  CHECK(first.slots == second.slots);
  CHECK(first.last_call_correlation_id == second.last_call_correlation_id);
}
