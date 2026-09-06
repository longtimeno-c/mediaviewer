// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/spsc_ring.h"

namespace {

struct message {
  std::uint64_t sequence;
  std::uint32_t payload;
  std::uint32_t padding;
};

}  // namespace

TEST_CASE("spsc_ring is FIFO and bounded", "[core][ring]") {
  mv::spsc_ring<message, 4> ring;
  REQUIRE(ring.empty());

  // Capacity N holds N-1: one slot is spent distinguishing full from empty.
  REQUIRE(ring.try_push(message{1, 10, 0}));
  REQUIRE(ring.try_push(message{2, 20, 0}));
  REQUIRE(ring.try_push(message{3, 30, 0}));
  REQUIRE_FALSE(ring.try_push(message{4, 40, 0}));

  message out{};
  REQUIRE(ring.try_pop(out));
  REQUIRE(out.sequence == 1);
  REQUIRE(out.payload == 10);

  // A pop frees exactly one slot.
  REQUIRE(ring.try_push(message{4, 40, 0}));

  REQUIRE(ring.try_pop(out));
  REQUIRE(out.sequence == 2);
  REQUIRE(ring.try_pop(out));
  REQUIRE(out.sequence == 3);
  REQUIRE(ring.try_pop(out));
  REQUIRE(out.sequence == 4);
  REQUIRE_FALSE(ring.try_pop(out));
  REQUIRE(ring.empty());
}

TEST_CASE("spsc_ring survives a real producer and consumer", "[core][ring]") {
  // Small ring, large volume: the producer will hit "full" repeatedly, which is
  // the case that actually exercises the wraparound arithmetic.
  static constexpr std::uint64_t total = 200000;
  mv::spsc_ring<message, 64> ring;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < total;) {
      if (ring.try_push(message{i, static_cast<std::uint32_t>(i & 0xffffu), 0})) ++i;
      else std::this_thread::yield();
    }
  });

  std::uint64_t expected = 0;
  bool ordered = true;
  bool payload_intact = true;
  while (expected < total) {
    message out{};
    if (!ring.try_pop(out)) {
      std::this_thread::yield();
      continue;
    }
    if (out.sequence != expected) ordered = false;
    if (out.payload != static_cast<std::uint32_t>(expected & 0xffffu)) payload_intact = false;
    ++expected;
  }
  producer.join();

  REQUIRE(ordered);
  REQUIRE(payload_intact);
  REQUIRE(ring.empty());
}

namespace {

struct triple {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
};

}  // namespace

TEST_CASE("publish_slot hands back the newest snapshot", "[core][publish]") {
  mv::publish_slot<triple> slot;

  // Nothing published yet: a well-defined value, not uninitialised storage.
  const triple initial = slot.acquire();
  REQUIRE(initial.a == 0);

  slot.publish(triple{1, 2, 3});
  REQUIRE(slot.acquire().a == 1);
  // Reading twice without an intervening publish is stable.
  REQUIRE(slot.acquire().a == 1);

  slot.publish(triple{4, 8, 12});
  slot.publish(triple{5, 10, 15});
  // The consumer sees the newest, not a queue — this is a snapshot, not a ring.
  const triple newest = slot.acquire();
  REQUIRE(newest.a == 5);
  REQUIRE(newest.b == 10);
}

TEST_CASE("publish_slot cannot be starved by a hot producer", "[core][publish]") {
  // The consumer here is the render thread. A publish primitive it can livelock
  // on is the wrong primitive, and a seqlock — which retries while a publish is
  // in flight — is exactly that: under a producer in a tight loop its reader
  // makes no progress at all.
  mv::publish_slot<triple> slot;
  slot.publish(triple{0, 0, 0});

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      (void)slot.acquire();
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // Let the reader be scheduled before judging it. Counting from thread
  // creation measures how fast this machine starts threads, not whether the
  // primitive starves.
  while (reads.load(std::memory_order_relaxed) == 0) std::this_thread::yield();

  // Measure progress made *while the producer is hot*, over wall time rather
  // than over a publish count: a fixed iteration count finishes in microseconds
  // on an idle box and in milliseconds on a loaded one, which makes any
  // threshold a coin toss rather than a property.
  const std::uint64_t before = reads.load(std::memory_order_relaxed);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  std::uint64_t published = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    for (int i = 0; i < 1000; ++i) {
      ++published;
      slot.publish(triple{published, published * 2, published * 3});
    }
  }
  const std::uint64_t during = reads.load(std::memory_order_relaxed) - before;

  stop.store(true, std::memory_order_relaxed);
  reader.join();

  // Not "some reads happened" — the consumer must keep pace under load, not
  // crawl. A livelocked reader scores exactly zero here.
  REQUIRE(published > 1000);
  REQUIRE(during > 1000);
}

TEST_CASE("publish_slot never hands back a torn snapshot", "[core][publish]") {
  // The UI thread publishes; the render thread reads. A torn read here is the
  // bug that shows up as one frame of nonsense geometry and is then blamed on
  // the GPU for a week.
  mv::publish_slot<triple> slot;
  slot.publish(triple{0, 0, 0});

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<bool> torn{false};

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const triple s = slot.acquire();
      // Every published snapshot satisfies b == a * 2 and c == a * 3.
      if (s.b != s.a * 2 || s.c != s.a * 3) torn.store(true, std::memory_order_relaxed);
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::uint64_t i = 1; i <= 500000; ++i) slot.publish(triple{i, i * 2, i * 3});

  stop.store(true, std::memory_order_relaxed);
  reader.join();

  REQUIRE(reads.load() > 0);
  REQUIRE_FALSE(torn.load());
}
