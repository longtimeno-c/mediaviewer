// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/generators.h"

#include <array>
#include <utility>

namespace mv::meta {
namespace {

using entry = std::pair<std::string_view, generator>;

// Match order: the more specific spelling first ("Adobe Firefly" before
// "Firefly", "ChatGPT" before "OpenAI", the A1111 web UI before "Stable
// Diffusion"). A pattern matches as whole words, ignoring ASCII case.
constexpr std::array kTable{
    entry{"ChatGPT", {"ChatGPT"}},
    entry{"GPT-4o", {"ChatGPT"}},
    entry{"gpt-image", {"ChatGPT"}},
    entry{"DALL\xC2\xB7" "E", {"DALL\xC2\xB7" "E"}},
    entry{"DALL-E", {"DALL\xC2\xB7" "E"}},
    entry{"DALL E", {"DALL\xC2\xB7" "E"}},
    entry{"DALLE", {"DALL\xC2\xB7" "E"}},
    entry{"Sora", {"Sora"}},
    entry{"OpenAI", {"OpenAI"}},
    entry{"Adobe Firefly", {"Adobe Firefly"}},
    entry{"Firefly", {"Adobe Firefly"}},
    entry{"Midjourney", {"Midjourney"}},
    entry{"stable-diffusion-webui", {"Stable Diffusion (AUTOMATIC1111)"}},
    entry{"AUTOMATIC1111", {"Stable Diffusion (AUTOMATIC1111)"}},
    entry{"ComfyUI", {"ComfyUI"}},
    entry{"InvokeAI", {"InvokeAI"}},
    entry{"Fooocus", {"Fooocus"}},
    entry{"NovelAI", {"NovelAI"}},
    entry{"DreamStudio", {"DreamStudio"}},
    entry{"Stable Diffusion", {"Stable Diffusion"}},
    entry{"StableDiffusion", {"Stable Diffusion"}},
    entry{"SDXL", {"Stable Diffusion"}},
    entry{"Stability AI", {"Stability AI"}},
    entry{"Bing Image Creator", {"Bing Image Creator"}},
    entry{"Microsoft Designer", {"Microsoft Designer"}},
    // Not bare "Meta AI": that is also the Ray-Ban Meta glasses' companion app,
    // which carries real camera photos. Not bare "Imagen": Spanish for "image".
    entry{"Imagine with Meta", {"Meta AI"}},
    entry{"Google Imagen", {"Google Imagen"}},
    entry{"Gemini", {"Google Gemini"}},
    entry{"Leonardo.Ai", {"Leonardo.Ai"}},
    entry{"Leonardo AI", {"Leonardo.Ai"}},
    entry{"Ideogram", {"Ideogram"}},
    entry{"FLUX.1", {"FLUX"}},
    entry{"Black Forest Labs", {"FLUX"}},
    entry{"RunwayML", {"Runway"}},
    entry{"Runway Gen-2", {"Runway"}},
    entry{"Runway Gen-3", {"Runway"}},
    entry{"Luma Dream Machine", {"Luma Dream Machine"}},
    entry{"Kling AI", {"Kling"}},
    entry{"Krea AI", {"Krea"}},
    entry{"Playground AI", {"Playground"}},
    entry{"Canva Magic Media", {"Canva Magic Media"}},
    entry{"Recraft", {"Recraft"}},
    entry{"NightCafe", {"NightCafe"}},
    entry{"Craiyon", {"Craiyon"}},
    entry{"Grok", {"Grok"}},
};

constexpr char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool alnum(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool contains_word(std::string_view text, std::string_view word) noexcept {
  if (word.empty() || text.size() < word.size()) return false;
  for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
    std::size_t k = 0;
    while (k < word.size() && lower(text[i + k]) == lower(word[k])) ++k;
    if (k != word.size()) continue;
    const bool left = i == 0 || !alnum(text[i - 1]);
    const bool right = i + k == text.size() || !alnum(text[i + k]);
    if (left && right) return true;
  }
  return false;
}

}  // namespace

std::optional<generator> match_generator(std::string_view text) noexcept {
  if (text.empty()) return std::nullopt;
  // A generator's name is near the start of any field that carries it; a
  // multi-kilobyte prompt is not searched end to end.
  if (text.size() > 4096) text = text.substr(0, 4096);
  for (const auto& [pattern, g] : kTable) {
    if (contains_word(text, pattern)) return g;
  }
  return std::nullopt;
}

std::span<const std::pair<std::string_view, generator>> generator_table() noexcept { return kTable; }

std::string_view source_type_term(std::string_view uri) noexcept {
  while (!uri.empty() && (uri.back() == '/' || uri.back() == ' ')) uri.remove_suffix(1);
  const auto slash = uri.find_last_of("/#:");
  return slash == std::string_view::npos ? uri : uri.substr(slash + 1);
}

bool source_type_is_ai(std::string_view uri) noexcept {
  const std::string_view t = source_type_term(uri);
  return t == "trainedAlgorithmicMedia" || t == "compositeWithTrainedAlgorithmicMedia";
}

std::string describe_source_type(std::string_view uri) {
  const std::string_view t = source_type_term(uri);
  if (t == "trainedAlgorithmicMedia") return "AI-generated (trainedAlgorithmicMedia)";
  if (t == "compositeWithTrainedAlgorithmicMedia") {
    return "Includes AI-generated elements (compositeWithTrainedAlgorithmicMedia)";
  }
  // Any other IPTC term, its camelCase read as words: "digitalCapture" → "Digital capture".
  std::string out;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    if (c >= 'A' && c <= 'Z' && i > 0) {
      out += ' ';
      out += lower(c);
    } else {
      out += i == 0 && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
  }
  return out.empty() ? std::string(uri) : out + " (" + std::string(t) + ")";
}

std::string evidence_line(std::string_view field, std::string_view value) {
  std::string out(field);
  out += ": ";
  out += parse::clean_text(value, 80);
  return out;
}

provenance derive_provenance(const parse::evidence& ev) {
  provenance p;
  // An AI source type is the strongest statement a file can make. source_types
  // holds XMP's and C2PA's together (XMP's first: the readers run in that
  // order), so XMP's own term is set aside once and the C2PA claim, when there
  // is one, is named with its own term.
  std::string_view c2pa_term;
  bool any_ai = false;
  bool xmp_skipped = false;
  const bool xmp_ai = source_type_is_ai(ev.xmp_source_type);
  for (const std::string& s : ev.source_types) {
    if (!source_type_is_ai(s)) continue;
    any_ai = true;
    if (xmp_ai && !xmp_skipped && s == ev.xmp_source_type) {
      xmp_skipped = true;
      continue;
    }
    if (c2pa_term.empty()) c2pa_term = s;
  }
  if (any_ai) {
    p.ai_generated = true;
    p.evidence = !c2pa_term.empty() ? "Content Credentials (C2PA): " + describe_source_type(c2pa_term)
                                    : "IPTC digital source type (XMP): " + describe_source_type(ev.xmp_source_type);
  }

  // Every field that can name the tool, most authoritative first.
  const auto try_match = [&](std::string_view field, std::string_view text) {
    if (!p.made_with.empty()) return;
    if (const auto g = match_generator(text)) {
      p.made_with = std::string(g->name);
      if (g->ai && !p.ai_generated) {
        p.ai_generated = true;
        p.evidence = evidence_line(field, text);
      }
    }
  };
  try_match("C2PA claim generator", ev.c2pa_generator);
  for (const std::string& a : ev.c2pa_agents) try_match("C2PA software agent", a);
  try_match("PNG Software", ev.png_software);
  for (const std::string& t : ev.png_generator_text) try_match("PNG text", t);
  try_match("XMP CreatorTool", ev.xmp_creator_tool);
  try_match("EXIF Software", ev.exif_software);

  // The generator chunks themselves: these tools write them and nothing else does.
  if (ev.png_parameters || ev.png_comfyui) {
    if (!p.ai_generated) {
      p.evidence = ev.png_comfyui ? "PNG prompt / workflow chunks (a ComfyUI graph)"
                                  : "PNG parameters chunk (Stable Diffusion settings)";
    }
    p.ai_generated = true;
    if (p.made_with.empty()) p.made_with = ev.png_comfyui ? "ComfyUI" : "Stable Diffusion";
  }
  // A signed claim names the tool that made the file even when it is not a generator.
  if (p.made_with.empty()) p.made_with = ev.c2pa_generator;
  return p;
}

}  // namespace mv::meta
