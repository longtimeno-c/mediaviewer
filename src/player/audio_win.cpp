// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5b - WASAPI shared-mode render client. Windows port (D9 / plan/15).
//
// OWNER: mediaviewer-08 (5b). This is the ONLY file in player/ permitted to
// include <audioclient.h> / <mmdeviceapi.h> / <audiopolicy.h>:
// tools/check-hostable-core.ps1 exempts *_win.cpp by name and now bans those
// headers everywhere else. A Core Audio host replaces this file and nothing else.
//
// Shape, and why: the endpoint owns its own render thread, because WASAPI's
// event-driven mode is a pull. The alternative — a passive sink the clock polls
// — costs either a Sleep loop or a busy-wait, and plan/03 rule 1 says never
// Sleep. So there are exactly two threads and one lock-free handoff:
//
//     decode thread  --write()-->  sample_fifo  --render thread-->  WASAPI
//
// The render thread touches ONLY the fifo and the render client. It never sees
// FFmpeg, never allocates, never takes a lock (CLAUDE.md rule 1). Starved, it
// writes silence and counts it rather than stalling the endpoint.
#include "player/audio_sink.h"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "core/spsc_ring.h"  // mv::cache_line
#include "core/trace.h"

namespace mv::player {
namespace {

constexpr std::int64_t ns_per_second = 1'000'000'000;

// A lock-free float ring. core/spsc_ring.h carries fixed-size POD elements by
// value; audio is a stream of samples whose block boundary does not line up
// with what the endpoint asks for, so this is the same acquire/release
// discipline over a flat sample buffer. Single producer (decode), single
// consumer (render). Never blocks on either end.
class sample_fifo {
 public:
  // Sized in samples, rounded up to a power of two so the wrap is a mask.
  void reset(std::size_t capacity_samples) {
    std::size_t capacity = 1;
    while (capacity < capacity_samples) capacity <<= 1;
    samples_.assign(capacity, 0.0f);
    mask_ = capacity - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  void clear() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_release);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return samples_.size(); }

  [[nodiscard]] std::size_t available() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & mask_;
  }

  [[nodiscard]] std::size_t space() const noexcept {
    return mask_ == 0 ? 0 : mask_ - available();
  }

  // [producer] Copies as much as fits. Returns samples taken; never blocks.
  [[nodiscard]] std::size_t push(const float* in, std::size_t count) noexcept {
    if (mask_ == 0) return 0;
    const std::size_t taken = std::min(count, space());
    std::size_t head = head_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < taken; ++i) {
      samples_[head] = in[i];
      head = (head + 1) & mask_;
    }
    head_.store(head, std::memory_order_release);
    return taken;
  }

  // [consumer] Fills `out` with up to `count` samples. Returns samples popped.
  [[nodiscard]] std::size_t pop(float* out, std::size_t count) noexcept {
    if (mask_ == 0) return 0;
    const std::size_t got = std::min(count, available());
    std::size_t tail = tail_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < got; ++i) {
      out[i] = samples_[tail];
      tail = (tail + 1) & mask_;
    }
    tail_.store(tail, std::memory_order_release);
    return got;
  }

 private:
  std::vector<float> samples_;
  std::size_t mask_ = 0;
  alignas(cache_line) std::atomic<std::size_t> head_{0};
  alignas(cache_line) std::atomic<std::size_t> tail_{0};
};

// Default-endpoint change notification. The verify line is "unplugging the
// audio device mid-playback recovers without stopping video", so this only
// ever sets a flag — the rebuild happens on the clock's thread, because doing
// COM work inside an MMDevice callback is how you deadlock the audio service.
class device_notifier final : public IMMNotificationClient {
 public:
  explicit device_notifier(std::atomic<bool>* flag) noexcept : flag_(flag) {}

  // Owned by the sink for its whole lifetime; the refcount exists to satisfy
  // COM, not to manage us.
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return static_cast<ULONG>(refs_.fetch_sub(1, std::memory_order_acq_rel) - 1);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                   LPCWSTR) override {
    if (flow == eRender && role == eConsole) flag_->store(true, std::memory_order_release);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD state) override {
    // Disabling or unplugging the endpoint we are on arrives here as well as
    // through AUDCLNT_E_DEVICE_INVALIDATED on the render thread, and which one
    // lands first is not deterministic. Both set the same flag.
    if (state != DEVICE_STATE_ACTIVE) flag_->store(true, std::memory_order_release);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
    flag_->store(true, std::memory_order_release);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }

 private:
  std::atomic<bool>* flag_;
  std::atomic<long> refs_{1};
};

// RAII for CoInitializeEx on a thread we created. RPC_E_CHANGED_MODE means
// someone already put this thread in an apartment; that is not our failure and
// not ours to undo.
class com_scope {
 public:
  com_scope() noexcept {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr);
  }
  ~com_scope() {
    if (owned_) ::CoUninitialize();
  }
  com_scope(const com_scope&) = delete;
  com_scope& operator=(const com_scope&) = delete;

 private:
  bool owned_ = false;
};

template <typename T>
void safe_release(T*& p) noexcept {
  if (p != nullptr) {
    p->Release();
    p = nullptr;
  }
}

class wasapi_sink final : public audio_sink {
 public:
  wasapi_sink() = default;

  ~wasapi_sink() override { close(); }

  [[nodiscard]] expected open(std::uint32_t sample_rate, std::uint32_t channels) override {
    if (sample_rate == 0 || channels == 0) return err(status::invalid_arg);
    close();

    com_scope com;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
      MV_LOG_ERROR("audio: MMDeviceEnumerator failed (0x%08lx)", static_cast<unsigned long>(hr));
      return err(status::io);
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
      // No endpoint at all — a machine with no sound card, or every device
      // disabled. The clock falls back to the host master and playback goes on;
      // this is not fatal (verify line: a clip with no audio still plays).
      safe_release(enumerator);
      MV_LOG_ERROR("audio: no default render endpoint (0x%08lx)",
                   static_cast<unsigned long>(hr));
      return err(status::io);
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client_));
    safe_release(device);
    if (FAILED(hr)) {
      safe_release(enumerator);
      MV_LOG_ERROR("audio: IAudioClient activate failed (0x%08lx)",
                   static_cast<unsigned long>(hr));
      return err(status::io);
    }

    if (!negotiate_format(sample_rate, channels)) {
      safe_release(enumerator);
      close();
      return err(status::unsupported_format);
    }

    // Zero duration with EVENTCALLBACK asks the engine for its own period,
    // which is the lowest latency shared mode offers and the size the event
    // will actually fire at.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             0, 0, format_, nullptr);
    if (FAILED(hr)) {
      safe_release(enumerator);
      close();
      MV_LOG_ERROR("audio: IAudioClient::Initialize failed (0x%08lx)",
                   static_cast<unsigned long>(hr));
      return err(status::io);
    }

    hr = client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
      safe_release(enumerator);
      close();
      return err(status::io);
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    if (SUCCEEDED(client_->GetDevicePeriod(&default_period, &min_period))) {
      // REFERENCE_TIME is 100 ns units.
      period_frames_ = static_cast<std::uint32_t>(
          (static_cast<std::int64_t>(default_period) * sample_rate_) / 10'000'000);
    }

    hr = client_->GetService(__uuidof(IAudioRenderClient),
                             reinterpret_cast<void**>(&render_));
    if (FAILED(hr)) {
      safe_release(enumerator);
      close();
      return err(status::io);
    }

    hr = client_->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock_));
    if (FAILED(hr)) {
      // Without IAudioClock there is no "samples actually played" and therefore
      // no audio master. Refuse rather than silently substituting a wall clock:
      // plan/05 is explicit that a wall clock produces a slow drift ramp that
      // reads as a decode bug.
      safe_release(enumerator);
      close();
      MV_LOG_ERROR("audio: IAudioClock unavailable (0x%08lx)",
                   static_cast<unsigned long>(hr));
      return err(status::unsupported_format);
    }
    if (FAILED(clock_->GetFrequency(&clock_frequency_)) || clock_frequency_ == 0) {
      safe_release(enumerator);
      close();
      return err(status::io);
    }

    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr) {
      safe_release(enumerator);
      close();
      return err(status::out_of_memory);
    }
    if (FAILED(client_->SetEventHandle(event_))) {
      safe_release(enumerator);
      close();
      return err(status::io);
    }

    // Two engine periods of slack in the fifo. Less and a decode hiccup is an
    // audible underrun; more and a seek has to discard audio the user already
    // committed to hearing.
    fifo_.reset(static_cast<std::size_t>(buffer_frames_) * channels_ * 4);
    scratch_.assign(static_cast<std::size_t>(buffer_frames_) * channels_, 0.0f);

    // Register for device change only once the stream is otherwise good, so a
    // failed open cannot leave a live callback pointing at a dead sink.
    notifier_ = new (std::nothrow) device_notifier(&device_changed_);
    if (notifier_ != nullptr &&
        SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(notifier_))) {
      enumerator_ = enumerator;  // kept so we can unregister in close()
    } else {
      safe_release(enumerator);
      if (notifier_ != nullptr) {
        notifier_->Release();
        notifier_ = nullptr;
      }
      // Losing the notification is a degraded mode, not a failure: the render
      // thread still sees AUDCLNT_E_DEVICE_INVALIDATED and sets the same flag.
      MV_LOG_ERROR("audio: endpoint notifications unavailable; relying on the render thread");
    }

    hr = client_->Start();
    if (FAILED(hr)) {
      close();
      return err(status::io);
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { render_loop(); });
    return {};
  }

  void close() noexcept override {
    running_.store(false, std::memory_order_release);
    if (event_ != nullptr) ::SetEvent(event_);
    if (thread_.joinable()) thread_.join();

    if (client_ != nullptr) client_->Stop();

    if (enumerator_ != nullptr && notifier_ != nullptr) {
      enumerator_->UnregisterEndpointNotificationCallback(notifier_);
    }
    if (notifier_ != nullptr) {
      notifier_->Release();
      notifier_ = nullptr;
    }
    safe_release(enumerator_);
    safe_release(clock_);
    safe_release(render_);
    safe_release(client_);

    if (format_ != nullptr) {
      ::CoTaskMemFree(format_);
      format_ = nullptr;
    }
    if (event_ != nullptr) {
      ::CloseHandle(event_);
      event_ = nullptr;
    }

    fifo_.clear();
    buffer_frames_ = 0;
    period_frames_ = 0;
    clock_frequency_ = 0;
    last_position_.store(0, std::memory_order_relaxed);
    frames_written_.store(0, std::memory_order_relaxed);
    silence_fills_.store(0, std::memory_order_relaxed);
    device_changed_.store(false, std::memory_order_relaxed);
  }

  [[nodiscard]] audio_endpoint_info info() const noexcept override {
    audio_endpoint_info out;
    out.sample_rate = sample_rate_;
    out.channels = channels_;
    out.buffer_frames = buffer_frames_;
    out.period_frames = period_frames_;
    return out;
  }

  [[nodiscard]] std::uint32_t write(const float* interleaved,
                                    std::uint32_t frames) noexcept override {
    if (interleaved == nullptr || frames == 0 || channels_ == 0) return 0;
    // The FIFO capacity is a power of two minus one. Round available space
    // down to whole frames, or a partial stereo frame shifts channel alignment.
    const auto accepted = std::min<std::size_t>(frames, fifo_.space() / channels_);
    const std::size_t taken = fifo_.push(interleaved, accepted * channels_);
    return static_cast<std::uint32_t>(taken / channels_);
  }

  [[nodiscard]] time_ns played_ns(std::uint64_t* out_discontinuity) const noexcept override {
    if (clock_ == nullptr || clock_frequency_ == 0) {
      if (out_discontinuity != nullptr) {
        *out_discontinuity = discontinuities_.load(std::memory_order_relaxed);
      }
      return 0;
    }

    UINT64 position = 0;
    if (FAILED(clock_->GetPosition(&position, nullptr))) {
      discontinuities_.fetch_add(1, std::memory_order_relaxed);
      if (out_discontinuity != nullptr) {
        *out_discontinuity = discontinuities_.load(std::memory_order_relaxed);
      }
      return last_reported_ns_.load(std::memory_order_relaxed);
    }

    // IAudioClock::GetPosition already reports the stream position of the
    // sample being PLAYED, so it is "samples actually played" on its own.
    //
    // Do NOT also subtract GetCurrentPadding here. That subtraction belongs to
    // the other formulation of the same quantity — frames we wrote, minus the
    // frames still queued — and applying both puts the clock a whole endpoint
    // buffer behind, which shows up as a constant A/V offset and gets blamed on
    // the presenter. We compute that second formulation separately, below, as a
    // cross-check on this one.
    const std::int64_t now_ns = static_cast<std::int64_t>(
        (static_cast<double>(position) / static_cast<double>(clock_frequency_)) *
        static_cast<double>(ns_per_second));

    const std::int64_t previous = last_reported_ns_.load(std::memory_order_relaxed);
    // The position is monotonic within a stream. A step backwards means the
    // stream was reset under us (device change, engine restart), and a single
    // one of those poisons a regression slope — so it is counted and reported
    // rather than averaged over. Same contract as gfx::pacer's
    // statistics_discontinuities.
    if (now_ns + discontinuity_slack_ns < previous) {
      discontinuities_.fetch_add(1, std::memory_order_relaxed);
    }
    last_reported_ns_.store(now_ns, std::memory_order_relaxed);
    last_position_.store(position, std::memory_order_relaxed);

    if (out_discontinuity != nullptr) {
      *out_discontinuity = discontinuities_.load(std::memory_order_relaxed);
    }
    return now_ns;
  }

  [[nodiscard]] bool device_changed() const noexcept override {
    return device_changed_.load(std::memory_order_acquire);
  }

  void set_paused(bool value) noexcept override { if (paused_.exchange(value) != value && event_) ::SetEvent(event_); }

  void set_volume(float volume) noexcept override {
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
  }

  void set_muted(bool muted) noexcept override {
    muted_.store(muted, std::memory_order_relaxed);
  }

  // Diagnostic only, read by the clock for the overlay: the independent
  // estimate of the play position, from what we wrote minus what is still
  // queued. Disagreement with played_ns() is the signature of a wrong position
  // query — which plan/05 warns is the failure that looks like a decode bug.
  [[nodiscard]] time_ns written_minus_padding_ns() const noexcept {
    if (client_ == nullptr || sample_rate_ == 0) return 0;
    UINT32 padding = 0;
    if (FAILED(client_->GetCurrentPadding(&padding))) return 0;
    const std::int64_t written =
        static_cast<std::int64_t>(frames_written_.load(std::memory_order_relaxed));
    const std::int64_t played = written - static_cast<std::int64_t>(padding);
    if (played <= 0) return 0;
    return played * ns_per_second / static_cast<std::int64_t>(sample_rate_);
  }

  [[nodiscard]] std::uint64_t silence_fills() const noexcept {
    return silence_fills_.load(std::memory_order_relaxed);
  }

 private:
  // A position may legitimately repeat between two reads; only a real step
  // backwards is a discontinuity. One millisecond of slack absorbs the former.
  static constexpr std::int64_t discontinuity_slack_ns = 1'000'000;

  // Picks a float32 format the endpoint will take. The requested rate and
  // channel count are a preference, not a demand: shared mode plays at the
  // engine's rate, and swresample upstream converts to whatever we report back
  // through info(). Forcing the requested rate here would just move the
  // resampler into the audio engine where we cannot see its ratio.
  [[nodiscard]] bool negotiate_format(std::uint32_t sample_rate,
                                      std::uint32_t channels) noexcept {
    if (channels > 2) return false;
    auto* want = static_cast<WAVEFORMATEXTENSIBLE*>(::CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!want) return false;
    *want = {};
    want->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want->Format.nChannels = static_cast<WORD>(channels);
    want->Format.nSamplesPerSec = sample_rate;
    want->Format.wBitsPerSample = 32;
    want->Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    want->Format.nAvgBytesPerSec = sample_rate * want->Format.nBlockAlign;
    want->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want->Samples.wValidBitsPerSample = 32;
    want->dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    want->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    format_ = reinterpret_cast<WAVEFORMATEX*>(want);
    sample_rate_ = sample_rate; channels_ = channels;
    return true;
  }

  [[nodiscard]] static bool is_float32(const WAVEFORMATEX& format) noexcept {
    if (format.wBitsPerSample != 32) return false;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
      const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
      return ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
  }

  // The real-time thread. Everything it touches is preallocated.
  void render_loop() noexcept {
    com_scope com;

    // "Pro Audio" is the MMCSS task the audio stack itself uses; without it a
    // busy machine will preempt this thread and the underruns read as decode
    // problems. Failure is not fatal, just worse.
    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    bool stopped_for_pause = false;
    while (running_.load(std::memory_order_acquire)) {
      const DWORD wait = ::WaitForSingleObject(event_, 2000);
      if (!running_.load(std::memory_order_acquire)) break;
      if (wait != WAIT_OBJECT_0 && stopped_for_pause) continue;
      if (wait != WAIT_OBJECT_0) {
        // The endpoint stopped firing. On a device change the client is already
        // invalid; flag it and let the clock rebuild on its own thread.
        device_changed_.store(true, std::memory_order_release);
        continue;
      }

      if (paused_.load()) {
        if (!stopped_for_pause) { (void)client_->Stop(); stopped_for_pause = true; }
        continue;
      }
      if (stopped_for_pause) { (void)client_->Start(); stopped_for_pause = false; }
      UINT32 padding = 0;
      HRESULT hr = client_->GetCurrentPadding(&padding);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        device_changed_.store(true, std::memory_order_release);
        break;
      }
      if (FAILED(hr)) continue;

      if (buffer_frames_ <= padding) continue;
      const UINT32 frames = buffer_frames_ - padding;

      BYTE* data = nullptr;
      hr = render_->GetBuffer(frames, &data);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        device_changed_.store(true, std::memory_order_release);
        break;
      }
      if (FAILED(hr) || data == nullptr) continue;

      const std::size_t wanted = static_cast<std::size_t>(frames) * channels_;
      const std::size_t got = fifo_.pop(scratch_.data(), wanted);

      DWORD flags = 0;
      if (got == 0) {
        // Nothing decoded yet, or the decoder fell behind. Silence keeps the
        // endpoint running and the clock advancing; stalling it would stop the
        // master clock and take video down with it.
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
        silence_fills_.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (got < wanted) {
          std::memset(scratch_.data() + got, 0, (wanted - got) * sizeof(float));
          silence_fills_.fetch_add(1, std::memory_order_relaxed);
        }
        const float gain =
            muted_.load(std::memory_order_relaxed) ? 0.0f : volume_.load(std::memory_order_relaxed);
        if (gain != 1.0f) {
          for (std::size_t i = 0; i < wanted; ++i) scratch_[i] *= gain;
        }
        std::memcpy(data, scratch_.data(), wanted * sizeof(float));
      }

      hr = render_->ReleaseBuffer(frames, flags);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        device_changed_.store(true, std::memory_order_release);
        break;
      }
      frames_written_.fetch_add(frames, std::memory_order_relaxed);
    }

    if (mmcss != nullptr) ::AvRevertMmThreadCharacteristics(mmcss);
  }

  std::atomic<bool> paused_{false};
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  IAudioClock* clock_ = nullptr;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  device_notifier* notifier_ = nullptr;
  WAVEFORMATEX* format_ = nullptr;
  HANDLE event_ = nullptr;

  std::thread thread_;
  sample_fifo fifo_;
  std::vector<float> scratch_;  // preallocated; the render thread never allocates

  std::uint32_t sample_rate_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t buffer_frames_ = 0;
  std::uint32_t period_frames_ = 0;
  UINT64 clock_frequency_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<bool> device_changed_{false};
  std::atomic<bool> muted_{false};
  std::atomic<float> volume_{1.0f};
  std::atomic<std::uint64_t> frames_written_{0};
  std::atomic<std::uint64_t> silence_fills_{0};

  // played_ns() is const by contract but has to remember what it last saw in
  // order to notice a step backwards.
  mutable std::atomic<UINT64> last_position_{0};
  mutable std::atomic<std::int64_t> last_reported_ns_{0};
  mutable std::atomic<std::uint64_t> discontinuities_{0};
};

}  // namespace

result<audio_sink*> create_audio_sink() {
  auto* sink = new (std::nothrow) wasapi_sink();
  if (sink == nullptr) return err(status::out_of_memory);
  return static_cast<audio_sink*>(sink);
}

void destroy_audio_sink(audio_sink* sink) noexcept { delete sink; }

}  // namespace mv::player
