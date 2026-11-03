// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-adjust.md slice 6: eyes open from the same moment. A face
// whose eyes are shut takes the eyes of the same person from a neighbouring
// frame of the burst: real pixels, aligned, colour-matched and blended, never
// generated. Owner call 2026-11-02: the People face data may be used for this
// edit (docs/design/12); which faces are the same person comes from the AI
// pack (mv_ai_api eyes_json), everything here is pure image work.
//
// Landmarks are YuNet's five points, 0..1 of the upright picture: right eye,
// left eye, nose tip, right and left mouth corners (x, y each).
//
//   openness   per eye, in a patch about its centre sized by the distance
//              between the eyes: the spread of lightness (an open eye has a
//              dark iris against white) times how tall the dark part is (a
//              shut eye is a thin dark line of lashes). Comparable within one
//              person's burst, not across people; no model.
//   transfer   the similarity (scale, turn, shift) that takes the donor's eye
//              centres onto the target's; each eye's ellipse of the donor,
//              its colour matched to the ring of skin round the target's,
//              blended in on a feathered edge.
// Worker only (rule 1).
#pragma once

#include <array>
#include <span>

#include "core/job_system.h"
#include "core/result.h"
#include "image/linear.h"

namespace mv::edit {

using face_landmarks = std::array<float, 10>;

// The lower of the two eyes' openness (a face counts as shut if either is).
[[nodiscard]] float eye_openness(const image::linear_image& img, const face_landmarks& lm) noexcept;

// A donor counts as better when its openness is at least this times the
// target's: a little more open is noise, not a fix.
inline constexpr float kEyesBetter = 1.35f;

// `target` with both eyes of the face at `target_lm` replaced by those of the
// donor face at `donor_lm` in `donor`. Refused (invalid_arg) when either
// face's eyes are under 12 px apart: nothing to transfer.
[[nodiscard]] expected transfer_eyes(image::linear_image& target, const face_landmarks& target_lm,
                                     const image::linear_image& donor, const face_landmarks& donor_lm) noexcept;

}  // namespace mv::edit
