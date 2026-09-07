// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/job_system.h"

using namespace std::chrono_literals;

namespace {

// Waits for a predicate rather than sleeping a fixed amount: a timing-sensitive
// test that sleeps is a test that goes flaky on a busy CI box.
template <typename Predicate>
bool wait_for(Predicate p, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (p()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return p();
}

}  // namespace

TEST_CASE("job_system runs work and reports completion", "[core][jobs]") {
  mv::job_system jobs;
  REQUIRE(jobs.start(2) == mv::status::ok);
  REQUIRE(jobs.worker_count() == 2);

  std::atomic<int> ran{0};
  std::atomic<int> done{0};
  std::atomic<mv::status> reported{mv::status::internal};

  const mv::job_id id = jobs.submit(
      [&](const mv::job_context& ctx) {
        REQUIRE(ctx.id() != mv::invalid_job);
        ran.fetch_add(1);
        return mv::status::ok;
      },
      [&](mv::job_id, mv::generation, mv::status s) {
        reported.store(s);
        done.fetch_add(1);
      });

  REQUIRE(id != mv::invalid_job);
  REQUIRE(wait_for([&] { return done.load() == 1; }));
  REQUIRE(ran.load() == 1);
  REQUIRE(reported.load() == mv::status::ok);
  REQUIRE(jobs.completed() == 1);
}

TEST_CASE("bumping the generation abandons queued work", "[core][jobs][cancellation]") {
  // The scenario this exists for: arrow-keying through a folder queues a decode
  // per file. Without generations, all two hundred run.
  mv::job_system jobs;
  REQUIRE(jobs.start(1) == mv::status::ok);

  std::atomic<bool> release{false};
  std::atomic<int> bodies_run{0};
  std::atomic<int> completions{0};
  std::atomic<int> cancelled_completions{0};

  // Occupy the single worker so the rest of the batch is still queued when the
  // generation moves.
  jobs.submit([&](const mv::job_context&) {
    while (!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    return mv::status::ok;
  });

  constexpr int queued = 50;
  for (int i = 0; i < queued; ++i) {
    jobs.submit(
        [&](const mv::job_context&) {
          bodies_run.fetch_add(1);
          return mv::status::ok;
        },
        [&](mv::job_id, mv::generation, mv::status s) {
          if (s == mv::status::cancelled) cancelled_completions.fetch_add(1);
          completions.fetch_add(1);
        });
  }

  const mv::generation before = jobs.current_generation();
  const mv::generation after = jobs.bump_generation();
  REQUIRE(after == before + 1);

  release.store(true, std::memory_order_release);
  REQUIRE(wait_for([&] { return completions.load() == queued; }));

  // Every one of them was abandoned before its body ran, and every one still
  // reported a completion — nothing is left waiting on an answer that never
  // comes.
  REQUIRE(bodies_run.load() == 0);
  REQUIRE(cancelled_completions.load() == queued);
  REQUIRE(jobs.cancelled() >= static_cast<std::uint64_t>(queued));
}

TEST_CASE("a running job sees cancellation through its context", "[core][jobs][cancellation]") {
  mv::job_system jobs;
  REQUIRE(jobs.start(1) == mv::status::ok);

  std::atomic<bool> started{false};
  std::atomic<bool> observed_cancel{false};
  std::atomic<bool> finished{false};

  jobs.submit(
      [&](const mv::job_context& ctx) {
        started.store(true);
        // Stands in for a tile boundary in a decode loop.
        for (int i = 0; i < 100000; ++i) {
          if (ctx.cancelled()) {
            observed_cancel.store(true);
            return mv::status::cancelled;
          }
          std::this_thread::sleep_for(100us);
        }
        return mv::status::ok;
      },
      [&](mv::job_id, mv::generation, mv::status) { finished.store(true); });

  REQUIRE(wait_for([&] { return started.load(); }));
  jobs.bump_generation();
  REQUIRE(wait_for([&] { return finished.load(); }));
  REQUIRE(observed_cancel.load());
}

TEST_CASE("shutdown reports never-started jobs rather than dropping them",
          "[core][jobs][shutdown]") {
  // A completion that never arrives is a UI element stuck on a spinner
  // forever. Shutdown must answer every submission.
  auto jobs = std::make_unique<mv::job_system>();
  REQUIRE(jobs->start(1) == mv::status::ok);

  std::atomic<bool> release{false};
  std::atomic<int> completions{0};

  jobs->submit([&](const mv::job_context&) {
    while (!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    return mv::status::ok;
  });

  constexpr int queued = 20;
  for (int i = 0; i < queued; ++i) {
    jobs->submit([](const mv::job_context&) { return mv::status::ok; },
                 [&](mv::job_id, mv::generation, mv::status) { completions.fetch_add(1); });
  }

  release.store(true, std::memory_order_release);
  jobs->shutdown();

  REQUIRE(completions.load() == queued);
}

TEST_CASE("submitting after shutdown fails instead of silently vanishing", "[core][jobs]") {
  mv::job_system jobs;
  REQUIRE(jobs.start(1) == mv::status::ok);
  jobs.shutdown();
  REQUIRE(jobs.submit([](const mv::job_context&) { return mv::status::ok; }) == mv::invalid_job);
}

TEST_CASE("many producers submit concurrently without loss", "[core][jobs]") {
  mv::job_system jobs;
  REQUIRE(jobs.start(4) == mv::status::ok);

  constexpr int producers = 8;
  constexpr int per_producer = 500;
  std::atomic<int> completions{0};

  std::vector<std::thread> threads;
  threads.reserve(producers);
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&] {
      for (int i = 0; i < per_producer; ++i) {
        jobs.submit([](const mv::job_context&) { return mv::status::ok; },
                    [&](mv::job_id, mv::generation, mv::status) { completions.fetch_add(1); });
      }
    });
  }
  for (auto& t : threads) t.join();

  REQUIRE(wait_for([&] { return completions.load() == producers * per_producer; }, 30s));
  REQUIRE(jobs.submitted() == producers * per_producer);
}

TEST_CASE("job exceptions complete with errors and the worker survives callbacks", "[core][jobs]") {
  mv::job_system jobs;
  REQUIRE(jobs.start(1) == mv::status::ok);
  std::atomic<int> internal{0}, oom{0}, done{0};
  jobs.submit([](const mv::job_context&) -> mv::status { throw std::runtime_error("job"); },
              [&](mv::job_id, mv::generation, mv::status s) {
                if (s == mv::status::internal) ++internal;
                throw std::runtime_error("callback");
              });
  jobs.submit([](const mv::job_context&) -> mv::status { throw std::bad_alloc(); },
              [&](mv::job_id, mv::generation, mv::status s) {
                if (s == mv::status::out_of_memory) ++oom;
              });
  jobs.submit([](const mv::job_context&) { return mv::status::ok; },
              [&](mv::job_id, mv::generation, mv::status) { ++done; });
  REQUIRE(wait_for([&] { return done.load() == 1; }));
  REQUIRE(internal.load() == 1);
  REQUIRE(oom.load() == 1);
  REQUIRE(jobs.completed() == 3);
}

TEST_CASE("view-tied work does not queue behind the background sweep",
          "[core][jobs][priority]") {
  // Opening a camera dump queues a thumbnail job per file at
  // background_generation. With one FIFO queue the first arrow press landed
  // behind all of them and the image took a second to appear. Foreground work
  // jumps that queue.
  mv::job_system jobs;
  REQUIRE(jobs.start(1) == mv::status::ok);

  std::atomic<bool> release{false};
  std::atomic<int> order{0};
  std::atomic<int> view_rank{-1};

  // Occupy the single worker so everything after this queues up behind it.
  jobs.submit_at(mv::background_generation, [&](const mv::job_context&) {
    while (!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    return mv::status::ok;
  });

  constexpr int sweep = 200;
  for (int i = 0; i < sweep; ++i) {
    jobs.submit_at(mv::background_generation, [&](const mv::job_context&) {
      order.fetch_add(1);
      return mv::status::ok;
    });
  }

  // Submitted last; must run first.
  jobs.submit([&](const mv::job_context&) {
    view_rank.store(order.fetch_add(1));
    return mv::status::ok;
  });

  release.store(true, std::memory_order_release);
  REQUIRE(wait_for([&] { return view_rank.load() >= 0; }));
  REQUIRE(view_rank.load() == 0);

  jobs.shutdown();
}
