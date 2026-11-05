// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Restore faces (docs/plans/edit-refresh.md S10, owner call C): each
// face of a photo restored by a face model (RestoreFormer++ by default, GFPGAN
// v1.4 for a more natural look), guarded by People so it stays the same person.
//
//   1. The faces and their five landmarks from the AI pack's People model
//      (the host table's still_faces): on the whole photo and, for a large
//      one, on overlapping tiles so a small face in a group is found too.
//   2. Each face aligned to the FFHQ 5-point template at the model's side
//      (512) by a similarity fitted to its landmarks, then refined once from
//      the landmarks the pack finds on that aligned crop.
//   3. The model restores the crop.
//   4. The guard: the restored crop, blended as it will be pasted, against the
//      original crop through People's embedding (still_faces with `other`). A
//      face whose cosine is under the guard keeps its original pixels.
//   5. The restored face pasted back through the inverse warp under a
//      feathered elliptical face mask, at Strength. A face larger in the photo
//      than the model's side keeps its own detail: only the model's change is
//      carried (restored minus the aligned original), never its lower
//      resolution.
//
// Every pixel outside the faces' masks is the photo's own. The model is not
// here (face_restorer), so the steps are tested without it. The runner's
// thread only (rule 1).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/enhance/diffusion.h"
#include "addons/enhance/remove.h"
#include "core/result.h"

namespace mv::infer {
class runtime;
}

namespace mv::enhance {

// The FFHQ five points at 512 (facexlib's template, the alignment GFPGAN and
// RestoreFormer++ were trained on): the eye on the picture's left, the other
// eye, the nose tip, the mouth corner on the left, the other corner (x, y).
// The pack's landmarks come in the same order (right eye, left eye, nose,
// mouth corners: the person's right is the picture's left).
inline constexpr std::array<float, 10> kFfhq512{192.98138f, 239.94708f, 318.90277f, 240.1936f,  256.63416f,
                                                314.01935f, 201.26117f, 371.41043f, 313.08905f, 371.15118f};

// The People identity guard (docs/design/29 "Restore faces"): the least cosine,
// People's AdaFace IR-50 embedding, between a face and its restored, blended
// self. Measured on LFW (tools/enhance/face_guard.py, 2026-11-05, 240 faces,
// half degraded): different people reach 0.384 at a false-accept rate of 1e-5
// (0.678 at 1e-6); another person's restored face blended in at 0.7 reached
// 0.322 at most; the lowest of 960 real restorations (both models, Strength
// 0.7 and 1) was 0.685 (GFPGAN at 1), 0.726 at 0.7. 0.55 sits between: no
// real restoration of the 960 was kept, no other-person blend passed.
inline constexpr float kFaceGuard = 0.55f;

// A face with less than this between the eyes (photo pixels) is left alone:
// too little of it for a model to restore rather than invent.
inline constexpr float kMinEyeDistance = 10.0f;

// x' = a x - b y + tx, y' = b x + a y + ty: rotation, uniform scale and a shift.
struct similarity {
  double a = 1, b = 0, tx = 0, ty = 0;
  void apply(double x, double y, double& ox, double& oy) const noexcept {
    ox = a * x - b * y + tx;
    oy = b * x + a * y + ty;
  }
  [[nodiscard]] double scale() const noexcept;  // how much it enlarges
  [[nodiscard]] similarity inverse() const noexcept;
};

// The least-squares similarity taking `from` to `to` (five x, y points each),
// never a reflection. Identity when the points are degenerate.
[[nodiscard]] similarity fit_similarity(std::span<const float, 10> from, std::span<const float, 10> to) noexcept;

// The width x height picture under `to_out` (photo -> out): each output pixel
// the photo at the inverse point, bilinear, averaged over a grid of up to 8 x
// 8 samples where the output is smaller than the photo there (no aliasing).
// Outside the photo, its nearest edge pixel.
[[nodiscard]] rgb8 warp_rgb(const rgb8& photo, const similarity& to_out, std::uint32_t width, std::uint32_t height);

// The feathered face mask in the aligned frame, side x side, 0..1: an
// ellipse round the FFHQ face (forehead to chin, cheek to cheek) at 1 inside
// 75 % of its radius, falling smoothly to 0 at its rim, 0 at the crop's edge.
[[nodiscard]] std::vector<float> face_mask(std::uint32_t side);

// aligned + strength * mask * (restored - aligned), in the aligned frame: the
// face as it will be pasted (what the guard compares).
[[nodiscard]] rgb8 blend_face(const rgb8& aligned, const rgb8& restored, const std::vector<float>& mask,
                              float strength);

// The restored face back into `photo` through the inverse of `to_face`, each
// photo pixel weighted by the mask there times `strength`. Where the face is
// larger in the photo than in the crop (to_face.scale() < 1), only the change
// (restored - aligned) is added, so the photo keeps its own detail; else the
// restored face is sampled with the same averaging as warp_rgb. Pixels where
// the mask is 0 are not touched. Returns the box of photo pixels it may have
// changed (width 0 when none).
pixel_box paste_face(rgb8& photo, const similarity& to_face, const rgb8& aligned, const rgb8& restored,
                     const std::vector<float>& mask, float strength);

// One face of a still_faces answer, in pixels of the picture it was asked of.
struct face_found {
  float x = 0, y = 0, w = 0, h = 0;
  float score = 0;
  std::array<float, 10> lm{};
  float sim = -1;  // with `other`: the cosine to the same face changed (-1 none)
};
// The faces of the pack's JSON, its 0..1 boxes and landmarks scaled to a
// width x height picture. Faces without five landmarks are left out.
[[nodiscard]] result<std::vector<face_found>> parse_faces(std::string_view json, std::uint32_t width,
                                                         std::uint32_t height);

// What restores a face (a stand-in in the tests): a side x side crop in, the
// same out.
class face_restorer {
 public:
  virtual ~face_restorer() = default;
  [[nodiscard]] virtual std::uint32_t side() const noexcept = 0;
  [[nodiscard]] virtual result<rgb8> restore(const rgb8& face) = 0;
};

// A face model's tensors, from its io entry (models/face/model.json): one RGB
// input scaled to -1..1, NCHW at `side`, and the output by name (or the
// first), clipped to -1..1.
struct face_io {
  std::uint32_t side = 512;
  std::string file;
  std::string input = "input";
  std::string output;  // "" the first output
};

class face_model final : public face_restorer {
 public:
  ~face_model() override;
  // CPU and fp32 (the plan's quality call), `threads` intra-op.
  [[nodiscard]] static result<std::unique_ptr<face_model>> open(infer::runtime& rt, const std::string& file,
                                                                const face_io& io, int threads);
  [[nodiscard]] std::uint32_t side() const noexcept override;
  [[nodiscard]] result<rgb8> restore(const rgb8& face) override;

 private:
  struct impl;
  explicit face_model(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

// The pack, as Restore faces asks it: the faces in `img`, and with `other`
// (the same size) each one's cosine to the same face changed.
using faces_fn = std::function<result<std::vector<face_found>>(const rgb8& img, const rgb8* other)>;

struct face_options {
  float strength = 0.7f;
  float guard = kFaceGuard;
  // (done, total) faces; false cancels.
  std::function<bool(std::size_t, std::size_t)> progress;
};

struct face_outcome {
  int found = 0;     // faces the pack found
  int restored = 0;  // pasted back
  int kept = 0;      // the guard kept the original (or the pack lost the face)
  int small = 0;     // too small to restore
  std::vector<pixel_box> changed;  // the photo pixels each pasted face may have changed
  std::vector<float> sims;         // the guard's cosine per face it measured
};

// The faces where the pack finds them: on the photo (at most kFindEdge on its
// long edge) and, from 2 * kFindTile on its short edge, on tiles that
// overlap by half; a face found twice is kept once (the larger box).
inline constexpr std::uint32_t kFindEdge = 2048;
inline constexpr std::uint32_t kFindTile = 1024;
[[nodiscard]] result<std::vector<face_found>> find_faces(const rgb8& photo, const faces_fn& faces);

// Steps 2-5 for every face of `found` (find_faces), in place. status::cancelled
// when `progress` says so; the photo may then be partly restored (the runner
// writes nothing).
[[nodiscard]] result<face_outcome> restore_faces(rgb8& photo, const std::vector<face_found>& found,
                                                 face_restorer& model, const faces_fn& faces,
                                                 const face_options& options);

}  // namespace mv::enhance
