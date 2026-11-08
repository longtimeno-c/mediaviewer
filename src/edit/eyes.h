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

// How differently two faces are turned: the nose and mouth corners placed in
// each face's own frame (eye midpoint at 0, the eye line along x, eye distance
// 1), the mean distance between the two. 0 the same pose; a frontal face and
// one turned about 30 degrees are about 0.25 apart. Landmarks are 0..1 of
// their image, so each image's size is given. A donor from another day (People)
// must be within kPoseMatch: further, its eyes look the wrong way.
[[nodiscard]] float pose_distance(const face_landmarks& a, float aw, float ah, const face_landmarks& b, float bw,
                                  float bh) noexcept;
inline constexpr float kPoseMatch = 0.12f;

// The whole inner face (brows to chin, cheek to cheek) from a donor photo of
// the same person, turned as this one is (owner 2026-11-04: "improve faces from
// your photos"): aligned by the least-squares similarity of the five
// landmarks, its light, glints, sharpness and grain matched to the target's as
// transfer_eyes does, feathered into the face around it. invalid_arg for a
// face under 24 px between the eyes or a donor at another pose.
//
// The light and colour come from this photo area by area (the donor times
// this photo's blur of the face over its own), so a donor lit by a coloured
// spot, or with paint on its cheeks, does not carry that over: when the
// donor's colour differs from this photo's in patches (the spread of the
// red/green and blue/green ratios over the face's middle over kLookPatchy, in
// log units) it is refused, invalid_arg, and the next donor may be tried.
[[nodiscard]] expected transfer_face(image::linear_image& target, const face_landmarks& target_lm,
                                     const image::linear_image& donor, const face_landmarks& donor_lm);
// The whole-face ellipse's half axes, in eye distances: cheek to cheek, and
// brows to chin (not the hairline: a donor's hair is not this photo's).
inline constexpr float kFaceHalfWidth = 1.05f;
inline constexpr float kFaceHalfHeight = 1.12f;
// 2026-11-07, IMG_2113 against five photos of the same person: clean donors
// 0.041-0.047; one with flag paint on both cheeks 0.094 (paint showed); one
// in other light 0.089 (washed out).
inline constexpr float kLookPatchy = 0.07f;

// docs/plans/edit-refresh.md S8: whether a face's eyes look shut on their own,
// with no donor to compare against (the Faces panel offers Open eyes only
// then): either eye's dark band (darker than 55 % of the skin about it: lashes,
// or lids and iris) covers under kShutRows of its patch's height. An open eye
// shows the iris over half the patch tall; a shut one a line of lashes. A
// guess for the button, not the fix's test (that compares with the donor,
// kEyesBetter).
[[nodiscard]] bool eyes_look_shut(const image::linear_image& img, const face_landmarks& lm) noexcept;
inline constexpr float kShutRows = 0.26f;

// S8: the part of the picture a fix of one face may change, as x, y, w, h in
// 0..1 of the upright still: the detector's box (0..1, the pack's x, y, w, h)
// grown by kFaceRegionGrow of its size each side, joined with the bounds of
// the whole-face ellipse transfer_face pastes (from the landmarks; `w` x `h`
// the still in pixels), clamped to the picture. Every pixel outside it is
// left exactly as it was (media::fix_faces_in fades the fix to nothing at its
// edge), and it is the box the Faces tool draws on the canvas.
inline constexpr float kFaceRegionGrow = 0.12f;
[[nodiscard]] std::array<float, 4> face_region(const std::array<float, 4>& box, const face_landmarks& lm, float w,
                                               float h) noexcept;

// `target` with both eyes of the face at `target_lm` replaced by those of the
// donor face at `donor_lm` in `donor`. Refused (invalid_arg) when either
// face's eyes are under 12 px apart: nothing to transfer.
[[nodiscard]] expected transfer_eyes(image::linear_image& target, const face_landmarks& target_lm,
                                     const image::linear_image& donor, const face_landmarks& donor_lm) noexcept;

}  // namespace mv::edit
