// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/agent/transcript.h"

#include <algorithm>
#include <cmath>

namespace mv::agent {
namespace {

using nle::rational;

float percentile(std::vector<float> v, double p) {
  if (v.empty()) return -90.0f;
  const std::size_t k = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

}  // namespace

laid_transcript lay(const nle::item& it, rational frame, const std::vector<segment>& segments,
                    const std::vector<float>& loud, std::int64_t loud_from_ms, const transcript_options& o) {
  laid_transcript out;
  const std::int64_t used_from = nle::to_ms(it.source_in);
  const std::int64_t used_to = used_from + nle::to_ms(it.duration);
  const int hop = std::max(1, o.hop_ms);
  auto snap = [&](rational t) { return frame.num > 0 ? nle::snap(t, frame).value_or(t) : t; };
  // File ms -> timeline, exact up to the ms, then on the frame grid.
  auto to_tl = [&](std::int64_t ms) {
    const rational file = nle::from_ms(ms);
    return snap(nle::add(it.at, nle::sub(file, it.source_in).value_or(rational{})).value_or(it.at));
  };
  auto hop_of = [&](std::int64_t ms) -> std::int64_t { return (ms - loud_from_ms) / hop; };
  auto db_at = [&](std::int64_t h) -> float {
    return h >= 0 && static_cast<std::size_t>(h) < loud.size() ? loud[static_cast<std::size_t>(h)] : -120.0f;
  };

  out.loudness = !loud.empty();
  float threshold = -120.0f;
  if (out.loudness) {
    out.floor_db = percentile(loud, 0.10);
    out.speech_db = percentile(loud, 0.90);
    const float span = std::max(0.0f, out.speech_db - out.floor_db);
    threshold = std::clamp(out.floor_db + std::max(6.0f, 0.3f * span), -60.0f, -28.0f);
    out.threshold_db = threshold;
  }

  // Quiet runs in the sound (at least 250 ms): where speech stops and starts.
  struct run {
    std::int64_t a, b;  // file ms
  };
  std::vector<run> quiet;
  if (out.loudness) {
    std::int64_t r = -1;
    for (std::int64_t h = 0; h <= static_cast<std::int64_t>(loud.size()); ++h) {
      const bool q = h < static_cast<std::int64_t>(loud.size()) && db_at(h) < threshold;
      if (q && r < 0) r = h;
      if (!q && r >= 0) {
        if ((h - r) * hop >= 250) quiet.push_back({loud_from_ms + r * hop, loud_from_ms + h * hop});
        r = -1;
      }
    }
  }

  // Phrases inside the used range, in time order.
  std::vector<segment> segs;
  for (const segment& s : segments) {
    if (s.end_ms <= used_from || s.start_ms >= used_to) continue;
    segment t = s;
    while (!t.text.empty() && t.text.front() == ' ') t.text.erase(0, 1);
    while (!t.text.empty() && t.text.back() == ' ') t.text.pop_back();
    if (!t.text.empty() && t.text.front() != '[') segs.push_back(std::move(t));  // "[BLANK_AUDIO]" is no speech
  }
  std::sort(segs.begin(), segs.end(), [](const segment& x, const segment& y) { return x.start_ms < y.start_ms; });
  std::int64_t prev_end = used_from;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const segment& s = segs[i];
    std::int64_t a = std::max(s.start_ms, used_from), b = std::min(s.end_ms, used_to);
    const std::int64_t next = i + 1 < segs.size() ? segs[i + 1].start_ms : used_to;
    phrase p;
    p.text = s.text;
    if (!quiet.empty()) {
      // Whisper's edges can be off by most of a pause. The sound says where
      // speech starts: the end of the last quiet run that ends near the
      // phrase's start (and after the phrase before); and where it stops: the
      // start of the last quiet run that begins inside the phrase, before the
      // next phrase starts.
      for (const run& q : quiet) {
        if (q.b <= a + 500 && q.b > prev_end && q.b >= a - 1500) {
          a = std::max(a - 1500, q.b);
          p.refined = true;
        }
      }
      std::int64_t stop = -1;
      for (const run& q : quiet) {
        if (q.a > a + 100 && q.a < next + 500 && q.a <= b + 1500) stop = q.a;
      }
      if (stop > a) {
        b = stop;
        p.refined = true;
      }
      a = std::clamp(a, used_from, used_to);
      b = std::clamp(b, used_from, used_to);
    }
    prev_end = std::max(prev_end, b);
    if (b <= a) continue;
    p.file_start_ms = a;
    p.file_end_ms = b;
    p.at = to_tl(a);
    p.end = to_tl(b);
    if (o.words) {
      // Words share the phrase by length; each boundary moves to the
      // quietest hop within 120 ms of its estimate.
      std::vector<std::string> ws;
      std::string cur;
      for (const char c : p.text) {
        if (c == ' ' || c == '\t' || c == '\n') {
          if (!cur.empty()) ws.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) ws.push_back(cur);
      std::size_t total = 0;
      for (const std::string& w : ws) total += w.size() + 1;
      std::vector<std::int64_t> edges{a};
      std::size_t acc = 0;
      for (std::size_t k = 0; k + 1 < ws.size(); ++k) {
        acc += ws[k].size() + 1;
        std::int64_t e = a + static_cast<std::int64_t>(static_cast<double>(b - a) * static_cast<double>(acc) /
                                                       static_cast<double>(std::max<std::size_t>(total, 1)));
        if (out.loudness) {
          const std::int64_t h0 = hop_of(e), win = 120 / hop;
          std::int64_t best = h0;
          for (std::int64_t h = h0 - win; h <= h0 + win; ++h) {
            if (db_at(h) < db_at(best)) best = h;
          }
          e = loud_from_ms + best * hop;
        }
        e = std::clamp(e, edges.back(), b);
        edges.push_back(e);
      }
      edges.push_back(b);
      for (std::size_t k = 0; k < ws.size(); ++k) p.words.push_back({ws[k], to_tl(edges[k]), to_tl(edges[k + 1])});
    }
    out.phrases.push_back(std::move(p));
  }

  // Silences from the sound.
  if (out.loudness) {
    const std::int64_t h_from = std::max<std::int64_t>(0, hop_of(used_from));
    const std::int64_t h_to = std::min<std::int64_t>(static_cast<std::int64_t>(loud.size()), hop_of(used_to));
    std::int64_t run = -1;
    for (std::int64_t h = h_from; h <= h_to; ++h) {
      const bool quiet = h < h_to && db_at(h) < threshold;
      if (quiet && run < 0) run = h;
      if (!quiet && run >= 0) {
        const std::int64_t a = loud_from_ms + run * hop, b = loud_from_ms + h * hop;
        if (b - a >= o.min_silence_ms) {
          silence s;
          s.at = to_tl(a);
          s.end = to_tl(b);
          // A silence that touches the item's edge is cut whole on that side.
          const std::int64_t ka = a <= used_from ? a : a + o.keep_ms;
          const std::int64_t kb = b >= used_to ? b : b - o.keep_ms;
          s.cut_from = to_tl(ka);
          s.cut_to = to_tl(std::max(ka, kb));
          out.silences.push_back(s);
        }
        run = -1;
      }
    }
  }
  return out;
}

}  // namespace mv::agent
