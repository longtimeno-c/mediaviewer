// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// People refinement (plan/17 PR 24, "Refinement") on synthetic embeddings:
// the chaining case, the outlier case, the anchor case, weak faces,
// rejections, an incremental call, and convergence. Vectors are 128-d like
// SFace's: a person is a random unit direction, a face that direction plus
// Gaussian noise, which puts same-person pairs near cosine 0.55 and
// strangers near 0 (SFace's own genuine / impostor spread). The last two
// cases run the same rules through faces.db: snapshot, compute, commit.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "addons/ai/face_refine.h"
#include "addons/ai/faces.h"
#include "catch_compat.h"

namespace {

using mv::ai::refine_face;
using mv::ai::refine_input;
using mv::ai::refine_move;
using mv::ai::refine_output;
using mv::ai::refine_params;
using mv::ai::refine_why;

#ifndef MV_REFINE_SEED
#define MV_REFINE_SEED 1234  // -D another to sweep seeds
#endif
constexpr std::uint32_t kDim = 128;

struct world {
  std::mt19937 rng{MV_REFINE_SEED};
  std::vector<float> emb;
  std::vector<refine_face> faces;
  std::vector<std::int64_t> named;
  std::int64_t next_id = 1;

  static void normalise(std::vector<float>& v) {
    double n = 0;
    for (float x : v) n += static_cast<double>(x) * x;
    n = std::sqrt(n);
    for (float& x : v) x = static_cast<float>(x / n);
  }
  // Box-Muller on raw mt19937 words: std::normal_distribution's sequence is
  // implementation-defined, so libc++ and the MSVC STL would test different data.
  double uniform() { return (static_cast<double>(rng()) + 0.5) / 4294967296.0; }
  float gauss(float sigma) {
    const double r = std::sqrt(-2.0 * std::log(uniform()));
    return static_cast<float>(sigma * r * std::cos(6.283185307179586 * uniform()));
  }
  std::vector<float> direction() {
    std::vector<float> v(kDim);
    for (float& x : v) x = gauss(1);
    normalise(v);
    return v;
  }
  // A blend of two directions (a lookalike, or a face halfway between two).
  static std::vector<float> mix(const std::vector<float>& a, const std::vector<float>& b, float t) {
    std::vector<float> v(kDim);
    for (std::uint32_t i = 0; i < kDim; ++i) v[i] = (1 - t) * a[i] + t * b[i];
    normalise(v);
    return v;
  }
  std::int64_t face(const std::vector<float>& base, std::int64_t person, float noise = 0.075f,
                    float quality = 0.8f, bool pinned = false) {
    std::vector<float> v = base;
    for (float& x : v) x += gauss(noise);
    normalise(v);
    refine_face f;
    f.id = next_id++;
    f.person = person;
    f.quality = quality;
    f.pinned = pinned;
    f.row = static_cast<std::uint32_t>(emb.size() / kDim);
    emb.insert(emb.end(), v.begin(), v.end());
    faces.push_back(f);
    return f.id;
  }
  refine_face& at(std::int64_t id) { return faces[static_cast<std::size_t>(id - 1)]; }
  std::int64_t focus = 0;  // refine_input::focus: judge only this person's faces
  refine_output run(const refine_params& p = {}, bool regroup = true,
                    std::span<const mv::ai::person_proto> fixed = {}) {
    std::sort(named.begin(), named.end());
    refine_input in;
    in.dim = kDim;
    in.emb = emb;
    in.faces = faces;
    in.named = named;
    in.fixed = fixed;
    in.regroup = regroup;
    in.focus = focus;
    return mv::ai::refine_people(in, p);
  }
  // Applies the moves (new groups get ids from 1000 up), as faces_db would.
  void apply(const refine_output& out) {
    for (const refine_move& m : out.moves) {
      at(m.face).person = m.to < 0 ? 1000 - m.to : m.to;
    }
  }
};

const refine_move* move_of(const refine_output& out, std::int64_t face) {
  for (const refine_move& m : out.moves) {
    if (m.face == face) return &m;
  }
  return nullptr;
}

std::int64_t person_after(const refine_output& out, world& w, std::int64_t face) {
  const refine_move* m = move_of(out, face);
  return m ? m->to : w.at(face).person;
}

}  // namespace

TEST_CASE("refine: synthetic faces look like SFace's genuine / impostor spread", "[ai][faces][refine]") {
  world w;
  const auto a = w.direction();
  const auto b = w.direction();
  const std::int64_t f1 = w.face(a, 1), f2 = w.face(a, 1), g1 = w.face(b, 2);
  const auto cos = [&](std::int64_t x, std::int64_t y) {
    float s = 0;
    for (std::uint32_t i = 0; i < kDim; ++i) {
      s += w.emb[w.at(x).row * kDim + i] * w.emb[w.at(y).row * kDim + i];
    }
    return s;
  };
  CHECK(cos(f1, f2) > 0.40f);
  CHECK(cos(f1, f2) < 0.75f);
  CHECK(std::fabs(cos(f1, g1)) < 0.3f);
}

TEST_CASE("refine: two people chained into one come apart; the majority keeps the id",
          "[ai][faces][refine]") {
  world w;
  const auto anna = w.direction();
  const auto beth = w.direction();
  std::vector<std::int64_t> as, bs;
  for (int i = 0; i < 7; ++i) as.push_back(w.face(anna, 5));
  for (int i = 0; i < 4; ++i) bs.push_back(w.face(beth, 5));
  // The bridge that glued them: a weak crop halfway between the two.
  const std::int64_t bridge = w.face(world::mix(anna, beth, 0.5f), 5, 0.02f, 0.2f);

  const refine_output out = w.run();
  for (std::int64_t f : as) CHECK(person_after(out, w, f) == 5);
  std::set<std::int64_t> beth_to;
  for (std::int64_t f : bs) {
    const refine_move* m = move_of(out, f);
    REQUIRE(m);
    CHECK(m->from == 5);
    CHECK(m->why == refine_why::regroup);
    beth_to.insert(m->to);
  }
  CHECK(beth_to.size() == 1);        // one new person, all four of her
  CHECK(*beth_to.begin() < 0);
  CHECK(out.groups == 1);
  // The weak bridge is never a reference and never regrouped; whether it stays
  // with Anna depends only on how like her it is, and it never joins Beth.
  CHECK(person_after(out, w, bridge) != *beth_to.begin());
}

TEST_CASE("refine: an outlier leaves, and a face clearly someone else's moves to them",
          "[ai][faces][refine]") {
  world w;
  const auto anna = w.direction();
  const auto carl = w.direction();
  const auto stranger = w.direction();
  for (int i = 0; i < 8; ++i) w.face(anna, 1);
  for (int i = 0; i < 5; ++i) w.face(carl, 2);
  const std::int64_t carl_in_anna = w.face(carl, 1);
  const std::int64_t nobody_in_anna = w.face(stranger, 1);

  const refine_output out = w.run();
  const refine_move* m = move_of(out, carl_in_anna);
  REQUIRE(m);
  CHECK(m->why == refine_why::move);
  CHECK(m->to == 2);
  CHECK(m->best >= refine_params{}.join);
  const refine_move* e = move_of(out, nobody_in_anna);
  REQUIRE(e);
  CHECK(e->why == refine_why::evict);
  CHECK(e->to == 0);  // unassigned, never a wrong name
  CHECK(out.moves.size() == 2);
}

TEST_CASE("refine: a focused call judges only that person's faces", "[ai][faces][refine]") {
  world w;
  const auto anna = w.direction();
  const auto carl = w.direction();
  const auto stranger = w.direction();
  const auto dora = w.direction();
  for (int i = 0; i < 8; ++i) w.face(anna, 1);
  for (int i = 0; i < 8; ++i) w.face(carl, 2);
  const std::int64_t carl_in_anna = w.face(carl, 1);
  const std::int64_t nobody_in_anna = w.face(stranger, 1);
  // The same mistakes the other way round, under Carl: left alone.
  const std::int64_t anna_in_carl = w.face(anna, 2);
  const std::int64_t nobody_in_carl = w.face(stranger, 2);
  // Unassigned faces that would regroup (and one Anna would admit): untouched.
  for (int i = 0; i < 3; ++i) w.face(dora, 0);
  const std::int64_t loose_anna = w.face(anna, 0);

  w.focus = 1;
  const refine_output out = w.run();
  const refine_move* m = move_of(out, carl_in_anna);
  REQUIRE(m);
  CHECK(m->to == 2);
  const refine_move* e = move_of(out, nobody_in_anna);
  REQUIRE(e);
  CHECK(e->to == 0);
  CHECK_FALSE(move_of(out, anna_in_carl));
  CHECK_FALSE(move_of(out, nobody_in_carl));
  CHECK_FALSE(move_of(out, loose_anna));
  CHECK(out.groups == 0);
  CHECK(out.moves.size() == 2);
  for (const refine_move& mv : out.moves) CHECK(mv.from == 1);

  // Unfocused, the same library corrects Carl too.
  w.focus = 0;
  const refine_output all = w.run();
  CHECK(move_of(all, anna_in_carl));
  CHECK(move_of(all, nobody_in_carl));
}

TEST_CASE("refine: pinned faces are anchors that never move, and define a named person",
          "[ai][faces][refine]") {
  world w;
  const auto anna = w.direction();
  const auto beth = w.direction();
  const auto profile = w.direction();
  // Named Anna: three faces the user confirmed, two the clustering added,
  // and six of Beth glued on: the impostors are the majority.
  std::vector<std::int64_t> pins, anna_auto, beth_auto;
  for (int i = 0; i < 3; ++i) pins.push_back(w.face(anna, 7, 0.075f, 0.8f, true));
  for (int i = 0; i < 2; ++i) anna_auto.push_back(w.face(anna, 7));
  for (int i = 0; i < 6; ++i) beth_auto.push_back(w.face(beth, 7));
  // A shot the user moved into Anna that looks like nobody in her: stays.
  const std::int64_t odd_pin = w.face(profile, 7, 0.075f, 0.8f, true);
  w.named = {7};

  const refine_output out = w.run();
  for (std::int64_t f : pins) CHECK_FALSE(move_of(out, f));
  CHECK_FALSE(move_of(out, odd_pin));
  for (std::int64_t f : anna_auto) CHECK(person_after(out, w, f) == 7);
  for (std::int64_t f : beth_auto) {
    const refine_move* m = move_of(out, f);
    REQUIRE(m);
    CHECK(m->to < 0);  // out of Anna, into a new unnamed person
  }
}

TEST_CASE("refine: a rejected person is never joined; a weak face is never moved",
          "[ai][faces][refine]") {
  world w;
  const auto anna = w.direction();
  const auto ben = w.direction();
  for (int i = 0; i < 6; ++i) w.face(anna, 1);
  for (int i = 0; i < 6; ++i) w.face(ben, 2);
  // Ben's face in Anna, but the user said "not Ben" about it once.
  const std::int64_t said_not_ben = w.face(ben, 1);
  w.at(said_not_ben).rejected = {2};
  // A blurry Ben in Anna: it may leave Anna, but a weak vector never files it under Ben.
  const std::int64_t blurry = w.face(ben, 1, 0.075f, 0.1f);
  // An unassigned clear Ben is admitted.
  const std::int64_t loose = w.face(ben, 0);

  const refine_output out = w.run();
  CHECK(person_after(out, w, said_not_ben) != 2);
  CHECK(person_after(out, w, said_not_ben) == 0);  // not like Anna either: unassigned
  CHECK(person_after(out, w, blurry) != 2);
  const refine_move* m = move_of(out, loose);
  REQUIRE(m);
  CHECK(m->why == refine_why::admit);
  CHECK(m->to == 2);
}

TEST_CASE("refine: an ambiguous face between two lookalikes stays put", "[ai][faces][refine]") {
  world w;
  const auto a = w.direction();
  const auto twin = world::mix(a, w.direction(), 0.35f);  // a sibling: cosine ~0.8 to a
  for (int i = 0; i < 6; ++i) w.face(a, 1);
  for (int i = 0; i < 6; ++i) w.face(twin, 2);
  const std::int64_t between = w.face(world::mix(a, twin, 0.5f), 1, 0.02f);  // truly halfway
  const refine_output out = w.run();
  // Close to both: no margin, so no move; like its own person: no eviction.
  CHECK(person_after(out, w, between) == 1);
}

TEST_CASE("refine: an incremental call against cached prototypes; and a second call changes nothing",
          "[ai][faces][refine]") {
  world w;
  const auto anna = w.direction();
  const auto carl = w.direction();
  const auto beth = w.direction();
  for (int i = 0; i < 8; ++i) w.face(anna, 1);
  for (int i = 0; i < 3; ++i) w.face(beth, 1);  // chained into Anna
  for (int i = 0; i < 5; ++i) w.face(carl, 2);
  w.face(carl, 1);                               // Carl filed under Anna

  refine_output first = w.run();
  CHECK(first.moves.size() == 4);
  w.apply(first);
  // Converged: the same call on its own result moves nothing (no ping-pong).
  const refine_output again = w.run();
  CHECK(again.moves.empty());
  CHECK(again.passes == 1);

  // New faces land in a fresh person; only it is rebuilt, the rest are cached.
  const std::vector<mv::ai::person_proto> cached = first.protos;
  world inc;
  inc.rng.seed(99);
  inc.emb = w.emb;
  inc.next_id = w.next_id;
  const std::int64_t late_carl = inc.face(carl, 50);
  inc.faces = {inc.faces.back()};
  refine_input in;
  in.dim = kDim;
  in.emb = inc.emb;
  in.faces = inc.faces;
  in.fixed = cached;
  const refine_output out = mv::ai::refine_people(in, refine_params{});
  const refine_move* m = move_of(out, late_carl);
  REQUIRE(m);
  CHECK(m->to == 2);
  // Carl was cached, not given: no prototype of his one new face comes back.
  for (const mv::ai::person_proto& pr : out.protos) CHECK(pr.person != 2);
}

TEST_CASE("refine: quality from score, size, sharpness and pose", "[ai][faces][refine]") {
  CHECK(mv::ai::face_quality(0.95f, 160, 400, 1) > 0.9f);
  CHECK(mv::ai::face_quality(0.95f, 30, 400, 1) == 0.0f);  // too small to trust
  CHECK(mv::ai::face_quality(0.95f, 160, 10, 1) == 0.0f);  // blurred
  CHECK(mv::ai::face_quality(0.95f, 160, 400, 0.1f) < 0.35f);  // profile
  const float frontal[10] = {40, 50, 72, 50, 56, 70, 42, 90, 70, 90};
  const float turned[10] = {40, 50, 72, 50, 74, 70, 42, 90, 70, 90};
  CHECK(mv::ai::landmark_frontalness(std::span<const float, 10>(frontal)) > 0.9f);
  CHECK(mv::ai::landmark_frontalness(std::span<const float, 10>(turned)) < 0.1f);
  // A flat crop has no edges; a checkerboard is all edges.
  std::vector<float> flat(3 * 16 * 16, 128.0f), check(3 * 16 * 16);
  for (std::size_t i = 0; i < check.size(); ++i) check[i] = ((i % 16 + (i / 16) % 16) % 2) ? 255.0f : 0.0f;
  CHECK(mv::ai::crop_sharpness(flat, 16) < 1e-3f);
  CHECK(mv::ai::crop_sharpness(check, 16) > 1000.0f);
}

// ---- through faces.db ------------------------------------------------------------

namespace {

std::string temp_db(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() / "mv_face_refine_tests";
  std::filesystem::create_directories(dir);
  const std::string p = (dir / name).string();
  mv::ai::faces_db::destroy(p);
  return p;
}

mv::ai::face_in face_in_of(world& w, const std::vector<float>& base, float score, float x) {
  const std::int64_t id = w.face(base, 0);
  mv::ai::face_in f;
  f.x = x;
  f.y = 0.2f;
  f.w = 0.2f;
  f.h = 0.3f;
  f.score = score;
  f.quality = 0.8f;
  const float* v = w.emb.data() + std::size_t{w.at(id).row} * kDim;
  f.emb.assign(v, v + kDim);
  return f;
}

refine_input input_of(const mv::ai::refine_snapshot& snap) {
  refine_input in;
  in.dim = kDim;
  in.emb = snap.emb;
  in.faces = snap.faces;
  in.fixed = snap.fixed;
  in.named = snap.named;
  in.regroup = true;
  return in;
}

}  // namespace

TEST_CASE("refine through faces.db: a loosely clustered library comes apart, the user's faces stay",
          "[ai][faces][refine]") {
  const std::string path = temp_db("loose.db");
  world w;
  const auto anna = w.direction();
  const auto beth = w.direction();
  {
    // The old looseness, exaggerated: a threshold that lets anyone join.
    auto db = mv::ai::faces_db::open(path, -1.0f, kDim);
    REQUIRE(db);
    for (int i = 0; i < 6; ++i) {
      const mv::ai::face_in f = face_in_of(w, anna, 0.99f, 0.1f);
      REQUIRE((*db)->add(100 + i, "a", -1, std::span<const mv::ai::face_in>(&f, 1)));
    }
    for (int i = 0; i < 4; ++i) {
      const mv::ai::face_in f = face_in_of(w, beth, 0.85f, 0.1f);
      REQUIRE((*db)->add(200 + i, "b", -1, std::span<const mv::ai::face_in>(&f, 1)));
    }
    const auto people = (*db)->people(1);
    REQUIRE(people.size() == 1);                   // all ten under one person
    REQUIRE((*db)->rename(people[0].id, "Anna"));  // its cover, an Anna face, is pinned
  }
  auto db = mv::ai::faces_db::open(path, 0.40f, kDim);
  REQUIRE(db);
  CHECK((*db)->refine_due());
  CHECK((*db)->refine_full_due());  // nothing cached after an open
  {
    const mv::ai::refine_snapshot snap = (*db)->refine_begin(false);
    CHECK(snap.full);
    const mv::ai::refine_stats st = (*db)->refine_commit(snap, mv::ai::refine_people(input_of(snap), {}));
    CHECK(st.regrouped == 4);
    CHECK(st.groups == 1);
  }
  const auto people = (*db)->people(1);
  REQUIRE(people.size() == 2);
  CHECK(people[0].name == "Anna");
  CHECK(people[0].faces == 6);
  CHECK(people[0].cover.pinned);
  CHECK(people[1].name.empty());
  CHECK(people[1].faces == 4);
  // The new person is rebuilt once more, then all is settled.
  if ((*db)->refine_due()) {
    const mv::ai::refine_snapshot snap = (*db)->refine_begin(false);
    const mv::ai::refine_stats st = (*db)->refine_commit(snap, mv::ai::refine_people(input_of(snap), {}));
    CHECK_FALSE(st.changed());
  }
  CHECK_FALSE((*db)->refine_due());

  // The user acts between a snapshot and its commit: the user wins.
  const auto beths = (*db)->faces_of(people[1].id);
  REQUIRE(beths.size() == 4);
  const mv::ai::refine_snapshot snap = (*db)->refine_begin(true);
  const std::vector<std::int64_t> pick{beths[0].id};
  auto fresh = (*db)->split(pick);
  REQUIRE(fresh);
  // A synthetic verdict that would evict that very face.
  refine_output out;
  out.moves.push_back(refine_move{beths[0].id, people[1].id, 0, refine_why::evict, 0.1f, -1});
  const mv::ai::refine_stats st = (*db)->refine_commit(snap, out);
  CHECK(st.skipped == 1);
  CHECK(st.evicted == 0);
  auto kept = (*db)->face(beths[0].id);
  REQUIRE(kept);
  CHECK(kept->person == *fresh);
  CHECK(kept->pinned);
}

TEST_CASE("refine through faces.db: a re-analysis replaces a face's vector in place",
          "[ai][faces][refine]") {
  const std::string path = temp_db("rescan.db");
  world w;
  const auto anna = w.direction();
  auto db = mv::ai::faces_db::open(path, 0.40f, kDim);
  REQUIRE(db);
  for (int i = 0; i < 3; ++i) {
    const mv::ai::face_in f = face_in_of(w, anna, 0.95f, 0.1f);
    REQUIRE((*db)->add(10 + i, "a", -1, std::span<const mv::ai::face_in>(&f, 1)));
  }
  const auto before = (*db)->people(1);
  REQUIRE(before.size() == 1);
  REQUIRE((*db)->faces_of(before[0].id).size() == 3);
  REQUIRE((*db)->mark_scanned(10, "spec"));
  CHECK((*db)->scanned(10, "spec"));
  REQUIRE((*db)->rescan(10, "spec"));
  CHECK_FALSE((*db)->scanned(10, "spec"));
  mv::ai::face_in again = face_in_of(w, anna, 0.95f, 0.11f);  // the same box, nudged
  again.tta = true;
  REQUIRE((*db)->add(10, "a", -1, std::span<const mv::ai::face_in>(&again, 1)));
  CHECK((*db)->face_count() == 3);
  CHECK((*db)->faces_of(before[0].id).size() == 3);
}
