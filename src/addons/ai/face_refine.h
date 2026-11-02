// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People refinement (docs/design/17 PR 24, "Refinement"): a re-check of a person's
// faces against the person they were filed under, after the online
// clustering in faces_db::add. Runs only when the user asks (the person's
// "Refine" in People), never in the background. Pure logic over vectors: no SQLite, no model,
// no threads, so the rules are tested on synthetic embeddings.
//
// Scores are in PAIRWISE cosine units (the units SFace's 0.363 verification
// threshold is quoted in), never cosine-to-a-normalised-centroid, which
// inflates an impostor's score by 1/sqrt(the cluster's own mean similarity).
//
// Per person, per pass:
//   refs      the members worth trusting: pinned faces (the user's), then the
//             good-quality ones, at most max_refs
//   anchors   the usable pinned faces, plus the medoid of the refs (unless
//             the person has pins and the medoid does not resemble any of
//             them). A lone pin (it agrees with none of the person's other
//             pins nor the medoid) that another person explains better by
//             `margin` is distrusted when the person has other pins: not an
//             anchor, still never moved (2026-11-02: a merge pinned an
//             impostor's cover, and it held 153 faces of someone else)
//   core      refs within `core` of an anchor: one hop, so no chaining
//   exemplars trusted pins first, then the core, at most max_exemplars and
//             one per photo or clip (a clip's frames are one sighting)
//   support(f, p) = mean of the top_k cosines of f to p's exemplars, not f,
//             and not from f's own photo or clip while p has others
//   negatives the faces the user said are not p ("not this person"):
//             neg(f, p) is the same mean over them
//
// A face that is not pinned then
//   moves    to another person b when support(f, b) >= join and beats both
//            its own person, the runner-up and neg(f, b) by `margin` (never a
//            weak face, never into a person the user rejected it from, never
//            into one that holds another face of its photo or frame)
//   leaves   to unassigned when support(f, own) < keep (keep_weak if weak):
//            a doubtful face is better filed under nobody than a wrong name;
//            and always when it is unusable (quality < unusable_quality), or
//            its person is unnamed, unpinned and has no face that is not weak,
//            or the user rejected it from that person, or it resembles what
//            the user rejected from that person more than the person (neg >=
//            join), or a better face of its own frame is in that person (two
//            faces in one frame are two people)
//   joins    (unassigned) the clear winner, by the same join + margin test;
//            a weak clip frame joins the person its track is in: the clear
//            frames of its clip within track_ms that it resembles at `join`,
//            all in one person
// Passes repeat until nothing changes or `passes` run; a face changes at
// most once per call. With `regroup`, unassigned good faces that are close to
// each other (average linkage at `join`) become new unnamed people, which is
// how a person that chaining glued onto another comes back out.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <utility>
#include <string>
#include <vector>

namespace mv::ai {

struct refine_params {
  float join = 0.40f;       // admit / move: the model's same_person (SFace ~0.363 + slack)
  float keep = 0.30f;       // stay: below this a face is not like its own person
  float keep_weak = 0.34f;  // stay, for a weak face (its vector is noisier)
  float margin = 0.08f;     // a move or admission beats own and runner-up by this
  float core = 0.40f;       // one hop from an anchor
  float weak_quality = 0.35f;  // below: never a reference, never moved, never admitted
  // Below: nobody can be told from this face (a blurred clip frame, a
  // 25 px stranger, a hood or an ear the detector took for a face). It
  // belongs to no person; it is evicted from any it is in.
  float unusable_quality = 0.10f;
  std::uint32_t top_k = 3;
  std::uint32_t max_exemplars = 12;
  std::uint32_t max_refs = 256;     // caps the medoid's O(n^2) on a huge person
  std::uint32_t shortlist = 3;      // alternatives scored by exemplars after the mean prefilter
  std::uint32_t passes = 4;
  std::uint32_t min_group = 2;      // a regrouped person needs this many faces
  std::uint32_t max_regroup = 4000; // unassigned faces considered for new people per call
  std::uint32_t max_recheck = 32;   // borderline faces returned for a better embedding
  std::uint32_t max_negatives = 12; // rejected faces a person is checked against
  // A weak clip frame follows its track: clear frames of the same clip this
  // close in time (ms) that it resembles at `join`, when they agree on one person.
  std::int64_t track_ms = 15000;
};

struct refine_face {
  std::int64_t id = 0;
  std::int64_t person = 0;  // 0: unassigned
  float quality = 0.5f;     // 0..1 (face_quality); unknown rows arrive as a proxy
  bool pinned = false;      // the user put it there: never judged, always an anchor
  std::uint32_t row = 0;    // row of the embedding matrix (dim floats, L2-normalised)
  std::vector<std::int64_t> rejected;  // sorted: persons it must never join
  // Its photo or clip, and the frame in it (-1: a still). asset < 0: unknown,
  // every face its own sighting (the synthetic tests).
  std::int64_t asset = -1;
  std::int64_t pts_ms = -1;
};

// A person's prototype. Returned for the persons a call rebuilt; passed back
// as `fixed` for the persons an incremental call does not rebuild.
struct person_proto {
  std::int64_t person = 0;
  bool named = false;
  bool pinned = false;              // it has a pinned face (always a candidate)
  // Every member is weak: unnamed and unpinned, nothing vouches for it as a
  // person, so its faces leave and it is no candidate.
  bool weak_only = false;
  std::vector<float> exemplars;     // n * dim, L2-normalised
  std::vector<std::int64_t> exemplar_ids;
  std::vector<std::int64_t> exemplar_assets;  // parallel to exemplar_ids; -1 unknown
  std::vector<float> core_mean;     // dim; unnormalised, so dot = mean pairwise cosine
  std::uint32_t members = 0;        // the faces it was built from (refs, at most max_refs)
  // Pinned faces that agree with none of the person's other pins (or are
  // unusable): not anchors; the chrome offers to unpin them.
  std::vector<std::int64_t> doubtful_pins;
  // Faces the user rejected from this person (n * dim), at most max_negatives:
  // filled by the caller that knows them (faces_db; refine_people from its faces).
  std::vector<float> negatives;
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
  // Set: judge only the faces filed under this person (the user asked to
  // refine them); every other face stays put and serves as a candidate, and
  // regroup draws only on faces this call took from `focus`.
  std::int64_t focus = 0;
  const std::atomic<bool>* cancel = nullptr;  // set: stop early, change nothing
};

struct refine_output {
  std::vector<refine_move> moves;         // at most one per face
  std::uint32_t groups = 0;               // new people (to = -1 .. -groups)
  std::vector<person_proto> protos;       // the rebuilt persons, after the last move
  std::vector<std::int64_t> recheck;      // borderline faces worth a better embedding
  std::vector<std::int64_t> distrusted;   // lone pins another person explains: not anchors
  std::uint32_t passes = 0;
};

[[nodiscard]] refine_output refine_people(const refine_input& in, const refine_params& p);

// One person's prototype from its faces (rows of `emb`, dim floats each), by
// the very rules refine_people builds with: faces_db's online matcher caches
// these, so the two cannot drift. Only the first max_refs refs count, as there.
[[nodiscard]] person_proto build_person_proto(std::uint32_t dim, std::span<const float> emb,
                                              std::span<const refine_face> faces, std::int64_t person,
                                              bool named, const refine_params& p);
// support(v, p): the mean of the top_k cosines of `v` to p's exemplars,
// skipping `self` (0 skips nothing) and, while p has exemplars from other
// photos or clips, those of `self_asset` (< 0 skips none). -1 when it has none.
[[nodiscard]] float exemplar_support(std::span<const float> v, const person_proto& p, std::int64_t self,
                                     std::uint32_t top_k, std::int64_t self_asset = -1);
// neg(v, p): the same mean over p's negatives (the faces rejected from p). -1
// when it has none.
[[nodiscard]] float negative_support(std::span<const float> v, const person_proto& p, std::uint32_t top_k);

// ---- duplicates (docs/design/17 "Merge duplicates") ----------------------------------

struct dedupe_person {
  std::int64_t id = 0;
  std::string name;          // "" unnamed
  std::uint32_t faces = 0;
};

struct dedupe_input {
  std::uint32_t dim = 0;
  std::span<const person_proto> protos;   // every person, as a full refinement rebuilt them
  std::span<const dedupe_person> people;  // names and sizes (any order); a proto without one is unnamed, 0 faces
  // (min, max) pairs never merged: a split kept them apart, or a face of one
  // was rejected from the other. Sorted.
  std::span<const std::pair<std::int64_t, std::int64_t>> apart;
  const std::atomic<bool>* cancel = nullptr;  // set: stop early, return nothing
};

// People who are the same person. Two people link when each one's exemplars
// vouch for the other's, both ways, at `join` (support, the refinement's own
// measure: each exemplar's top-k mean cosine to the other's exemplars,
// averaged), unless they are apart or both named and named differently. Links
// are taken strongest first into groups by complete linkage: a group takes a
// person only when every member links to it, so a chain of lookalikes cannot
// walk. Each group lists its survivor first: named over unnamed, then the
// most faces, then the oldest id. Pairs whose cores are far apart (mean
// pairwise below join - 2 margins) are not scored.
[[nodiscard]] std::vector<std::vector<std::int64_t>> find_duplicates(const dedupe_input& in,
                                                                     const refine_params& p);

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
