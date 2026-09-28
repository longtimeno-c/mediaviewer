// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People refinement (plan/17 PR 24, "Refinement"): a background re-check of
// every face against the person it was filed under, after the online
// clustering in faces_db::add. Pure logic over vectors: no SQLite, no model,
// no threads, so the rules are tested on synthetic embeddings.
//
// Scores are in PAIRWISE cosine units (the units SFace's 0.363 verification
// threshold is quoted in), never cosine-to-a-normalised-centroid, which
// inflates an impostor's score by 1/sqrt(the cluster's own mean similarity).
//
// Per person, per pass:
//   refs      the members worth trusting: pinned faces (the user's), then the
//             good-quality ones, at most max_refs
//   anchors   pinned faces, plus the medoid of the refs (unless the person has
//             pins and the medoid does not resemble any of them)
//   core      refs within `core` of an anchor: one hop, so no chaining
//   exemplars pinned first, then the core, at most max_exemplars
//   support(f, p) = mean of the top_k cosines of f to p's exemplars (not f)
//
// A face that is not pinned then
//   moves    to another person b when support(f, b) >= join and beats both
//            its own person and the runner-up by `margin` (never a weak face,
//            never into a person the user rejected it from)
//   leaves   to unassigned when support(f, own) < keep (keep_weak if weak):
//            a doubtful face is better filed under nobody than a wrong name
//   joins    (unassigned) the clear winner, by the same join + margin test
// Passes repeat until nothing changes or `passes` run; a face changes at
// most once per call. With `regroup`, unassigned good faces that are close to
// each other (average linkage at `join`) become new unnamed people, which is
// how a person that chaining glued onto another comes back out.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace mv::ai {

struct refine_params {
  float join = 0.40f;       // admit / move: the model's same_person (SFace ~0.363 + slack)
  float keep = 0.30f;       // stay: below this a face is not like its own person
  float keep_weak = 0.34f;  // stay, for a weak face (its vector is noisier)
  float margin = 0.08f;     // a move or admission beats own and runner-up by this
  float core = 0.40f;       // one hop from an anchor
  float weak_quality = 0.35f;  // below: never a reference, never moved, never admitted
  std::uint32_t top_k = 3;
  std::uint32_t max_exemplars = 12;
  std::uint32_t max_refs = 256;     // caps the medoid's O(n^2) on a huge person
  std::uint32_t shortlist = 3;      // alternatives scored by exemplars after the mean prefilter
  std::uint32_t passes = 4;
  std::uint32_t min_group = 2;      // a regrouped person needs this many faces
  std::uint32_t max_regroup = 4000; // unassigned faces considered for new people per call
  std::uint32_t max_recheck = 32;   // borderline faces returned for a better embedding
};

struct refine_face {
  std::int64_t id = 0;
  std::int64_t person = 0;  // 0: unassigned
  float quality = 0.5f;     // 0..1 (face_quality); unknown rows arrive as a proxy
  bool pinned = false;      // the user put it there: never judged, always an anchor
  std::uint32_t row = 0;    // row of the embedding matrix (dim floats, L2-normalised)
  std::vector<std::int64_t> rejected;  // sorted: persons it must never join
};

// A person's prototype. Returned for the persons a call rebuilt; passed back
// as `fixed` for the persons an incremental call does not rebuild.
struct person_proto {
  std::int64_t person = 0;
  bool named = false;
  bool pinned = false;              // it has a pinned face (always a candidate)
  std::vector<float> exemplars;     // n * dim, L2-normalised
  std::vector<std::int64_t> exemplar_ids;
  std::vector<float> core_mean;     // dim; unnormalised, so dot = mean pairwise cosine
};

enum class refine_why : std::uint8_t { evict, move, admit, regroup };

struct refine_move {
  std::int64_t face = 0;
  std::int64_t from = 0;  // 0: was unassigned
  std::int64_t to = 0;    // 0: unassigned; < 0: new person -(g + 1) (regroup)
  refine_why why = refine_why::evict;
  float own = 0;          // support by its own person when judged (-1: no evidence)
  float best = 0;         // support by the person it went to (-1: none)
};

struct refine_input {
  std::uint32_t dim = 0;
  std::span<const float> emb;             // rows * dim
  std::span<const refine_face> faces;     // every face of the rebuilt persons, and unassigned ones
  std::span<const person_proto> fixed;    // candidates not rebuilt this call
  std::span<const std::int64_t> named;    // sorted ids of named persons
  bool regroup = false;
  const std::atomic<bool>* cancel = nullptr;  // set: stop early, change nothing
};

struct refine_output {
  std::vector<refine_move> moves;         // at most one per face
  std::uint32_t groups = 0;               // new people (to = -1 .. -groups)
  std::vector<person_proto> protos;       // the rebuilt persons, after the last move
  std::vector<std::int64_t> recheck;      // borderline faces worth a better embedding
  std::uint32_t passes = 0;
};

[[nodiscard]] refine_output refine_people(const refine_input& in, const refine_params& p);

// 0..1 from what the analyser knows of a crop: detector score, the face's
// short side in pixels of the analysed image, the aligned crop's sharpness
// (variance of the Laplacian of its luma, 0..255 scale) and how frontal the
// landmarks are (0 profile .. 1 frontal).
[[nodiscard]] float face_quality(float score, float side_px, float sharpness, float frontal);
// A stand-in for rows analysed before quality was recorded: score and size.
[[nodiscard]] float face_quality_proxy(float score, float w, float h);
// Luma Laplacian variance of a CHW 3 x side x side tensor (0..255 floats).
[[nodiscard]] float crop_sharpness(std::span<const float> chw, std::uint32_t side);
// 0..1: the nose's offset from the eyes' midline against the eye distance
// (YuNet order: right eye, left eye, nose, mouth right, mouth left; x, y).
[[nodiscard]] float landmark_frontalness(std::span<const float, 10> lm);

}  // namespace mv::ai
