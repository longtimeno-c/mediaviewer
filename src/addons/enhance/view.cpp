// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

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

// Graphics memory where the model would run on an NVIDIA card; a Mac's GPU
// shares the memory.
std::string memory_kind(provider on, const model& m, const machine& here) {
  if (on == provider::cuda || (on == provider::none && m.cuda && here.accelerator != "apple")) {
    return "graphics memory";
  }
  return "memory";
}

// "16.0 GB": bytes / 2^30 to `places` (1 or 2) decimals, rounded.
std::string gb_text(std::uint64_t bytes, int places) {
  constexpr std::uint64_t kGiB = 1ull << 30;
  const std::uint64_t scale = places == 2 ? 100 : 10;
  const std::uint64_t n = bytes / kGiB * scale + ((bytes % kGiB) * scale + kGiB / 2) / kGiB;
  std::string part = std::to_string(n % scale);
  const auto width = static_cast<std::size_t>(places);
  if (part.size() < width) part.insert(0, width - part.size(), '0');
  return std::to_string(n / scale) + "." + part + " GB";
}

// Rule 3 failed: worded by what is missing. A Mac whose chip the model has
// no Core ML path for; a PC without the NVIDIA card a CUDA-only model needs,
// or the Mac a Core ML-only one does; a PC with no card it can use, for a
// model either card runs or none does; and a card here that the model has
// passed no self-test on, too slow on the processor.
std::string needs_gpu_line(const model& m, const machine& here) {
  if (here.accelerator == "apple") return "Does not run on this Mac";
  if (m.cuda && !m.coreml) return "Needs an NVIDIA graphics card";
  if (m.coreml && !m.cuda) return "Needs a Mac with Apple silicon";
  if (here.accelerator.empty()) return "Needs a graphics card";
  return "Too slow on this " + place(here) + "'s processor";
}

std::string fit_line(const fit& f, const model& m, bool recommended, const machine& here) {
  switch (f.reason) {
    case why::fits: return recommended ? "Recommended for this " + place(here) : "Fits this " + place(here);
    case why::too_large: return "Too large to download (over 5 GB)";
    case why::no_runtime: return "Needs a Mac with Apple silicon";
    case why::needs_gpu: return needs_gpu_line(m, here);
    case why::memory: {
      // Rounded alike ("16 GB" against a 16 GB PC's 15.8): to the tenth,
      // then the hundredth, so the two never read the same.
      std::string need = memory_text(f.need);
      std::string have = memory_text(f.have);
      for (int places = 1; need == have && places <= 2; ++places) {
        need = gb_text(f.need, places);
        have = gb_text(f.have, places);
      }
      return "Needs " + need + " of " + memory_kind(f.on, m, here) + "; this " + place(here) + " has " + have;
    }
    case why::disk: return "Needs " + size_text(f.need) + " free on this disk; " + size_text(f.have) + " is free";
    case why::no_files: return "Not available in this version";
  }
  return {};
}

// Local search's line: the fetcher's wait is one snapshot, so no countdown.
constexpr const char* kWaiting = "Waiting for Hugging Face\xE2\x80\xA6 downloads resume automatically";  // "…"

bool moving(download_state s) noexcept { return s == download_state::downloading || s == download_state::waiting; }

// The line of a download under way.
std::string moving_line(const kind_status& s) {
  if (s.state == download_state::waiting) return kWaiting;
  return "Downloading " + size_text(s.done_bytes) + " of " + size_text(s.total_bytes);
}

double fraction(std::uint64_t done, std::uint64_t total) {
  return total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : -1.0;
}

// Its bar in whole percent (-1: no total): with the line, what a person
// sees move.
int percent(const kind_status& s) {
  const double v = fraction(s.done_bytes, s.total_bytes);
  return v < 0 ? -1 : static_cast<int>(v * 100);
}

// Why an unfit model's run did not start: what the store recorded at the
// run, with the page's provider for its memory's kind.
std::string unfit_line(const kind_status& s, const fit& f, const model& m, const machine& here) {
  fit recorded = f;
  if (s.reason != why::fits) {
    recorded.reason = s.reason;
    recorded.need = s.need;
    recorded.have = s.have;
  }
  return fit_line(recorded, m, false, here);
}

// The line under a chosen model: how its download stands.
std::string state_line(const kind_status& s, const fit& f, const model& m, const machine& here) {
  switch (s.state) {
    case download_state::none: return fit_line(f, m, false, here);
    case download_state::ready: return "Downloaded";
    case download_state::queued: return "Waiting to download";
    case download_state::downloading:
    case download_state::waiting: return moving_line(s);
    case download_state::stopped: return "The download stopped.";
    case download_state::gone: return "This file is no longer on Hugging Face; an update will replace it.";
    case download_state::failed: return "The download did not match its checksum.";
    case download_state::refused: return "Too large to download (over 5 GB)";
    case download_state::no_space:
      return "Needs " + size_text(s.need) + " free on this disk; " + size_text(s.have) + " is free";
    case download_state::other_window:
      // What is known: another window holds the lock. Queued here, this
      // store runs the kind once it lets go; otherwise Resume queues it.
      return s.lock_wait ? "Another MediaViewer window is downloading a model. This one carries on when it finishes."
                         : "Another MediaViewer window is downloading a model. Click Resume and this one carries on "
                           "when it finishes.";
    case download_state::unsupported: return "Downloads are not available on this computer.";
    case download_state::unfit:
      return f.ok() ? "Fits this " + place(here) + " now; Resume downloads it." : unfit_line(s, f, m, here);
  }
  return {};
}

// A card's main button: Download for a model the kind has not chosen; for
// the chosen one, by how its download stands (none: it has only Delete).
verb main_verb(const kind_status* chosen) noexcept {
  if (!chosen) return verb::download;
  switch (chosen->state) {
    case download_state::queued:
    case download_state::downloading:
    case download_state::waiting: return verb::cancel;
    case download_state::other_window: return chosen->lock_wait ? verb::cancel : verb::resume;
    case download_state::stopped:
    case download_state::gone:
    case download_state::failed:
    case download_state::refused:
    case download_state::no_space:
    case download_state::unsupported:
    case download_state::unfit: return verb::resume;
    case download_state::ready:
    case download_state::none: return verb::none;
  }
  return verb::none;
}

// The chosen model's Delete, there unless its download is under way.
bool deletable(const kind_status* chosen) noexcept {
  return chosen && chosen->state != download_state::queued && !moving(chosen->state);
}

void text(writer& w, const std::string& id, const std::string& s, const char* style) {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).end_object();
}

void button(writer& w, const std::string& id, const std::string& label, const std::string& help, const char* style,
            bool disabled = false) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(clip(label));
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

// The card's ids, from the kind and the model.
std::string node(const char* what, kind k, const std::string& id) { return std::string(what) + "." + kind_id(k) + "." + id; }

// The bytes the disk rule counts: the store's figure when the page has it.
std::uint64_t left_of(const model& m, const kind_status* chosen, const fetch_left& still_to_fetch) {
  if (still_to_fetch) return still_to_fetch(m);
  if (chosen && chosen->model == m.id && chosen->total_bytes >= chosen->done_bytes) {
    return chosen->total_bytes - chosen->done_bytes;
  }
  return m.download_bytes();
}

// A card the page draws: a model under one of its kinds.
struct card_ref {
  const kind_entry* e;
  const model* m;
};

// What one card's buttons name. A label is the control's accessible name
// (screen.h), so no two read alike: the model's name; with its kind when
// another card shows that name; with what it is, then its id, when two of
// one kind's models share a name too.
std::string who_of(const std::vector<card_ref>& cards, std::size_t at) {
  const auto named = [&cards](std::size_t i, int tier) {
    const card_ref& c = cards[i];
    std::string s = clean(c.m->name);
    if (tier >= 2) s += " (" + clean(tier == 2 ? c.m->model_name : c.m->id) + ")";
    if (tier >= 1) s += " for " + clean(c.e->name);
    return s;
  };
  for (int tier = 0; tier < 3; ++tier) {
    const std::string mine = named(at, tier);
    bool alone = true;
    for (std::size_t i = 0; i < cards.size() && alone; ++i) alone = i == at || named(i, tier) != mine;
    if (alone) return mine;
  }
  return named(at, 3);
}

void card(writer& w, const kind_entry& e, const model& m, const machine& here, const kind_status* chosen,
          bool recommended, std::uint64_t left, const std::string& who) {
  const kind k = e.k;
  const kind_status* mine = chosen && chosen->model == m.id ? chosen : nullptr;
  const fit f = offer(m, e, here, left);
  const provider on = f.on != provider::none ? f.on : provider_for(m, e, here);
  const std::string name = clean(m.name);

  stack(w, node("card", k, m.id), 8, "vertical", "card");
  text(w, node("title", k, m.id), name + kDot + clean(m.model_name), "title");
  // Where it runs here; nowhere (rule 3 failed): its state line says why.
  const char* runs = on == provider::cpu                                ? "runs on the processor"
                     : on == provider::cuda || on == provider::coreml ? "runs on the graphics card"
                                                                      : nullptr;
  text(w, node("detail", k, m.id),
       "Downloads " + size_text(m.download_bytes()) + " from Hugging Face" + kDot + "needs " + memory_text(m.run_bytes) +
           " of " + memory_kind(on, m, here) + kDot + (runs ? std::string(runs) + kDot : std::string()) + m.licence,
       "secondary");
  text(w, node("state", k, m.id), mine ? state_line(*mine, f, m, here) : fit_line(f, m, recommended, here), "caption");
  if (mine && moving(mine->state)) {
    progress(w, node("progress", k, m.id), "Downloading " + who, fraction(mine->done_bytes, mine->total_bytes));
  }

  // The help says more than the label: the kind and the size.
  const std::string what = name + " (" + clean(e.name) + ")";
  stack(w, node("buttons", k, m.id), 8, "horizontal");
  switch (main_verb(mine)) {
    case verb::download:
      button(w, node("a", k, m.id), "Download " + who, "Download " + what + ", " + size_text(m.download_bytes()),
             recommended && f.ok() ? "primary" : "plain", !f.ok());
      break;
    case verb::cancel: button(w, node("a", k, m.id), "Cancel " + who, "Stop downloading " + what, "plain"); break;
    case verb::resume: {
      // Refused by the rule when it ran: Resume once it fits now (a new
      // driver, more memory), and until then says why it cannot.
      const bool refused = mine->state == download_state::refused || mine->state == download_state::unfit;
      const bool can = !refused || f.ok();
      button(w, node("a", k, m.id), "Resume " + who,
             can ? "Resume downloading " + what
                 : mine->state == download_state::unfit ? unfit_line(*mine, f, m, here) : fit_line(f, m, false, here),
             "primary", !can);
      break;
    }
    case verb::remove:
    case verb::none: break;
  }
  if (deletable(mine)) {
    button(w, node("d", k, m.id), "Delete " + who, "Delete " + what + " from this computer", "destructive");
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

std::string machine_facts(const machine& here) {
  std::string out;
  if (here.cores) {
    const char* arch = here.arch == "arm64" ? " Arm" : here.arch == "x86_64" ? " x86-64" : "";
    out = std::to_string(here.cores) + arch + (here.cores == 1 ? " core" : " cores") + kDot;
  }
  if (here.ram_total) out += memory_text(here.ram_free) + " of memory free" + kDot;
  return out + size_text(here.free_disk) + " free on this disk";
}

std::string models_tree(const catalog& c, const machine& here, std::span<const kind_status> status,
                        const fetch_left& still_to_fetch, std::string_view notice) {
  std::vector<card_ref> cards;
  for (const kind_entry& e : c.kinds) {
    for (const std::string& id : e.models) {
      const model* m = c.find(id);
      if (m && m->serves(e.k)) cards.push_back({&e, m});
    }
  }
  writer w;
  stack(w, "models", 16, "vertical");
  text(w, "machine", machine_line(here), "body");
  text(w, "facts", machine_facts(here), "secondary");
  if (!notice.empty()) text(w, "notice", clip(clean(std::string(notice))), "body");
  std::size_t at = 0;
  for (const kind_entry& e : c.kinds) {
    const kind_status* st = status_of(status, e.k);
    const kind_status* chosen = st && !st->model.empty() ? st : nullptr;
    text(w, std::string("kind.") + kind_id(e.k), clean(e.name), "heading");
    const model* best = recommend(c, e.k, here, [chosen, &still_to_fetch](const model& m) {
      return left_of(m, chosen, still_to_fetch);
    });
    const std::size_t first = at;
    for (; at < cards.size() && cards[at].e == &e; ++at) {
      const model& m = *cards[at].m;
      card(w, e, m, here, chosen, &m == best, left_of(m, chosen, still_to_fetch), who_of(cards, at));
    }
    if (at == first) text(w, std::string("none.") + kind_id(e.k), "No model yet.", "secondary");
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
    if (a.k != b.k || a.model != b.model || a.state != b.state || a.lock_wait != b.lock_wait ||
        a.reason != b.reason || a.need != b.need || a.have != b.have) {
      return {};
    }
    if (!moving(b.state) || b.model.empty()) continue;
    // Only what a person sees: the line, or the bar by a whole percent.
    const std::string line = moving_line(b);
    if (line == moving_line(a) && percent(a) == percent(b)) continue;
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

action action_of(std::string_view id, std::span<const kind_status> shown) {
  action a;
  // "a.<kind>.<model>" or "d.<kind>.<model>".
  if (id.size() < 2 || (id[0] != 'a' && id[0] != 'd') || id[1] != '.') return a;
  const std::string_view rest = id.substr(2);
  const std::size_t dot = rest.find('.');
  if (dot == std::string_view::npos) return a;
  const auto k = kind_from(rest.substr(0, dot));
  const std::string_view model_id = rest.substr(dot + 1);
  if (!k || model_id.empty() || model_id.size() > 32 || !std::all_of(model_id.begin(), model_id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      })) {
    return a;
  }
  const kind_status* st = status_of(shown, *k);
  if (!st) return a;  // no tree drawn from it
  const kind_status* chosen = !st->model.empty() && st->model == model_id ? st : nullptr;
  const verb v = id[0] == 'd' ? (deletable(chosen) ? verb::remove : verb::none) : main_verb(chosen);
  if (v == verb::none) return a;
  a.what = v;
  a.k = *k;
  if (v == verb::download) a.model = std::string(model_id);
  return a;
}

}  // namespace mv::enhance
