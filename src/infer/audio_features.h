// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Log-mel spectrograms for the audio index (plan/17 "Audio", 2026-09-27),
// matching the Hugging Face feature extractors the ONNX exports were made
// with (transformers.audio_utils: centred reflect-padded STFT, periodic Hann,
// power 2, Slaney mel banks):
//
//   CLAP     48 kHz, n_fft 1024, hop 480, 64 mels 50 Hz - 14 kHz, 10 log10
//            (dB), 10 s repeat-padded -> [1001 x 64]
//   Whisper  16 kHz, n_fft 400, hop 160, 80 mels 0 - 8 kHz, log10 clamped to
//            max - 8, (x + 4) / 4, 30 s zero-padded -> [80 x 3000]
//
// Pure C++, no allocation per sample beyond the output; tests compare with
// the numpy reference (tools/ai-reference/make_audio_reference.py).
#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace mv::infer {

// A real-input FFT of any size whose prime factors are 2, 3, 4 or 5
// (1024 and 400 both are). Returns the first n/2 + 1 bins.
class real_fft {
 public:
  explicit real_fft(std::size_t n);
  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  void run(std::span<const double> in, std::vector<std::complex<double>>& out) const;

 private:
  void fft(std::complex<double>* data, std::size_t n, std::size_t stride_twiddle,
           std::complex<double>* scratch) const;
  std::size_t n_;
  std::vector<std::complex<double>> twiddle_;  // e^{-2 pi i k / n}
  mutable std::vector<std::complex<double>> buf_, scratch_;
};

// transformers.audio_utils.mel_filter_bank(norm="slaney", mel_scale="slaney"):
// [bins x mels], row-major.
[[nodiscard]] std::vector<double> slaney_mel_bank(std::size_t bins, std::size_t mels, double fmin,
                                                  double fmax, double sample_rate);

class clap_features {
 public:
  clap_features();
  static constexpr std::uint32_t kRate = 48000;
  static constexpr std::size_t kSamples = 480000;  // 10 s
  static constexpr std::size_t kFrames = 1001;
  static constexpr std::size_t kMels = 64;
  // Appends [1 x 1001 x 64] floats for one window (repeat-padded when short).
  void compute(std::span<const float> pcm, std::vector<float>& out) const;

 private:
  real_fft fft_;
  std::vector<double> window_;
  std::vector<double> mel_;
};

class whisper_features {
 public:
  whisper_features();
  static constexpr std::uint32_t kRate = 16000;
  static constexpr std::size_t kSamples = 480000;  // 30 s
  static constexpr std::size_t kFrames = 3000;
  static constexpr std::size_t kMels = 80;
  // [80 x 3000] floats for one window (zero-padded when short).
  void compute(std::span<const float> pcm, std::vector<float>& out) const;

 private:
  real_fft fft_;
  std::vector<double> window_;
  std::vector<double> mel_;
};

}  // namespace mv::infer
