// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "addons/enhance/choose.h"
#include "core/json.h"

namespace mv::enhance {
namespace {

using json::writer;

constexpr const char* kDot = " \xC2\xB7 ";  // " · "

constexpr const char* kFootnote =
    "Models download from Hugging Face only when you click Download. Hugging Face sees this computer's network "
    "address and which public model files it sends; nothing about your photos, video or audio is sent.";

// .NET's Math.Round(value, digits): scale, round half to even, scale back,
// in doubles, as Math.Round does them.
double net_round(double v, int digits) {
  const double p = digits == 2 ? 100.0 : digits == 1 ? 10.0 : 1.0;
  return std::nearbyint(v * p) / p;
}

// .NET's double.ToString("0.00" / "0.0" / "0"): the 15 most significant
// digits first, then rounded half away from zero to the format's places.
std::string net_fixed(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.14e", v);
  std::string digits;
  int exp10 = 0;
  for (const char* p = buf; *p; ++p) {
    if (*p == 'e' || *p == 'E') {
      exp10 = std::atoi(p + 1);
      break;
    }
    if (*p >= '0' && *p <= '9') digits.push_back(*p);  // any locale's point is skipped
  }
  if (digits.empty()) return "0";
  int point = exp10 + 1;  // digits before the decimal point
  const int keep = point + decimals;
  std::string kept;
  if (keep < 0) {
    point = 0;
  } else if (keep == 0) {
    if (digits[0] >= '5') {
      kept = "1";
      ++point;
    }
  } else if (keep >= static_cast<int>(digits.size())) {
    kept = digits + std::string(static_cast<std::size_t>(keep) - digits.size(), '0');
  } else {
    kept = digits.substr(0, static_cast<std::size_t>(keep));
    if (digits[static_cast<std::size_t>(keep)] >= '5') {
      int i = keep - 1;
      while (i >= 0 && kept[static_cast<std::size_t>(i)] == '9') kept[static_cast<std::size_t>(i--)] = '0';
      if (i >= 0) {
        ++kept[static_cast<std::size_t>(i)];
      } else {
        kept.insert(kept.begin(), '1');
        ++point;
      }
    }
  }
  // Leading zeros for a value under 1, then the places the format shows.
  std::string full = (point < 0 ? std::string(static_cast<std::size_t>(-point), '0') : std::string()) + kept;
  const auto whole = static_cast<std::size_t>(std::max(point, 0));
  if (full.size() < whole + static_cast<std::size_t>(decimals)) {
    full.append(whole + static_cast<std::size_t>(decimals) - full.size(), '0');
  }
  std::string out = full.substr(0, whole);
  const std::size_t lead = out.find_first_not_of('0');
  out = lead == std::string::npos ? "0" : out.substr(lead);
  if (decimals > 0) out += "." + full.substr(whole, static_cast<std::size_t>(decimals));
  return out;
}

// Text from the catalogue or the OS, as the vocabulary takes it: no control
// characters (a text node allows none but a newline or tab).
std::string clean(std::string s) {
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) c = ' ';
  }
  return s;
}

// A label or help line: at most 200 bytes (screen.h kMaxLabel), cut between
// two UTF-8 characters.
std::string clip(std::string s) {
  constexpr std::size_t kMax = 200;
  if (s.size() <= kMax) return s;
  std::size_t cut = kMax;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  s.resize(cut);
  return s;
}

const kind_status* status_of(std::span<const kind_status> status, kind k) {
  for (const kind_status& s : status) {
    if (s.k == k) return &s;
  }
  return nullptr;
}

std::string place(const machine& here) { return here.os == "macos" ? "Mac" : "PC"; }

double fraction(std::uint64_t done, std::uint64_t total) {
  return total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : -1.0;
}

std::string memory_kind(provider on, const model& m) {
  if (on == provider::cuda || (on == provider::none && m.cuda)) return "graphics memory";
  return "memory";
}

std::string fit_line(const fit& f, const model& m, bool recommended, const machine& here) {
  switch (f.reason) {
    case why::fits: return recommended ? "Recommended for this " + place(here) : "Fits this " + place(here);
    case why::too_large: return "Too large to download (over 5 GB)";
    case why::no_runtime: return "Needs a Mac with Apple silicon";
    case why::needs_gpu: return "Needs a graphics card";
    case why::memory:
      return "Needs " + memory_text(f.need) + " of " + memory_kind(f.on, m) + "; this " + place(here) + " has " +
             memory_text(f.have);
    case why::disk: return "Needs " + size_text(f.need) + " free on this disk; " + size_text(f.have) + " is free";
    case why::no_files: return "Not available in this version";
  }
  return {};
}

std::string waiting_line(double retry_in_s) {
  std::string out = "Waiting for Hugging Face\xE2\x80\xA6";  // "…"
  if (retry_in_s > 0) out += " trying again in " + std::to_string(static_cast<long long>(std::ceil(retry_in_s))) + " s";
  return out;
}

// The line under a chosen model: how its download stands.
std::string state_line(const kind_status& s, const fit& f, const model& m, const machine& here) {
  switch (s.state) {
    case download_state::none: return fit_line(f, m, false, here);
    case download_state::ready: return "Downloaded";
    case download_state::queued: return "Waiting to download";
    case download_state::downloading:
      return "Downloading " + size_text(s.done_bytes) + " of " + size_text(s.total_bytes);
    case download_state::waiting: return waiting_line(s.retry_in_s);
    case download_state::stopped: return "The download stopped.";
    case download_state::gone: return "This file is no longer on Hugging Face; an update will replace it.";
    case download_state::failed: return "The download did not match its checksum.";
    case download_state::refused: return "Too large to download (over 5 GB)";
    case download_state::no_space:
      return "Needs " + size_text(s.need) + " free on this disk; " + size_text(s.have) + " is free";
    case download_state::other_window: return "Downloading in another MediaViewer window.";
    case download_state::unsupported: return "Downloads are not available on this computer.";
    case download_state::unfit: return f.ok() ? "The download stopped." : fit_line(f, m, false, here);
  }
  return {};
}

bool moving(download_state s) noexcept { return s == download_state::downloading || s == download_state::waiting; }

void text(writer& w, const std::string& id, const std::string& s, const char* style) {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).end_object();
}

void button(writer& w, const std::string& id, const char* label, const std::string& help, const char* style,
            bool disabled = false) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  w.key("help").string(clip(help)).key("style").string(style);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}

void stack(writer& w, const std::string& id, int spacing, const char* axis, const char* look = nullptr) {
  w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string(axis);
  w.key("spacing").integer(spacing);
  if (look) w.key("look").string(look);
  w.key("children").begin_array();
}

void end(writer& w) { w.end_array().end_object(); }

void progress(writer& w, const std::string& id, const std::string& label, double value) {
  w.begin_object().key("type").string("progress").key("id").string(id).key("label").string(clip(label));
  w.key("value").number(value).end_object();
}

// The card's text ids, from the kind and the model.
std::string node(const char* what, kind k, const std::string& id) { return std::string(what) + "." + kind_id(k) + "." + id; }

void card(writer& w, const kind_entry& e, const model& m, const machine& here, const kind_status* chosen,
          bool recommended) {
  const kind k = e.k;
  const bool is_chosen = chosen && chosen->model == m.id;
  const std::uint64_t left = is_chosen && chosen->total_bytes >= chosen->done_bytes
                                 ? chosen->total_bytes - chosen->done_bytes
                                 : m.download_bytes();
  const fit f = offer(m, e, here, left);
  const provider on = f.on != provider::none ? f.on : provider_for(m, e, here);
  const std::string name = clean(m.name);

  stack(w, node("card", k, m.id), 8, "vertical", "card");
  text(w, node("title", k, m.id), name + kDot + clean(m.model_name), "title");
  const bool gpu = on == provider::cuda || on == provider::coreml || (on == provider::none && (m.cuda || m.coreml));
  text(w, node("detail", k, m.id),
       "Downloads " + size_text(m.download_bytes()) + " from Hugging Face" + kDot + "needs " + memory_text(m.run_bytes) +
           " of " + memory_kind(on, m) + kDot + (gpu ? "runs on the graphics card" : "runs on the processor") + kDot +
           m.licence,
       "secondary");
  text(w, node("state", k, m.id), is_chosen ? state_line(*chosen, f, m, here) : fit_line(f, m, recommended, here),
       "caption");
  if (is_chosen && moving(chosen->state)) {
    progress(w, node("progress", k, m.id), "Downloading " + name, fraction(chosen->done_bytes, chosen->total_bytes));
  }

  const std::string what = name + " (" + clean(e.name) + ")";
  stack(w, node("buttons", k, m.id), 8, "horizontal");
  if (!is_chosen) {
    button(w, std::string("dl.") + kind_id(k) + "." + m.id, "Download",
           "Download " + what + ", " + size_text(m.download_bytes()), recommended && f.ok() ? "primary" : "plain",
           !f.ok());
  } else {
    const std::string kid = kind_id(k);
    switch (chosen->state) {
      case download_state::queued:
      case download_state::downloading:
      case download_state::waiting:
        button(w, "cancel." + kid, "Cancel", "Stop downloading " + what, "plain");
        break;
      case download_state::stopped:
      case download_state::gone:
      case download_state::failed:
      case download_state::no_space:
      case download_state::other_window:
      case download_state::unsupported:
        button(w, "resume." + kid, "Resume", "Resume downloading " + what, "primary");
        button(w, "delete." + kid, "Delete", "Delete " + what + " from this computer", "destructive");
        break;
      case download_state::ready:
      case download_state::refused:
      case download_state::unfit:
      case download_state::none:
        button(w, "delete." + kid, "Delete", "Delete " + what + " from this computer", "destructive");
        break;
    }
  }
  end(w);  // buttons
  end(w);  // card
}

}  // namespace

std::string size_text(std::uint64_t bytes_in) {
  static constexpr const char* kUnits[] = {"KB", "MB", "GB", "TB"};
  // ByteSize takes a long: anything past it is the largest long.
  const auto bytes = static_cast<std::int64_t>(
      std::min<std::uint64_t>(bytes_in, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
  if (bytes < 1000) return std::to_string(bytes) + " bytes";
  double v = static_cast<double>(bytes) / 1000.0;
  int u = 0;
  // Round first, so 999,999 bytes reads "1.00 MB", not "1000 KB".
  while (u < 3 && net_round(v, 0) >= 1000) {
    v /= 1000;
    ++u;
  }
  const int places = net_round(v, 2) < 10 ? 2 : net_round(v, 1) < 100 ? 1 : 0;
  return net_fixed(v, places) + " " + kUnits[u];
}

std::string memory_text(std::uint64_t bytes) {
  constexpr std::uint64_t kMiB = 1ull << 20;
  const std::uint64_t mib = bytes / kMiB;
  const std::uint64_t tenths = (mib * 10 + 512) / 1024;  // of a GiB, rounded
  if (tenths >= 100) return std::to_string((mib + 512) / 1024) + " GB";
  if (tenths >= 10) {
    return std::to_string(tenths / 10) + (tenths % 10 ? "." + std::to_string(tenths % 10) : std::string()) + " GB";
  }
  return std::to_string(std::max<std::uint64_t>((bytes + kMiB / 2) / kMiB, 1)) + " MB";
}

std::string machine_line(const machine& here) {
  const std::string memory = here.ram_total ? memory_text(here.ram_total) + " of memory" : "memory not known";
  const std::string gpu = clean(here.gpu);
  if (here.accelerator == "apple") {
    return "This Mac: " + (gpu.empty() ? memory : gpu + ", " + memory) + ".";
  }
  if (here.accelerator == "nvidia") {
    std::string card = gpu.empty() ? std::string("an NVIDIA graphics card") : gpu;
    if (here.vram) card += " with " + memory_text(here.vram);
    return "This " + place(here) + ": " + memory + ", " + card + ".";
  }
  if (here.os == "macos" && here.arch == "x86_64") return "This Mac: " + memory + " and an Intel processor.";
  return "This " + place(here) + ": " + memory + " and no supported graphics card.";
}

std::string models_tree(const catalog& c, const machine& here, std::span<const kind_status> status) {
  writer w;
  stack(w, "models", 16, "vertical");
  text(w, "machine", machine_line(here), "body");
  for (const kind_entry& e : c.kinds) {
    const kind_status* st = status_of(status, e.k);
    const kind_status* chosen = st && !st->model.empty() ? st : nullptr;
    text(w, std::string("kind.") + kind_id(e.k), clean(e.name), "heading");
    const model* best = recommend(c, e.k, here, [chosen](const model& m) {
      if (chosen && chosen->model == m.id && chosen->total_bytes >= chosen->done_bytes) {
        return chosen->total_bytes - chosen->done_bytes;
      }
      return m.download_bytes();
    });
    std::size_t shown = 0;
    for (const std::string& id : e.models) {
      const model* m = c.find(id);
      if (!m || !m->serves(e.k)) continue;
      card(w, e, *m, here, chosen, m == best);
      ++shown;
    }
    if (shown == 0) text(w, std::string("none.") + kind_id(e.k), "No model yet.", "secondary");
  }
  text(w, "footnote", kFootnote, "caption");
  end(w);
  return w.take();
}

std::string progress_diff(std::span<const kind_status> before, std::span<const kind_status> now) {
  if (before.size() != now.size()) return {};
  writer w;
  w.begin_array();
  for (std::size_t i = 0; i < now.size(); ++i) {
    const kind_status& a = before[i];
    const kind_status& b = now[i];
    if (a.k != b.k || a.model != b.model || a.state != b.state) return {};
    if (!moving(b.state) || b.model.empty()) continue;
    if (a.done_bytes == b.done_bytes && a.total_bytes == b.total_bytes && a.retry_in_s == b.retry_in_s) continue;
    const std::string line = b.state == download_state::waiting
                                 ? waiting_line(b.retry_in_s)
                                 : "Downloading " + size_text(b.done_bytes) + " of " + size_text(b.total_bytes);
    w.begin_object().key("op").string("set").key("id").string(node("progress", b.k, b.model));
    w.key("props").begin_object().key("value").number(fraction(b.done_bytes, b.total_bytes)).end_object();
    w.end_object();
    w.begin_object().key("op").string("set").key("id").string(node("state", b.k, b.model));
    w.key("props").begin_object().key("text").string(line).end_object();
    w.end_object();
  }
  w.end_array();
  return w.take();
}

action action_of(std::string_view id) {
  action a;
  const std::size_t dot = id.find('.');
  if (dot == std::string_view::npos) return a;
  const std::string_view head = id.substr(0, dot);
  std::string_view rest = id.substr(dot + 1);
  std::string_view model_id;
  if (head == "dl") {
    const std::size_t second = rest.find('.');
    if (second == std::string_view::npos) return a;
    model_id = rest.substr(second + 1);
    rest = rest.substr(0, second);
    if (model_id.empty() || model_id.size() > 32 || !std::all_of(model_id.begin(), model_id.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })) {
      return a;
    }
  }
  const auto k = kind_from(rest);
  if (!k) return a;
  if (head == "dl") {
    a.what = verb::download;
    a.model = std::string(model_id);
  } else if (head == "cancel") {
    a.what = verb::cancel;
  } else if (head == "resume") {
    a.what = verb::resume;
  } else if (head == "delete") {
    a.what = verb::remove;
  } else {
    return a;
  }
  a.k = *k;
  return a;
}

}  // namespace mv::enhance
