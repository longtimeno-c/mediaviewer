// SPDX-License-Identifier: GPL-2.0-or-later
// PR 19 -- the Core Audio endpoint behind audio_sink.h (D9 / plan/15): the
// counterpart of audio_win.cpp and the only place a host audio API is named.
// "Core Audio is the master clock. AVPlayer is forbidden." (plan/10 PR 19)
//
// A default-output AudioUnit. It converts our float32 interleaved stream to the
// device's format and follows the system default device on its own; we still
// listen for the change so the clock's discontinuity accounting stays honest.
//
// Threading: write() is called from av_clock's audio thread (single producer),
// the render callback is the single consumer, so the sample ring needs no lock.
// The callback never allocates, locks, or waits (CLAUDE.md rule 1).
//
// PLAYED POSITION is not a wall clock. Every render callback records the
// (host time this buffer will reach the device, frames consumed before it)
// anchor; played_ns() extrapolates from the latest anchor with the host clock,
// capped at one callback's worth so it can never run ahead of what the device
// was actually given -- which is what keeps a paused or starved stream from
// reading as a ramp.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "core/trace.h"
#include "player/audio_sink.h"

namespace mv::player {
namespace {

[[nodiscard]] std::uint64_t host_ticks_to_ns(std::uint64_t ticks) noexcept {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
  }();
  // ticks * numer / denom, split to avoid overflowing 64 bits on long uptimes.
  const std::uint64_t whole = ticks / tb.denom;
  const std::uint64_t rem = ticks % tb.denom;
  return whole * tb.numer + rem * tb.numer / tb.denom;
}

class core_audio_sink final : public audio_sink {
 public:
  ~core_audio_sink() override { close(); }

  [[nodiscard]] expected open(std::uint32_t sample_rate, std::uint32_t channels) override {
    close();
    if (sample_rate == 0 || channels == 0 || channels > 8) return err(status::invalid_arg);
    rate_ = sample_rate;
    channels_ = channels;

    // ~1.4 s of headroom, a power of two so indices wrap with a mask.
    capacity_frames_ = 1u << 16;
    ring_.assign(static_cast<std::size_t>(capacity_frames_) * channels_, 0.0f);
    write_pos_.store(0, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
    anchor_seq_.store(0, std::memory_order_relaxed);
    have_anchor_.store(false, std::memory_order_relaxed);
    discontinuities_.store(0, std::memory_order_relaxed);
    device_changed_.store(false, std::memory_order_relaxed);

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component || AudioComponentInstanceNew(component, &unit_) != noErr) {
      unit_ = nullptr;
      return err(status::unsupported_format);
    }

    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<Float64>(rate_);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerFrame = sizeof(float) * channels_;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame;
    format.mChannelsPerFrame = channels_;
    format.mBitsPerChannel = 32;
    AURenderCallbackStruct callback{&core_audio_sink::render, this};
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                             &format, sizeof(format)) != noErr ||
        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &callback, sizeof(callback)) != noErr ||
        AudioUnitInitialize(unit_) != noErr) {
      close();
      return err(status::internal);
    }

    UInt32 frames = 0;
    UInt32 size = sizeof(frames);
    if (AudioUnitGetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
                             &frames, &size) == noErr) {
      period_frames_ = frames;
    } else {
      period_frames_ = 512;
    }

    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    listening_ = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                                &core_audio_sink::device_listener, this) == noErr;

    if (AudioOutputUnitStart(unit_) != noErr) {
      close();
      return err(status::internal);
    }
    started_ = true;
    MV_LOG_INFO("player: Core Audio output %u Hz, %u ch, period %u frames", rate_, channels_,
                period_frames_);
    return {};
  }

  void close() noexcept override {
    if (listening_) {
      AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain};
      AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr,
                                        &core_audio_sink::device_listener, this);
      listening_ = false;
    }
    if (unit_) {
      // Stops the render callback (and waits for one in flight) before the ring
      // it reads goes away.
      AudioOutputUnitStop(unit_);
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
    }
    started_ = false;
  }

  [[nodiscard]] audio_endpoint_info info() const noexcept override {
    audio_endpoint_info out;
    out.sample_rate = rate_;
    out.channels = channels_;
    out.buffer_frames = capacity_frames_;
    out.period_frames = period_frames_;
    return out;
  }

  [[nodiscard]] std::uint32_t write(const float* interleaved, std::uint32_t frames) noexcept override {
    if (!unit_ || !interleaved || frames == 0) return 0;
    const std::uint64_t w = write_pos_.load(std::memory_order_relaxed);
    const std::uint64_t r = read_pos_.load(std::memory_order_acquire);
    const std::uint64_t used = w - r;
    const std::uint32_t space = static_cast<std::uint32_t>(capacity_frames_ - used);
    const std::uint32_t n = std::min(frames, space);
    const std::uint32_t mask = capacity_frames_ - 1;
    for (std::uint32_t i = 0; i < n; ++i) {
      std::memcpy(&ring_[static_cast<std::size_t>((w + i) & mask) * channels_],
                  interleaved + static_cast<std::size_t>(i) * channels_, sizeof(float) * channels_);
    }
    write_pos_.store(w + n, std::memory_order_release);
    return n;
  }

  [[nodiscard]] time_ns played_ns(std::uint64_t* out_discontinuity) const noexcept override {
    if (out_discontinuity) *out_discontinuity = discontinuities_.load(std::memory_order_relaxed);
    if (!have_anchor_.load(std::memory_order_acquire) || rate_ == 0) return 0;

    // Seqlock read of the (host_ns, frames, span) anchor.
    std::uint64_t host_ns = 0, frames = 0, span = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
      const std::uint32_t before = anchor_seq_.load(std::memory_order_acquire);
      if (before & 1u) continue;
      host_ns = anchor_host_ns_;
      frames = anchor_frames_;
      span = anchor_span_frames_;
      if (anchor_seq_.load(std::memory_order_acquire) == before) break;
    }

    // SIGNED offset from the anchor. ts->mHostTime is when this buffer's first
    // frame reaches the device, which is always in the FUTURE by the output
    // latency, so `now - host` is negative for most of each callback period. An
    // unsigned "only if now > host" version never extrapolates at all: the
    // position then steps by one callback (~11 ms) at a time, and a 60 Hz
    // presenter sampling that staircase drops a third of a 30 fps clip as "late".
    const std::uint64_t now_ns = host_ticks_to_ns(mach_absolute_time());
    const double since_anchor_ns =
        static_cast<double>(static_cast<std::int64_t>(now_ns) - static_cast<std::int64_t>(host_ns));
    double ahead_frames = since_anchor_ns * rate_ / 1e9;
    // Never past the end of the last buffer we handed the device: a paused or
    // starved stream holds its position instead of ramping.
    ahead_frames = std::min(ahead_frames, static_cast<double>(span));
    const double played = std::max(0.0, static_cast<double>(frames) + ahead_frames);
    return static_cast<time_ns>(played * 1e9 / rate_);
  }

  [[nodiscard]] bool device_changed() const noexcept override {
    return device_changed_.load(std::memory_order_acquire);
  }

  void set_paused(bool paused) noexcept override {
    if (!unit_ || paused_ == paused) return;
    paused_ = paused;
    if (paused) {
      AudioOutputUnitStop(unit_);
    } else {
      AudioOutputUnitStart(unit_);
    }
  }

  void set_volume(float volume) noexcept override {
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  void set_muted(bool muted) noexcept override { muted_.store(muted, std::memory_order_relaxed); }

 private:
  static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32,
                         UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<core_audio_sink*>(ref);
    return self->render_impl(ts, frames, io);
  }

  OSStatus render_impl(const AudioTimeStamp* ts, UInt32 frames, AudioBufferList* io) noexcept {
    if (!io || io->mNumberBuffers == 0) return noErr;
    float* out = static_cast<float*>(io->mBuffers[0].mData);
    const std::uint32_t channels = channels_;
    const std::uint32_t mask = capacity_frames_ - 1;

    const std::uint64_t r = read_pos_.load(std::memory_order_relaxed);
    const std::uint64_t w = write_pos_.load(std::memory_order_acquire);
    const std::uint32_t avail = static_cast<std::uint32_t>(w - r);
    const std::uint32_t n = std::min<std::uint32_t>(frames, avail);
    const float gain = muted_.load(std::memory_order_relaxed) ? 0.0f
                                                              : volume_.load(std::memory_order_relaxed);

    for (std::uint32_t i = 0; i < n; ++i) {
      const float* src = &ring_[static_cast<std::size_t>((r + i) & mask) * channels];
      for (std::uint32_t c = 0; c < channels; ++c) out[static_cast<std::size_t>(i) * channels + c] = src[c] * gain;
    }
    if (n < frames) {  // starved: silence, and the anchor below stops advancing
      std::memset(out + static_cast<std::size_t>(n) * channels, 0,
                  sizeof(float) * channels * (frames - n));
    }

    // Anchor: this buffer starts reaching the device at ts->mHostTime, and `r`
    // frames of real audio precede it.
    if (ts && (ts->mFlags & kAudioTimeStampHostTimeValid)) {
      const std::uint32_t seq = anchor_seq_.load(std::memory_order_relaxed);
      anchor_seq_.store(seq + 1, std::memory_order_release);  // odd = writing
      anchor_host_ns_ = host_ticks_to_ns(ts->mHostTime);
      anchor_frames_ = r;
      anchor_span_frames_ = n;
      anchor_seq_.store(seq + 2, std::memory_order_release);
      have_anchor_.store(true, std::memory_order_release);
    }
    read_pos_.store(r + n, std::memory_order_release);
    return noErr;
  }

  static OSStatus device_listener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*,
                                  void* ref) {
    auto* self = static_cast<core_audio_sink*>(ref);
    self->device_changed_.store(true, std::memory_order_release);
    self->discontinuities_.fetch_add(1, std::memory_order_relaxed);
    return noErr;
  }

  AudioComponentInstance unit_ = nullptr;
  std::uint32_t rate_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t period_frames_ = 0;
  std::uint32_t capacity_frames_ = 0;
  std::vector<float> ring_;
  bool started_ = false;
  bool listening_ = false;
  bool paused_ = false;

  alignas(64) std::atomic<std::uint64_t> write_pos_{0};  // frames, producer
  alignas(64) std::atomic<std::uint64_t> read_pos_{0};   // frames, callback

  // Position anchor, written by the callback under a seqlock.
  std::atomic<std::uint32_t> anchor_seq_{0};
  std::atomic<bool> have_anchor_{false};
  std::uint64_t anchor_host_ns_ = 0;
  std::uint64_t anchor_frames_ = 0;
  std::uint64_t anchor_span_frames_ = 0;

  std::atomic<std::uint64_t> discontinuities_{0};
  std::atomic<bool> device_changed_{false};
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> muted_{false};
};

}  // namespace

result<audio_sink*> create_audio_sink() {
  auto* sink = new (std::nothrow) core_audio_sink();
  if (!sink) return err(status::out_of_memory);
  return static_cast<audio_sink*>(sink);
}

void destroy_audio_sink(audio_sink* sink) noexcept { delete sink; }

}  // namespace mv::player
