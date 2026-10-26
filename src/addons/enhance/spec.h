// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297): what an installed model is and which knobs it has.
//
// Each model Enhance can run ships as models/<kind>/model.json in the add-on's
// signed folder; its weights are not in the zip. They are files the add-on's
// signed manifest pins under `remote` (Hugging Face URL, size, SHA-256,
// licence) at "<kind>/<file>", fetched on the person's click into the
// add-on's data folder (downloads.h). The sheet is built from the model, not
// from a fixed form: each knob says which kinds of item it applies to, and the
// sheet shows only those (sheet.h).
//
// Portable, no I/O (D9).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace mv::enhance {

// What a model enhances. A clip's picture is `video`, its sound `audio`.
enum class model_kind : std::uint8_t { still = 0, video = 1, audio = 2 };
inline constexpr std::array<model_kind, 3> kModelKinds{model_kind::still, model_kind::video, model_kind::audio};

[[nodiscard]] const char* kind_name(model_kind k) noexcept;  // "still" | "video" | "audio"
[[nodiscard]] std::optional<model_kind> kind_from(std::string_view name) noexcept;

// One weights file is at most this (#297, owner 2026-10-10): a pin over it is
// refused before anything is requested. Not a budget shared across kinds.
inline constexpr std::uint64_t kMaxWeightsBytes = 5'000'000'000ull;

struct knob_option {
  std::string id;
  std::string text;
};

struct knob {
  enum class type : std::uint8_t { slider, choice, toggle };
  std::string id;     // [a-z0-9_]{1,32}, unique in its model
  std::string label;  // "Noise reduction"
  std::string help;   // may be empty
  type t = type::slider;
  // slider
  double min = 0, max = 1, step = 0, def = 0;
  // choice
  std::vector<knob_option> options;
  std::string def_choice;
  // toggle
  bool def_on = false;
  // Which kinds of target it shows for ("still", "video", "audio"). The
  // model's own kind when the file names none.
  std::vector<model_kind> kinds;
};

struct model_spec {
  model_kind kind = model_kind::still;
  std::string name;     // "Speech cleanup"
  std::string credit;   // "DeepFilterNet3 (MIT)": who made the weights, and their licence
  // The remote paths this model needs, relative to its kind ("denoise.onnx"
  // is the manifest's remote "audio/denoise.onnx").
  std::vector<std::string> weights;
  // Free memory a run needs (RAM, and VRAM when the provider is CUDA): the
  // run gate, checked before the first unit of work.
  std::uint64_t run_bytes = 0;
  // CPU is offered only when one unit of work finishes inside a measured
  // budget on it; otherwise the model needs the GPU.
  bool cpu = false;
  std::vector<knob> knobs;
  // The model's own section (tensor names, tile size, sample rate): read by
  // the runner for that kind, opaque here.
  json::value io;

  [[nodiscard]] const knob* find(std::string_view id) const noexcept;
};

// Parses and checks models/<kind>/model.json. Refused (nullopt) when it is not
// that kind's, names no weights, or any knob is malformed: an add-on whose
// model.json does not parse offers that kind as not installed.
[[nodiscard]] std::optional<model_spec> parse_spec(std::string_view json_text, model_kind expected);

// A knob's value as JSON text ("0.800", "\"2\"", "true"), or its default
// when `stored` is absent or out of range / not one of the options.
[[nodiscard]] json::value knob_value(const knob& k, const json::value* stored);

// Writes a knob value (a number, string or boolean) to `w`; null otherwise.
void write_scalar(json::writer& w, const json::value& v);

// Checks a value the sheet reports for knob `k`; nullopt when it is not one
// the knob could hold (the host checks the tree too; this is the add-on's own).
[[nodiscard]] std::optional<json::value> check_knob_value(const knob& k, const json::value& v);

}  // namespace mv::enhance
