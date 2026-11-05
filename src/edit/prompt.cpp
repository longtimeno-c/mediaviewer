// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/prompt.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace mv::edit {
namespace {

using P = adjust_param;

// One step of a slider for a word (the size "a bit" halves, "much" doubles).
struct move {
  P param;
  float amount;
};

// A word that moves sliders. `region` words (shadows, sky, edges) take the
// direction of an adjective or verb just before them instead of their own.
struct word {
  std::string_view text;
  std::array<move, 4> moves;
  int count;
  bool region = false;
  bool set = false;  // black & white: set, not add
};

constexpr word kWords[] = {
    // Light.
    {"brighter", {{{P::exposure, 0.3f}}}, 1},   {"lighter", {{{P::exposure, 0.3f}}}, 1},
    {"brighten", {{{P::exposure, 0.3f}}}, 1},   {"lighten", {{{P::exposure, 0.3f}}}, 1},
    {"bright", {{{P::exposure, 0.3f}}}, 1},     {"light", {{{P::exposure, 0.3f}}}, 1},
    {"brightness", {{{P::exposure, 0.3f}}}, 1}, {"exposure", {{{P::exposure, 0.3f}}}, 1},
    {"darker", {{{P::exposure, -0.3f}}}, 1},    {"darken", {{{P::exposure, -0.3f}}}, 1},
    {"dark", {{{P::exposure, -0.3f}}}, 1},      {"dimmer", {{{P::exposure, -0.3f}}}, 1},
    {"dim", {{{P::exposure, -0.3f}}}, 1},       {"underexposed", {{{P::exposure, -0.3f}}}, 1},
    {"overexposed", {{{P::exposure, 0.3f}}}, 1},
    {"contrast", {{{P::contrast, 20.0f}}}, 1},  {"contrasty", {{{P::contrast, 20.0f}}}, 1},
    {"flat", {{{P::contrast, -20.0f}}}, 1},     {"flatter", {{{P::contrast, -20.0f}}}, 1},
    // Regions: the adjective before them says which way.
    {"shadows", {{{P::shadows, 30.0f}}}, 1, true},   {"shadow", {{{P::shadows, 30.0f}}}, 1, true},
    {"highlights", {{{P::highlights, 30.0f}}}, 1, true}, {"highlight", {{{P::highlights, 30.0f}}}, 1, true},
    {"sky", {{{P::highlights, 30.0f}}}, 1, true},
    {"skies", {{{P::highlights, 30.0f}}}, 1, true},  {"clouds", {{{P::highlights, 30.0f}}}, 1, true},
    {"whites", {{{P::whites, 20.0f}}}, 1, true},     {"blacks", {{{P::blacks, 20.0f}}}, 1, true},
    {"edges", {{{P::vignette, -30.0f}}}, 1, true},   {"corners", {{{P::vignette, -30.0f}}}, 1, true},
    {"blown", {{{P::highlights, -30.0f}}}, 1},
    // Colour.
    {"saturated", {{{P::saturation, 20.0f}}}, 1}, {"saturation", {{{P::saturation, 20.0f}}}, 1},
    {"saturate", {{{P::saturation, 20.0f}}}, 1},  {"desaturate", {{{P::saturation, -20.0f}}}, 1},
    {"vibrancy", {{{P::vibrance, 25.0f}}}, 1},    {"temperature", {{{P::temperature, 15.0f}}}, 1},
    {"colourful", {{{P::vibrance, 25.0f}}}, 1},   {"colorful", {{{P::vibrance, 25.0f}}}, 1},
    {"colour", {{{P::vibrance, 25.0f}}}, 1},      {"color", {{{P::vibrance, 25.0f}}}, 1},
    {"vivid", {{{P::vibrance, 25.0f}}}, 1},       {"vibrant", {{{P::vibrance, 25.0f}}}, 1},
    {"vibrance", {{{P::vibrance, 25.0f}}}, 1},    {"dull", {{{P::saturation, -20.0f}}}, 1},
    {"muted", {{{P::saturation, -20.0f}}}, 1},    {"desaturated", {{{P::saturation, -20.0f}}}, 1},
    {"warmer", {{{P::temperature, 15.0f}}}, 1},   {"warm", {{{P::temperature, 15.0f}}}, 1},
    {"warmth", {{{P::temperature, 15.0f}}}, 1},   {"golden", {{{P::temperature, 15.0f}}}, 1},
    {"yellow", {{{P::temperature, 15.0f}}}, 1},   {"orange", {{{P::temperature, 15.0f}}}, 1},
    {"cooler", {{{P::temperature, -15.0f}}}, 1},  {"cool", {{{P::temperature, -15.0f}}}, 1},
    {"colder", {{{P::temperature, -15.0f}}}, 1},  {"cold", {{{P::temperature, -15.0f}}}, 1},
    {"blue", {{{P::temperature, -15.0f}}}, 1},    {"bluer", {{{P::temperature, -15.0f}}}, 1},
    {"green", {{{P::tint, -12.0f}}}, 1},          {"greener", {{{P::tint, -12.0f}}}, 1},
    {"magenta", {{{P::tint, 12.0f}}}, 1},         {"pink", {{{P::tint, 12.0f}}}, 1},
    // Black & white.
    {"monochrome", {{{P::mono, 100.0f}}}, 1, false, true}, {"mono", {{{P::mono, 100.0f}}}, 1, false, true},
    {"greyscale", {{{P::mono, 100.0f}}}, 1, false, true},  {"grayscale", {{{P::mono, 100.0f}}}, 1, false, true},
    {"noir", {{{P::mono, 100.0f}, {P::contrast, 25.0f}}}, 2, false, true},
    // Detail.
    {"sharper", {{{P::sharpen, 30.0f}}}, 1},     {"sharpen", {{{P::sharpen, 30.0f}}}, 1},
    {"sharp", {{{P::sharpen, 30.0f}}}, 1},       {"crisp", {{{P::sharpen, 30.0f}}}, 1},
    {"sharpness", {{{P::sharpen, 30.0f}}}, 1},   {"sharpening", {{{P::sharpen, 30.0f}}}, 1},
    {"crisper", {{{P::sharpen, 30.0f}}}, 1},     {"blurry", {{{P::sharpen, -30.0f}}}, 1},
    {"noise", {{{P::noise, -30.0f}}}, 1},        {"noisy", {{{P::noise, -30.0f}}}, 1},
    {"grain", {{{P::noise, -30.0f}}}, 1},        {"grainy", {{{P::noise, -30.0f}}}, 1},
    {"denoise", {{{P::noise, 30.0f}}}, 1},       {"clean", {{{P::noise, 30.0f}}}, 1},
    {"cleaner", {{{P::noise, 30.0f}}}, 1},       {"smoother", {{{P::noise, 30.0f}}}, 1},
    {"smooth", {{{P::noise, 30.0f}}}, 1},        {"smoothen", {{{P::noise, 30.0f}}}, 1},
    {"denoising", {{{P::noise, 30.0f}}}, 1},
    {"detail", {{{P::definition, 25.0f}}}, 1},   {"details", {{{P::definition, 25.0f}}}, 1},
    {"definition", {{{P::definition, 25.0f}}}, 1}, {"clarity", {{{P::definition, 25.0f}}}, 1},
    {"texture", {{{P::definition, 25.0f}}}, 1},  {"brilliance", {{{P::brilliance, 25.0f}}}, 1},
    {"vignette", {{{P::vignette, 30.0f}}}, 1},   {"vignetting", {{{P::vignette, 30.0f}}}, 1},
    // Looks: several sliders at once.
    {"punchy", {{{P::contrast, 15.0f}, {P::vibrance, 25.0f}}}, 2},
    {"pop", {{{P::contrast, 15.0f}, {P::vibrance, 25.0f}}}, 2},
    {"punch", {{{P::contrast, 15.0f}, {P::vibrance, 25.0f}}}, 2},
    {"dramatic", {{{P::contrast, 25.0f}, {P::highlights, -20.0f}, {P::shadows, -15.0f}, {P::vignette, 25.0f}}}, 4},
    {"moody", {{{P::contrast, 20.0f}, {P::exposure, -0.2f}, {P::saturation, -15.0f}, {P::vignette, 25.0f}}}, 4},
    {"film", {{{P::contrast, -10.0f}, {P::blacks, 20.0f}, {P::saturation, -15.0f}, {P::temperature, 10.0f}}}, 4},
    {"vintage", {{{P::contrast, -10.0f}, {P::blacks, 20.0f}, {P::saturation, -15.0f}, {P::temperature, 10.0f}}}, 4},
    {"retro", {{{P::contrast, -10.0f}, {P::blacks, 20.0f}, {P::saturation, -15.0f}, {P::temperature, 10.0f}}}, 4},
    {"matte", {{{P::blacks, 30.0f}, {P::contrast, -10.0f}}}, 2},
    {"faded", {{{P::blacks, 30.0f}, {P::contrast, -10.0f}}}, 2},
    {"soft", {{{P::definition, -25.0f}, {P::contrast, -10.0f}}}, 2},
    {"softer", {{{P::definition, -25.0f}, {P::contrast, -10.0f}}}, 2},
    {"dreamy", {{{P::definition, -25.0f}, {P::contrast, -10.0f}, {P::brilliance, 15.0f}}}, 3},
};

// Verbs and the like that turn the next word around, or keep its way.
// "deeper blacks", "crush the shadows": down, as "less" is.
constexpr std::string_view kFlip[] = {"less", "reduce", "lower", "decrease", "remove", "fewer", "too",
                                      "tame", "recover", "not", "without", "lose", "deeper", "crush", "deepen"};
constexpr std::string_view kKeep[] = {"more", "increase", "boost", "add", "raise", "lift", "open", "extra",
                                      "higher", "stronger", "enhance", "improve", "turn", "bump", "crank",
                                      "pump", "push", "amp", "up"};
// A slider named as a thing ("exposure", "noise"): "down" right after it
// turns it the other way ("exposure down", "noise down"). After an adjective
// it is only an echo ("cool it down" is cooler, not warmer); before a mover
// it says which way ("turn down the contrast", "bring down the highlights").
constexpr std::string_view kNouns[] = {
    "exposure",   "brightness", "contrast",   "saturation", "vibrance",   "vibrancy",   "warmth",
    "temperature", "noise",     "grain",      "sharpness",  "clarity",    "definition", "detail",
    "details",    "texture",    "vignette",   "vignetting", "highlights", "highlight",  "shadows",
    "shadow",     "whites",     "blacks",     "brilliance"};
constexpr std::string_view kSmall[] = {"bit", "slightly", "little", "touch", "tad", "subtle", "subtly"};
constexpr std::string_view kBig[] = {"much", "lot", "lots", "very", "way", "really", "far", "loads"};
constexpr std::string_view kSkip[] = {
    "a", "an", "the", "and", "make", "it", "its", "please", "photo", "picture", "image", "this", "that",
    "me", "with", "look", "looks", "like", "some", "but", "can", "you", "i", "want", "should", "be", "so",
    "is", "of", "to", "in", "up", "down", "bring", "back", "give", "feel", "slight", "just", "my", "are",
    "out", "for", "on", "at", "or", "abit", "id", "would", "could", "get", "rid", "white", "black", "balance",
    "fix", "auto", "correct", "colours", "colors", "tone", "tones", "old", "overall", "whole", "now",
    // The tails of "it's", "photo's": the tokenizer splits at the apostrophe.
    "s", "t", "level", "amount"};

template <std::size_t N>
bool in(const std::string_view (&set)[N], std::string_view w) {
  return std::find(std::begin(set), std::end(set), w) != std::end(set);
}

const word* find(std::string_view w) {
  for (const word& k : kWords) {
    if (k.text == w) return &k;
  }
  return nullptr;
}

std::vector<std::string> tokens(std::string_view text) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c == '&') {
      cur.push_back(static_cast<char>(std::tolower(c)));
    } else if (!cur.empty()) {
      out.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

std::string format_move(P p, float delta) {
  char buf[64];
  if (p == P::exposure) {
    std::snprintf(buf, sizeof buf, "%s %+.1f EV", range_of(p).name, static_cast<double>(delta));
  } else if (p == P::mono) {
    std::snprintf(buf, sizeof buf, "Black & white %s", delta > 0 ? "on" : "off");
  } else {
    std::snprintf(buf, sizeof buf, "%s %+.0f", range_of(p).name, static_cast<double>(delta));
  }
  return buf;
}

}  // namespace

prompt_result parse_prompt(std::string_view text, const colour& current) {
  prompt_result r;
  r.value = current;
  const std::vector<std::string> t = tokens(text);
  float sign = 1.0f, scale = 1.0f;
  bool keep_next = false;  // "keep the sky": the next mover is left alone
  std::array<float, kAdjustParamCount> moved{};
  std::array<bool, kAdjustParamCount> touched{};
  auto apply = [&](P p, float amount, bool set) {
    const auto i = static_cast<std::size_t>(p);
    const float before = r.value.v[i];
    float after = set ? clamp_param(p, amount) : clamp_param(p, before + amount);
    // On the slider's own steps, so the pane shows what the note says.
    const float step = range_of(p).step;
    if (step > 0.0f) after = clamp_param(p, std::round(after / step) * step);
    r.value.v[i] = after;
    moved[i] += after - before;
    touched[i] = true;
  };
  // The last mover, to turn it round on a "down" right after it.
  struct last_mover {
    const word* w = nullptr;
    std::size_t at = 0;
    float factor = 0.0f;
    colour before;
    std::array<float, kAdjustParamCount> moved{};
    std::array<bool, kAdjustParamCount> touched{};
  } last;
  auto apply_word = [&](const word& k, std::size_t at, float factor) {
    last.w = &k;
    last.at = at;
    last.factor = factor;
    last.before = r.value;
    last.moved = moved;
    last.touched = touched;
    for (int m = 0; m < k.count; ++m) {
      const move& mv = k.moves[static_cast<std::size_t>(m)];
      if (k.set && m == 0) {
        apply(mv.param, factor < 0 ? 0.0f : mv.amount, true);
      } else {
        apply(mv.param, mv.amount * factor, false);
      }
    }
  };
  auto want_auto = [&](adjust_section s) {
    if (std::find(r.autos.begin(), r.autos.end(), s) == r.autos.end()) r.autos.push_back(s);
  };

  for (std::size_t i = 0; i < t.size(); ++i) {
    const std::string& w = t[i];
    const std::string_view next = i + 1 < t.size() ? std::string_view(t[i + 1]) : std::string_view();
    const std::string_view after = i + 2 < t.size() ? std::string_view(t[i + 2]) : std::string_view();
    // Phrases first.
    if ((w == "black" && (next == "and" || next == "&") && after == "white") || w == "b&w" || w == "bw") {
      apply(P::mono, sign < 0 ? 0.0f : 100.0f, true);
      i += w == "black" ? 2 : 0;
      sign = 1.0f;
      scale = 1.0f;
      continue;
    }
    if ((w == "white" && next == "balance") || ((w == "fix" || w == "auto" || w == "correct") &&
                                                (next == "colour" || next == "color" || next == "colours" ||
                                                 next == "colors" || next == "white"))) {
      want_auto(adjust_section::colour);
      continue;
    }
    if ((w == "auto" || w == "fix" || w == "correct") &&
        (next == "light" || next == "exposure" || next == "levels" || next == "tone" || next == "it" ||
         next.empty())) {
      want_auto(next == "levels" ? adjust_section::levels : adjust_section::light);
      if (next == "it" || next.empty()) want_auto(adjust_section::colour);
      if (!next.empty()) ++i;
      continue;
    }
    if (w == "keep" || w == "except") {
      keep_next = true;
      continue;
    }
    if (w == "noise" && (next == "reduction" || next == "removal")) {
      // "noise reduction": the denoise slider, the way the words around it say.
      if (!keep_next) apply_word(*find("denoise"), i + 1, sign * scale);
      keep_next = false;
      ++i;
      sign = 1.0f;
      scale = 1.0f;
      continue;
    }
    if (w == "down") {
      // Right after the last mover (filler words aside), or before the next.
      bool follows = last.w != nullptr;
      for (std::size_t j = follows ? last.at + 1 : i; j < i; ++j) follows = follows && in(kSkip, t[j]);
      if (follows && in(kNouns, last.w->text)) {
        // "exposure down": the mover just made, the other way.
        r.value = last.before;
        moved = last.moved;
        touched = last.touched;
        const word& again = *last.w;
        apply_word(again, last.at, -last.factor);
      } else if (!follows) {
        sign = -sign;  // "turn down the contrast"
      }
      // After an adjective it is an echo: "cool it down".
      continue;
    }
    if (in(kFlip, w)) {
      sign = -sign;
      continue;
    }
    if (in(kKeep, w)) continue;
    if (in(kSmall, w)) {
      scale = 0.5f;
      continue;
    }
    if (in(kBig, w)) {
      scale = 2.0f;
      continue;
    }
    const word* k = find(w);
    if (k == nullptr) {
      if (!in(kSkip, w)) r.unknown.push_back(w);
      continue;
    }
    // An adjective or verb just before a region gives the region its way:
    // "brighter shadows", "darker sky", "lift the shadows".
    if (!k->region) {
      std::size_t j = i + 1;
      while (j < t.size() && in(kSkip, t[j])) ++j;
      const word* region = j < t.size() ? find(t[j]) : nullptr;
      if (region != nullptr && region->region && k->count == 1 &&
          (k->moves[0].param == P::exposure || k->moves[0].param == P::contrast)) {
        const float way = k->moves[0].amount > 0.0f ? 1.0f : -1.0f;
        if (!keep_next) {
          for (int m = 0; m < region->count; ++m) {
            const move& mv = region->moves[static_cast<std::size_t>(m)];
            apply(mv.param, mv.amount * way * sign * scale, false);
          }
        }
        keep_next = false;
        i = j;
        sign = 1.0f;
        scale = 1.0f;
        continue;
      }
    }
    if (keep_next) {
      keep_next = false;
      continue;
    }
    apply_word(*k, i, sign * scale);
    sign = 1.0f;
    scale = 1.0f;
  }
  for (int i = 0; i < kAdjustParamCount; ++i) {
    const auto k = static_cast<std::size_t>(i);
    if (touched[k] && moved[k] != 0.0f) r.applied.push_back(format_move(static_cast<P>(i), moved[k]));
    if (touched[k]) r.recognised = true;
  }
  if (!r.autos.empty()) r.recognised = true;
  return r;
}

std::string prompt_summary(const prompt_result& r) {
  std::string s;
  for (const std::string& a : r.applied) {
    if (!s.empty()) s += " · ";
    s += a;
  }
  for (const adjust_section sec : r.autos) {
    if (!s.empty()) s += " · ";
    s += sec == adjust_section::light ? "Auto light" : sec == adjust_section::colour ? "Auto colour" : "Auto levels";
  }
  if (s.empty() && !r.recognised) return kPromptNotRecognised;
  if (!r.unknown.empty()) {
    s += s.empty() ? "Not understood: " : " · not understood: ";
    for (std::size_t i = 0; i < r.unknown.size(); ++i) {
      if (i) s += ", ";
      s += r.unknown[i];
    }
  }
  if (s.empty()) s = "Nothing to change: the sliders are already there";
  return s;
}

}  // namespace mv::edit
