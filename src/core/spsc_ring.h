// SPDX-License-Identifier: GPL-2.0-or-later
// Single-producer / single-consumer ring of POD messages.
//
// plan/02-architecture.md: "Communication is exclusively single-producer/
// single-consumer ring buffers of POD messages plus one MPMC job queue."
//
// Lock-free and wait-free on both ends, bounded. Full means the producer drops
// or backs off — it never blocks, because the producer may be the render thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mv {

// Hardware destructive interference size. 64 on every target we build for;
// hard-coded rather than depending on <new>'s optional constant.
inline constexpr std::size_t cache_line = 64;

template <typename T, std::size_t Capacity>
class spsc_ring {
  static_assert(std::is_trivially_copyable_v<T>, "SPSC ring carries POD only");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

 public:
  static constexpr std::size_t capacity = Capacity;

  // Producer side. Returns false when full; the caller decides whether to drop
  // or retry. Never blocks.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & (Capacity - 1);
    if (next == tail_.load(std::memory_order_acquire)) return false;  // full
    slots_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false when empty. Never blocks.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;  // empty
    out = slots_[tail];
    tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & (Capacity - 1);
  }

 private:
  alignas(cache_line) std::atomic<std::size_t> head_{0};
  alignas(cache_line) std::atomic<std::size_t> tail_{0};
  alignas(cache_line) T slots_[Capacity]{};
};

// Lock-free snapshot publish: the UI thread publishes a state snapshot, the
// render thread consumes one, and they never share a mutable object
// (plan/02-architecture.md, "The frame loop").
//
// A wait-free triple buffer. Three slots and one atomic; the producer owns one
// index, the consumer owns another, and they hand slots over through the third
// with an exchange. Neither side ever writes a slot the other is holding, so
// there is nothing to tear, and neither side ever waits.
//
// Two simpler things were tried first and are recorded here because both look
// correct:
//
//   - **Double buffering.** With one producer and two slots, a consumer still
//     copying the slot that was live two publishes ago is overwritten mid-copy
//     and reads a torn snapshot.
//
//   - **A seqlock.** Correct, but the reader retries while a publish is in
//     flight — so a producer in a tight loop starves it indefinitely. The
//     render thread is the consumer here. A primitive it can livelock on is the
//     wrong primitive, however rarely the UI thread would really publish that
//     fast.
template <typename T>
class publish_slot {
  static_assert(std::is_trivially_copyable_v<T>, "snapshots are POD");

  static constexpr std::uint32_t index_mask = 0x3u;
  static constexpr std::uint32_t dirty_bit = 0x4u;

 public:
  publish_slot() noexcept = default;

  publish_slot(const publish_slot&) = delete;
  publish_slot& operator=(const publish_slot&) = delete;

  // Producer thread only. One producer, enforced by convention.
  void publish(const T& value) noexcept {
    slots_[write_index_] = value;
    const std::uint32_t previous =
        shared_.exchange(write_index_ | dirty_bit, std::memory_order_acq_rel);
    // Take whatever slot the consumer is not holding.
    write_index_ = previous & index_mask;
  }

  // Consumer thread only. Wait-free: at most one exchange, never a retry.
  // Returns the last published snapshot, or the default-constructed value if
  // nothing has been published yet.
  [[nodiscard]] T acquire() noexcept {
    if (shared_.load(std::memory_order_acquire) & dirty_bit) {
      const std::uint32_t previous =
          shared_.exchange(read_index_, std::memory_order_acq_rel);
      read_index_ = previous & index_mask;
    }
    return slots_[read_index_];
  }

 private:
  // The three indices — write_index_, read_index_, and the one inside shared_ —
  // are distinct at all times, because every handover is an exchange.
  alignas(cache_line) std::atomic<std::uint32_t> shared_{2};
  std::uint32_t write_index_{0};
  std::uint32_t read_index_{1};
  alignas(cache_line) T slots_[3]{};
};

}  // namespace mv
