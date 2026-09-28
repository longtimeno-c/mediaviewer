// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "infer/audio_features.h"

#include <algorithm>
#include <cmath>

namespace mv::infer {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> periodic_hann(std::size_t n) {
  std::vector<double> w(n);
  for (std::size_t k = 0; k < n; ++k) w[k] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(k) / n);
  return w;
}

double hz_to_mel(double f) {
  constexpr double min_log_hz = 1000.0, min_log_mel = 15.0;
  const double logstep = 27.0 / std::log(6.4);
  return f >= min_log_hz ? min_log_mel + std::log(f / min_log_hz) * logstep : 3.0 * f / 200.0;
}

double mel_to_hz(double m) {
  constexpr double min_log_hz = 1000.0, min_log_mel = 15.0;
  const double logstep = std::log(6.4) / 27.0;
  return m >= min_log_mel ? min_log_hz * std::exp(logstep * (m - min_log_mel)) : 200.0 * m / 3.0;
}

// Centred, reflect-padded frames -> power -> mel (transformers' spectrogram).
void mel_power(std::span<const float> x, std::size_t n_fft, std::size_t hop, std::size_t frames,
               const real_fft& fft, const std::vector<double>& window, const std::vector<double>& mel,
               std::size_t mels, std::vector<double>& out /* frames x mels */) {
  const std::size_t pad = n_fft / 2;
  const auto len = static_cast<std::ptrdiff_t>(x.size());
  const auto at = [&](std::ptrdiff_t i) -> double {
    // numpy 'reflect': no edge repeat.
    if (len == 1) return x[0];
    while (i < 0 || i >= len) i = i < 0 ? -i : 2 * (len - 1) - i;
    return x[static_cast<std::size_t>(i)];
  };
  const std::size_t bins = n_fft / 2 + 1;
  std::vector<double> frame(n_fft);
  std::vector<std::complex<double>> spec;
  std::vector<double> power(bins);
  out.assign(frames * mels, 0.0);
  for (std::size_t f = 0; f < frames; ++f) {
    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(f * hop) - static_cast<std::ptrdiff_t>(pad);
    for (std::size_t k = 0; k < n_fft; ++k) frame[k] = at(start + static_cast<std::ptrdiff_t>(k)) * window[k];
    fft.run(frame, spec);
    for (std::size_t b = 0; b < bins; ++b) power[b] = std::norm(spec[b]);
    double* row = out.data() + f * mels;
    for (std::size_t b = 0; b < bins; ++b) {
      const double p = power[b];
      if (p == 0.0) continue;
      const double* m = mel.data() + b * mels;
      for (std::size_t j = 0; j < mels; ++j) row[j] += p * m[j];
    }
  }
}

}  // namespace

real_fft::real_fft(std::size_t n) : n_(n), twiddle_(n), buf_(n), scratch_(n) {
  for (std::size_t k = 0; k < n; ++k) {
    twiddle_[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n));
  }
}

// Recursive mixed-radix decimation in time. `stride_twiddle` = n_ / n.
void real_fft::fft(std::complex<double>* a, std::size_t n, std::size_t tw, std::complex<double>* s) const {
  if (n == 1) return;
  std::size_t r = 0;
  for (std::size_t p : {4, 2, 3, 5}) {
    if (n % p == 0) {
      r = p;
      break;
    }
  }
  if (r == 0) {  // not expected for 1024 / 400: a direct DFT keeps it correct
    for (std::size_t k = 0; k < n; ++k) {
      std::complex<double> acc = 0;
      for (std::size_t j = 0; j < n; ++j) acc += a[j] * twiddle_[(j * k * tw) % n_];
      s[k] = acc;
    }
    std::copy(s, s + n, a);
    return;
  }
  const std::size_t m = n / r;
  // Split into r interleaved subsequences, transform each.
  for (std::size_t q = 0; q < r; ++q) {
    for (std::size_t i = 0; i < m; ++i) s[q * m + i] = a[i * r + q];
  }
  for (std::size_t q = 0; q < r; ++q) fft(s + q * m, m, tw * r, a);
  // Combine: X[k + m t] = sum_q W_n^{q (k + m t)} Y_q[k].
  for (std::size_t k = 0; k < m; ++k) {
    for (std::size_t t = 0; t < r; ++t) {
      const std::size_t kk = k + m * t;
      std::complex<double> acc = 0;
      for (std::size_t q = 0; q < r; ++q) acc += s[q * m + k] * twiddle_[((q * kk) % n) * tw];
      a[kk] = acc;
    }
  }
}

void real_fft::run(std::span<const double> in, std::vector<std::complex<double>>& out) const {
  for (std::size_t i = 0; i < n_; ++i) buf_[i] = i < in.size() ? in[i] : 0.0;
  fft(buf_.data(), n_, 1, scratch_.data());
  out.assign(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n_ / 2 + 1));
}

std::vector<double> slaney_mel_bank(std::size_t bins, std::size_t mels, double fmin, double fmax,
                                    double sample_rate) {
  const double mmin = hz_to_mel(fmin), mmax = hz_to_mel(fmax);
  std::vector<double> ff(mels + 2);
  for (std::size_t i = 0; i < mels + 2; ++i) {
    ff[i] = mel_to_hz(mmin + (mmax - mmin) * static_cast<double>(i) / static_cast<double>(mels + 1));
  }
  // fft frequencies: linspace(0, sample_rate // 2, bins) (integer division, as numpy's caller).
  const double nyquist = std::floor(sample_rate / 2.0);
  std::vector<double> bank(bins * mels, 0.0);
  for (std::size_t b = 0; b < bins; ++b) {
    const double f = bins > 1 ? nyquist * static_cast<double>(b) / static_cast<double>(bins - 1) : 0.0;
    for (std::size_t j = 0; j < mels; ++j) {
      const double down = (f - ff[j]) / (ff[j + 1] - ff[j]);  // -slopes[:, :-2] / diff[:-1]
      const double up = (ff[j + 2] - f) / (ff[j + 2] - ff[j + 1]);
      const double v = std::max(0.0, std::min(down, up));
      bank[b * mels + j] = v * 2.0 / (ff[j + 2] - ff[j]);  // slaney area norm
    }
  }
  return bank;
}

clap_features::clap_features()
    : fft_(1024), window_(periodic_hann(1024)), mel_(slaney_mel_bank(513, kMels, 50.0, 14000.0, kRate)) {}

void clap_features::compute(std::span<const float> pcm, std::vector<float>& out) const {
  std::vector<float> x(kSamples, 0.0f);
  if (pcm.size() >= kSamples) {
    std::copy_n(pcm.begin(), kSamples, x.begin());
  } else if (!pcm.empty()) {
    // "repeatpad": whole repeats, then zeros.
    const std::size_t reps = kSamples / pcm.size();
    for (std::size_t r = 0; r < reps; ++r) std::copy(pcm.begin(), pcm.end(), x.begin() + static_cast<std::ptrdiff_t>(r * pcm.size()));
  }
  std::vector<double> m;
  mel_power(x, 1024, 480, kFrames, fft_, window_, mel_, kMels, m);
  const std::size_t base = out.size();
  out.resize(base + kFrames * kMels);
  for (std::size_t i = 0; i < m.size(); ++i) {
    out[base + i] = static_cast<float>(10.0 * std::log10(std::max(1e-10, m[i])));
  }
}

whisper_features::whisper_features()
    : fft_(400), window_(periodic_hann(400)), mel_(slaney_mel_bank(201, kMels, 0.0, 8000.0, kRate)) {}

void whisper_features::compute(std::span<const float> pcm, std::vector<float>& out) const {
  std::vector<float> x(kSamples, 0.0f);
  std::copy_n(pcm.begin(), std::min(pcm.size(), kSamples), x.begin());
  std::vector<double> m;
  mel_power(x, 400, 160, kFrames + 1, fft_, window_, mel_, kMels, m);  // the last frame is dropped
  std::vector<double> lg(kFrames * kMels);
  double peak = -1e30;
  for (std::size_t f = 0; f < kFrames; ++f) {
    for (std::size_t j = 0; j < kMels; ++j) {
      const double v = std::log10(std::max(1e-10, m[f * kMels + j]));
      lg[f * kMels + j] = v;
      peak = std::max(peak, v);
    }
  }
  out.assign(kMels * kFrames, 0.0f);
  for (std::size_t f = 0; f < kFrames; ++f) {
    for (std::size_t j = 0; j < kMels; ++j) {
      const double v = std::max(lg[f * kMels + j], peak - 8.0);
      out[j * kFrames + f] = static_cast<float>((v + 4.0) / 4.0);  // [mels x frames]
    }
  }
}

}  // namespace mv::infer
