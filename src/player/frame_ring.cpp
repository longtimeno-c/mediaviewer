// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5a - the presentation ring of textures WE own (plan/05), and the bounded
// packet queue that feeds the decoder.
//
// OWNER: mediaviewer-48 (5a).
#include "core/trace.h"
#include "player/video_internal.h"

namespace mv::player {

// ---------------------------------------------------------------------------
// packet_queue
// ---------------------------------------------------------------------------

bool packet_queue::push(packet_ptr packet) noexcept {
  std::unique_lock lock(mutex_);
  not_full_.wait(lock, [this] {
    return stopped_.load(std::memory_order_acquire) || items_.size() < packet_queue_capacity;
  });
  if (stopped_.load(std::memory_order_acquire)) return false;
  items_.push_back({std::move(packet), 0});
  lock.unlock();
  not_empty_.notify_one();
  return true;
}

bool packet_queue::pop(packet_ptr& out) noexcept {
  std::unique_lock lock(mutex_);
  not_empty_.wait(lock,
                  [this] { return stopped_.load(std::memory_order_acquire) || !items_.empty(); });
  if (items_.empty()) return false;  // stopped and drained
  out = std::move(items_.front().packet);
  items_.pop_front();
  lock.unlock();
  not_full_.notify_one();
  return true;
}

bool packet_queue::try_pop(packet_ptr& out) noexcept {
  return try_pop(out, nullptr);
}

bool packet_queue::try_push(packet_ptr& packet, std::uint32_t generation) noexcept {
  std::lock_guard lock(mutex_);
  if (stopped_.load(std::memory_order_acquire) || items_.size() >= packet_queue_capacity) return false;
  items_.push_back({std::move(packet), generation});
  not_empty_.notify_one();
  return true;
}

bool packet_queue::try_pop(packet_ptr& out, std::uint32_t* generation) noexcept {
  std::unique_lock lock(mutex_);
  if (items_.empty()) return false;
  if (generation) *generation = items_.front().generation;
  out = std::move(items_.front().packet);
  items_.pop_front();
  lock.unlock();
  not_full_.notify_one();
  return true;
}

void packet_queue::stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    stopped_.store(true, std::memory_order_release);
  }
  not_empty_.notify_all();
  not_full_.notify_all();
}

void packet_queue::clear() noexcept {
  std::lock_guard lock(mutex_);
  items_.clear();
}

void packet_queue::flush() noexcept {
  {
    std::lock_guard lock(mutex_);
    items_.clear();
  }
  // A demux thread blocked on a full queue has to be told the queue drained, or
  // a seek deadlocks against its own flush.
  not_full_.notify_all();
}

std::size_t packet_queue::size() const noexcept {
  std::lock_guard lock(mutex_);
  return items_.size();
}

// ---------------------------------------------------------------------------
// frame_ring
// ---------------------------------------------------------------------------

frame_ring::~frame_ring() { destroy(); }

expected frame_ring::create_slot(video_frame& slot) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = texture_w_;
  desc.Height = texture_h_;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = ten_bit_ ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  desc.SampleDesc = {1, 0};
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  gfx::com_ptr<ID3D11Texture2D> texture;
  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, texture.GetAddressOf());
  if (FAILED(hr)) return err(hr == E_OUTOFMEMORY ? status::out_of_memory : status::internal);

  // plan/05's SRV table. Getting the 10-bit pair wrong does not fail to draw —
  // it draws the wrong colours, which is why the table is spelled out rather
  // than derived.
  D3D11_SHADER_RESOURCE_VIEW_DESC luma{};
  luma.Format = ten_bit_ ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
  luma.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  luma.Texture2D.MipLevels = 1;

  D3D11_SHADER_RESOURCE_VIEW_DESC chroma = luma;
  chroma.Format = ten_bit_ ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

  hr = device_->CreateShaderResourceView(texture.Get(), &luma, slot.luma.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);
  hr = device_->CreateShaderResourceView(texture.Get(), &chroma, slot.chroma.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  slot.texture = std::move(texture);
  slot.ten_bit = ten_bit_;
  return {};
}

expected frame_ring::create(ID3D11Device* device, std::uint32_t texture_w,
                            std::uint32_t texture_h, bool ten_bit) {
  destroy();
  if (!device || texture_w == 0 || texture_h == 0) return err(status::invalid_arg);
  // 4:2:0 needs both dimensions even. A decoder allocation always is, but this
  // is also reached from the software path where the frame size is the file's.
  if ((texture_w & 1u) || (texture_h & 1u)) return err(status::invalid_arg);

  device_ = device;
  texture_w_ = texture_w;
  texture_h_ = texture_h;
  ten_bit_ = ten_bit;

  for (std::uint32_t i = 0; i < frame_ring_slots; ++i) {
    if (auto r = create_slot(slots_[i]); !r) {
      destroy();
      return r;
    }
  }

  // Every slot starts free. Done before the threads exist, so the single-
  // producer rule on free_ is not in force yet.
  for (std::uint32_t i = 0; i < frame_ring_slots; ++i) free_slots_[i] = i;
  free_head_.store(frame_ring_slots, std::memory_order_relaxed);
  free_tail_.store(0, std::memory_order_relaxed);
  ready_head_.store(0, std::memory_order_relaxed);
  ready_tail_.store(0, std::memory_order_relaxed);
  pending_index_ = index_capacity;

  initialized_.store(true, std::memory_order_release);
  MV_LOG_INFO("player: presentation ring %ux%u %s, %u slots", texture_w_, texture_h_,
              ten_bit_ ? "P010" : "NV12", frame_ring_slots);
  return {};
}

void frame_ring::destroy() noexcept {
  initialized_.store(false, std::memory_order_release);
  for (auto& slot : slots_) slot = video_frame{};
  device_ = nullptr;
  texture_w_ = texture_h_ = 0;
  ten_bit_ = false;
  free_head_.store(0, std::memory_order_relaxed);
  free_tail_.store(0, std::memory_order_relaxed);
  ready_head_.store(0, std::memory_order_relaxed);
  ready_tail_.store(0, std::memory_order_relaxed);
  pending_index_ = index_capacity;
}

video_frame* frame_ring::begin_write() noexcept {
  if (!valid()) return nullptr;
  // A slot taken and not committed stays taken: handing it back to free_ would
  // make the decode thread a second producer there.
  if (pending_index_ < frame_ring_slots) return &slots_[pending_index_];

  const std::uint32_t tail = free_tail_.load(std::memory_order_relaxed);
  if (tail == free_head_.load(std::memory_order_acquire)) return nullptr;  // none free
  pending_index_ = free_slots_[tail];
  free_tail_.store((tail + 1) % index_capacity, std::memory_order_release);
  return &slots_[pending_index_];
}

void frame_ring::commit() noexcept {
  if (pending_index_ >= frame_ring_slots) return;
  const std::uint32_t head = ready_head_.load(std::memory_order_relaxed);
  const std::uint32_t next = (head + 1) % index_capacity;
  // Cannot happen: the ready ring has one more cell than there are slots, and a
  // slot can only be in one of free/ready/pending. Checked rather than assumed
  // because silently overwriting would present a frame the render thread holds.
  if (next == ready_tail_.load(std::memory_order_acquire)) return;
  ready_slots_[head] = pending_index_;
  ready_head_.store(next, std::memory_order_release);
  pending_index_ = index_capacity;
}

video_frame* frame_ring::acquire(std::uint32_t generation, time_ns deadline_ns) noexcept {
  if (!valid()) return nullptr;
  for (;;) {
    const std::uint32_t tail = ready_tail_.load(std::memory_order_relaxed);
    if (tail == ready_head_.load(std::memory_order_acquire)) return nullptr;  // empty
    video_frame* frame = &slots_[ready_slots_[tail]];

    if (frame->generation != generation) {
      // Stale: decoded before a navigation or a seek. Recycle it here rather
      // than present it (plan/02 generation counters).
      ready_tail_.store((tail + 1) % index_capacity, std::memory_order_release);
      release(frame);
      continue;
    }
    if (frame->pts_ns > deadline_ns) return nullptr;  // not due yet — cadence hold

    ready_tail_.store((tail + 1) % index_capacity, std::memory_order_release);
    return frame;
  }
}

void frame_ring::drop_oldest() noexcept {
  const std::uint32_t tail = ready_tail_.load(std::memory_order_relaxed);
  if (tail == ready_head_.load(std::memory_order_acquire)) return;
  video_frame* frame = &slots_[ready_slots_[tail]];
  ready_tail_.store((tail + 1) % index_capacity, std::memory_order_release);
  release(frame);
}

void frame_ring::release(video_frame* frame) noexcept {
  if (!frame) return;
  const auto index = static_cast<std::uint32_t>(frame - slots_);
  if (index >= frame_ring_slots) return;  // not ours
  const std::uint32_t head = free_head_.load(std::memory_order_relaxed);
  const std::uint32_t next = (head + 1) % index_capacity;
  if (next == free_tail_.load(std::memory_order_acquire)) return;  // cannot happen
  free_slots_[head] = index;
  free_head_.store(next, std::memory_order_release);
}

bool frame_ring::peek_next_pts(time_ns* out_pts_ns) const noexcept {
  if (!out_pts_ns) return false;
  const std::uint32_t tail = ready_tail_.load(std::memory_order_acquire);
  if (tail == ready_head_.load(std::memory_order_acquire)) return false;
  // Safe to read without dequeuing: a slot only enters the ready ring after
  // commit() publishes it, and only leaves via the render thread, which is the
  // thread that would be asking. The decode thread never writes a slot that is
  // in the ready ring.
  *out_pts_ns = slots_[ready_slots_[tail]].pts_ns;
  return true;
}

std::uint32_t frame_ring::queued() const noexcept {
  const std::uint32_t head = ready_head_.load(std::memory_order_acquire);
  const std::uint32_t tail = ready_tail_.load(std::memory_order_acquire);
  return (head + index_capacity - tail) % index_capacity;
}

}  // namespace mv::player
