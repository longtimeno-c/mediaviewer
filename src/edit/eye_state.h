// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Whether a face's eyes are open, from a small learned model (owner
// 2026-11-07: "detect closed eyes", after brightness rules could not tell a
// blink from a big smile or a flash's red eye on real photos).
//
// The model is OpenVINO Open Model Zoo's open-closed-eye-0001 (Intel, Apache
// 2.0; docs/design/11-licensing.md): about 11 k weights, a 32x32 eye in, open
// or closed out. It was trained on infrared eyes, so each crop is shown to it
// as grey with its brightness and contrast standardised; that is what lets it
// read colour party photos (red eye, dim light, squints). It runs here, in
// plain C++, with its weights compiled in (eye_state_weights.inc, written by
// tools/eyes/export_eye_state.py): no runtime, no download, the same on every
// platform.
//
// Each eye is read square to the eye line (the landmarks' tilt taken out) at
// three sizes about its centre, each with four contrast settings and mirrored:
// the mean is the eye's probability of being open. Landmarks must sit on the
// eyes: YuNet's own points can be a quarter of an eye distance off on a tilted
// face (media::refine_landmarks puts them right first).
//
// 2026-11-07, 68 faces from two of the owner's folders labelled by eye (5 with
// both eyes shut, 3 with one): with refined landmarks every shut face read
// under 0.58 and every open one over 0.68 (squints, red eye, dim bar light,
// face paint included). Worker only (rule 1): about 30 ms a face.
#pragma once

#include <array>

#include "image/linear.h"

namespace mv::edit {

using face_landmarks = std::array<float, 10>;

// Probability that each eye is open, 0..1; -1 where it could not be read (the
// eyes under 10 px apart, or the crop off the picture).
struct eye_state {
  float right = -1.0f;  // lm[0..1], the eye on the picture's left
  float left = -1.0f;   // lm[2..3]
  [[nodiscard]] bool known() const noexcept { return right >= 0.0f && left >= 0.0f; }
  // The more open eye: a face reads as open when either eye is.
  [[nodiscard]] float open() const noexcept { return right > left ? right : left; }
};

// Under this an eye counts as shut.
inline constexpr float kEyeOpen = 0.63f;
// A fix's result must read this open (its more open eye) to be kept.
inline constexpr float kEyeFixedOpen = 0.9f;

// Whether a face's eyes can be read at all; read_eye_state answers unknown
// when not (never "shut"): eyes at least kEyeReadMinPx apart (smaller is a
// blur to the model), both inside the picture, the face not turned to profile
// or seen steeply from below (the nose within 0.3 eye distances of the eyes'
// middle across, 0.2..0.85 below), and no dark glasses (the lids over both
// eyes darker than half the cheeks'), and a brow above at least one of the
// points (a band under 0.72 of the lid's light, 0.2..0.42 eye distances up:
// YuNet can put a face's eye points on its brows). 2026-11-07, every photo of
// two of the owner's folders (284): 57 faces read shut before these, 15 after;
// gone to unknown were sunglasses, faces under 45 px, profiles and points on
// brows; one blink with faint brows went too. And sharp enough: a band over
// both eyes, levelled to 64 px between them, whose mean |Laplacian| is at
// least 0.025 of its mean grey (a blurred face read 0.016, blinks 0.034 up).
// A shut reading also needs skin under the points and eyes no greyer than
// the cheeks (eye_state.cpp looks_shut: a lens, or the real eye under points
// on a brow), else the eyes are unknown.
inline constexpr float kEyeReadMinPx = 45.0f;
[[nodiscard]] bool eyes_readable(const image::linear_image& img, const face_landmarks& lm) noexcept;

[[nodiscard]] eye_state read_eye_state(const image::linear_image& img, const face_landmarks& lm) noexcept;

// Both eyes shut (a wink, one eye shut, is left alone).
[[nodiscard]] inline bool eyes_closed(const eye_state& s) noexcept { return s.known() && s.open() < kEyeOpen; }

// The model on one 32x32 grey eye (row-major, 0..255): the probability it is
// open. Exposed for tests.
[[nodiscard]] float eye_open_probability(const std::array<float, 32 * 32>& grey) noexcept;

}  // namespace mv::edit
