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
#include <cstdlib>
#include <cstdio>
#include <chrono>
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

// ---- duplicates (plan/17 "Merge duplicates") ----------------------------------------

namespace {

using mv::ai::dedupe_input;
using mv::ai::dedupe_person;
using mv::ai::find_duplicates;
using groups_t = std::vector<std::vector<std::int64_t>>;

groups_t duplicates(const refine_output& out, std::span<const dedupe_person> people = {},
                    std::vector<std::pair<std::int64_t, std::int64_t>> apart = {}) {
  std::sort(apart.begin(), apart.end());
  dedupe_input in;
  in.dim = kDim;
  in.protos = out.protos;
  in.people = people;
  in.apart = apart;
  return find_duplicates(in, {});
}

bool together(const groups_t& g, std::int64_t a, std::int64_t b) {
  for (const auto& members : g) {
    if (std::find(members.begin(), members.end(), a) != members.end() &&
        std::find(members.begin(), members.end(), b) != members.end()) {
      return true;
    }
  }
  return false;
}

// Mean pairwise cosine across two persons' faces: what the idle consolidate
// compares (and merges at 0.42).
float across(world& w, std::int64_t a, std::int64_t b) {
  double sum = 0;
  std::size_t n = 0;
  for (const refine_face& x : w.faces) {
    if (x.person != a) continue;
    for (const refine_face& y : w.faces) {
      if (y.person != b) continue;
      const float* vx = w.emb.data() + std::size_t{x.row} * kDim;
      const float* vy = w.emb.data() + std::size_t{y.row} * kDim;
      double d = 0;
      for (std::uint32_t i = 0; i < kDim; ++i) d += static_cast<double>(vx[i]) * vy[i];
      sum += d;
      ++n;
    }
  }
  return n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
}

}  // namespace

TEST_CASE("dedupe: a duplicate the cluster average misses is found; a stranger is not",
          "[ai][faces][refine][dedupe]") {
  world w;
  const auto sam = w.direction();
  const auto zoe = w.direction();
  // Person 1: Sam, six clear faces and twelve weak ones (blurred, small,
  // turned) that drag the cluster's average down. Person 2: Sam again, six
  // clear faces. Person 3: Zoe.
  for (int i = 0; i < 6; ++i) w.face(sam, 1);
  for (int i = 0; i < 12; ++i) w.face(sam, 1, 0.2f, 0.2f);
  for (int i = 0; i < 6; ++i) w.face(sam, 2);
  for (int i = 0; i < 6; ++i) w.face(zoe, 3);
  // The idle consolidate's view: below its 0.42, so it never merged them.
  CHECK(across(w, 1, 2) < 0.42f);
  const refine_output out = w.run();
  const groups_t g = duplicates(out);
  REQUIRE(g.size() == 1);
  CHECK(together(g, 1, 2));
  CHECK_FALSE(together(g, 1, 3));
  CHECK_FALSE(together(g, 2, 3));
  // Survivor first: the one with more faces (both unnamed).
  const std::vector<dedupe_person> people{{1, "", 18}, {2, "", 6}, {3, "", 6}};
  CHECK(duplicates(out, people).front().front() == 1);
}

TEST_CASE("dedupe: names and the user's splits rule who merges and who survives",
          "[ai][faces][refine][dedupe]") {
  world w;
  const auto sam = w.direction();
  for (int i = 0; i < 8; ++i) w.face(sam, 1);
  for (int i = 0; i < 4; ++i) w.face(sam, 2);
  for (int i = 0; i < 3; ++i) w.face(sam, 4);
  const refine_output out = w.run();

  // Unnamed: one group, the largest survives.
  std::vector<dedupe_person> people{{1, "", 8}, {2, "", 4}, {4, "", 3}};
  groups_t g = duplicates(out, people);
  REQUIRE(g.size() == 1);
  CHECK(g[0] == std::vector<std::int64_t>{1, 2, 4});
  // A named person survives over a larger unnamed one.
  people[1].name = "Sam";
  g = duplicates(out, people);
  REQUIRE(g.size() == 1);
  CHECK(g[0].front() == 2);
  // The same name twice is one person: the larger named one survives.
  people[0].name = "Sam";
  g = duplicates(out, people);
  REQUIRE(g.size() == 1);
  CHECK(g[0].front() == 1);
  // Named differently: never together, whatever the faces say.
  people[1].name = "Sasha";
  g = duplicates(out, people);
  CHECK_FALSE(together(g, 1, 2));
  // A split kept apart: never together.
  people[1].name.clear();
  people[0].name.clear();
  g = duplicates(out, people, {{1, 2}});
  CHECK_FALSE(together(g, 1, 2));
  CHECK((together(g, 1, 4) || together(g, 2, 4)));
}

TEST_CASE("dedupe: a lookalike chain does not walk", "[ai][faces][refine][dedupe]") {
  world w;
  const auto a = w.direction();
  const auto c = w.direction();
  const auto b = world::mix(a, c, 0.5f);  // like each, the ends unlike each other
  for (int i = 0; i < 6; ++i) w.face(a, 1);
  for (int i = 0; i < 6; ++i) w.face(b, 2);
  for (int i = 0; i < 6; ++i) w.face(c, 3);
  const groups_t g = duplicates(w.run());
  CHECK_FALSE(together(g, 1, 3));
  for (const auto& members : g) CHECK(members.size() <= 2);
}

TEST_CASE("dedupe through faces.db: merge_auto pins nothing and leaves a split pair alone",
          "[ai][faces][refine][dedupe]") {
  const std::string path = temp_db("dedupe.db");
  world w;
  const auto anna = w.direction();
  const auto beth = w.direction();
  auto db = mv::ai::faces_db::open(path, 0.40f, kDim);
  REQUIRE(db);
  for (int i = 0; i < 4; ++i) {
    const mv::ai::face_in f = face_in_of(w, anna, 0.95f, 0.1f);
    REQUIRE((*db)->add(100 + i, "a", -1, std::span<const mv::ai::face_in>(&f, 1)));
  }
  for (int i = 0; i < 4; ++i) {
    const mv::ai::face_in f = face_in_of(w, beth, 0.95f, 0.1f);
    REQUIRE((*db)->add(200 + i, "b", -1, std::span<const mv::ai::face_in>(&f, 1)));
  }
  auto people = (*db)->people(1);
  REQUIRE(people.size() == 2);
  const std::int64_t first = people[0].id, second = people[1].id;
  // merge_auto merges what it is told (the engine decides who) and pins nothing.
  auto merged = (*db)->merge_auto(first, second);
  REQUIRE(merged);
  CHECK(merged.value());
  people = (*db)->people(1);
  REQUIRE(people.size() == 1);
  CHECK(people[0].faces == 8);
  for (const auto& f : (*db)->faces_of(first)) CHECK_FALSE(f.pinned);
  CHECK((*db)->merge_blocks().empty());
  // A split keeps the two apart: listed, and refused.
  const auto faces = (*db)->faces_of(first);
  const std::vector<std::int64_t> pick{faces[0].id};
  auto fresh = (*db)->split(pick);
  REQUIRE(fresh);
  const auto blocks = (*db)->merge_blocks();
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0] == std::make_pair(std::min(first, *fresh), std::max(first, *fresh)));
  merged = (*db)->merge_auto(first, *fresh);
  REQUIRE(merged);
  CHECK_FALSE(merged.value());
  CHECK((*db)->people(1).size() == 2);
}

TEST_CASE("a new face model inherits the user's people through a re-run", "[ai][faces][refine][rerun]") {
  const std::string path = temp_db("rerun.db");
  world w;
  // The old embedder's space, and the new one's: unrelated directions, as two
  // models' vectors are. Same width, so only the spec keeps them apart.
  const auto anna_old = w.direction();
  const auto ben_old = w.direction();
  const auto anna_new = w.direction();
  const auto ben_new = w.direction();
  mv::ai::face_tuning loose;
  loose.same_person = -1.0f;  // the old model glued Ben onto Anna
  std::int64_t anna = 0;
  {
    auto db = mv::ai::faces_db::open(path, loose, kDim, "old/1");
    REQUIRE(db);
    CHECK_FALSE((*db)->rerun_pending());
    for (int i = 0; i < 6; ++i) {
      const mv::ai::face_in f = face_in_of(w, anna_old, 0.99f, 0.1f);
      REQUIRE((*db)->add(100 + i, "a", -1, std::span<const mv::ai::face_in>(&f, 1)));
      REQUIRE((*db)->mark_scanned(100 + i, "old/1"));
    }
    for (int i = 0; i < 4; ++i) {
      const mv::ai::face_in f = face_in_of(w, ben_old, 0.85f, 0.1f);
      REQUIRE((*db)->add(200 + i, "b", -1, std::span<const mv::ai::face_in>(&f, 1)));
      REQUIRE((*db)->mark_scanned(200 + i, "old/1"));
    }
    const auto people = (*db)->people(1);
    REQUIRE(people.size() == 1);
    anna = people[0].id;
    REQUIRE((*db)->rename(anna, "Anna"));  // its cover, an Anna face, is pinned
  }

  // A pack update brings the new embedder: a re-run is due by itself, and
  // until an asset is re-analysed its faces still show their person but are
  // compared with nothing.
  auto db = mv::ai::faces_db::open(path, mv::ai::face_tuning{}, kDim, "new/2");
  REQUIRE(db);
  CHECK((*db)->rerun_pending());
  CHECK((*db)->stale_count() == 10);
  CHECK((*db)->scanned_assets("new/2").empty());
  REQUIRE((*db)->people(1).size() == 1);
  CHECK((*db)->people(1)[0].faces == 10);
  {
    const mv::ai::face_in probe = face_in_of(w, anna_new, 0.99f, 0.1f);
    CHECK((*db)->nearest_person(probe.emb) == 0);  // no new-space vector yet
  }

  // The re-run: each asset analysed again. The same box keeps its row,
  // person and pin; a new face (another box) waits; a face the new pass does
  // not find again goes when the asset is marked done.
  for (int i = 0; i < 6; ++i) {
    const mv::ai::face_in f = face_in_of(w, anna_new, 0.99f, 0.1f);
    REQUIRE((*db)->add(100 + i, "a", -1, std::span<const mv::ai::face_in>(&f, 1)));
    REQUIRE((*db)->mark_scanned(100 + i, "new/2"));
  }
  for (int i = 0; i < 3; ++i) {
    std::vector<mv::ai::face_in> found{face_in_of(w, ben_new, 0.85f, 0.1f)};
    if (i == 0) found.push_back(face_in_of(w, ben_new, 0.9f, 0.6f));  // a second face, new here
    REQUIRE((*db)->add(200 + i, "b", -1, found));
    REQUIRE((*db)->mark_scanned(200 + i, "new/2"));
  }
  {
    // Asset 203: nothing found this time, so its old face goes with it.
    REQUIRE((*db)->mark_scanned(203, "new/2"));
  }
  CHECK((*db)->stale_count() == 0);
  CHECK((*db)->face_count() == 10);           // 6 Anna, 3 Ben in place, 1 new Ben
  CHECK((*db)->unassigned_count() == 1);      // the new one waits for the settle
  REQUIRE((*db)->people(1).size() == 1);
  CHECK((*db)->people(1)[0].faces == 9);

  // The settle, as engine::settle_people runs it: a full refinement with no
  // focus, then rerun_done.
  const mv::ai::refine_snapshot snap = (*db)->refine_begin(true);
  CHECK(snap.faces.size() == 10);
  const mv::ai::refine_stats st =
      (*db)->refine_commit(snap, mv::ai::refine_people(input_of(snap), mv::ai::face_tuning{}.refine()));
  CHECK(st.changed());
  (*db)->rerun_done();
  CHECK_FALSE((*db)->rerun_pending());

  // Anna keeps her name and her pinned cover; Ben's faces, glued on by the
  // old model, come out together with his new face as one person.
  const auto people = (*db)->people(1);
  REQUIRE(people.size() == 2);
  CHECK(people[0].id == anna);
  CHECK(people[0].name == "Anna");
  CHECK(people[0].faces == 6);
  CHECK(people[0].cover.pinned);
  CHECK(people[1].name.empty());
  CHECK(people[1].faces == 4);
  CHECK((*db)->unassigned_count() == 0);

  // Reopening with the same model: nothing more to re-run.
  db->reset();
  auto again = mv::ai::faces_db::open(path, mv::ai::face_tuning{}, kDim, "new/2");
  REQUIRE(again);
  CHECK_FALSE((*again)->rerun_pending());
}

TEST_CASE("re-analysing with the same model keeps every person and correction", "[ai][faces][refine][rerun]") {
  const std::string path = temp_db("rerun_same.db");
  world w;
  const auto anna = w.direction();
  const auto ben = w.direction();
  auto db = mv::ai::faces_db::open(path, mv::ai::face_tuning{}, kDim, "m/1");
  REQUIRE(db);
  for (int i = 0; i < 4; ++i) {
    const mv::ai::face_in a = face_in_of(w, anna, 0.95f, 0.1f);
    const mv::ai::face_in b = face_in_of(w, ben, 0.95f, 0.6f);
    const std::vector<mv::ai::face_in> both{a, b};
    REQUIRE((*db)->add(10 + i, "p", -1, both));
    REQUIRE((*db)->mark_scanned(10 + i, "m/1"));
  }
  auto people = (*db)->people(1);
  REQUIRE(people.size() == 2);
  REQUIRE((*db)->rename(people[0].id, "Anna"));
  REQUIRE((*db)->rename(people[1].id, "Ben"));

  REQUIRE((*db)->rerun_all());
  CHECK((*db)->rerun_pending());
  CHECK((*db)->scanned_assets("m/1").empty());
  for (int i = 0; i < 4; ++i) {
    const std::vector<mv::ai::face_in> both{face_in_of(w, anna, 0.95f, 0.1f), face_in_of(w, ben, 0.95f, 0.6f)};
    REQUIRE((*db)->add(10 + i, "p", -1, both));
    REQUIRE((*db)->mark_scanned(10 + i, "m/1"));
  }
  CHECK((*db)->face_count() == 8);
  CHECK((*db)->unassigned_count() == 0);
  const mv::ai::refine_snapshot snap = (*db)->refine_begin(true);
  const mv::ai::refine_stats st = (*db)->refine_commit(snap, mv::ai::refine_people(input_of(snap), {}));
  CHECK_FALSE(st.changed());
  (*db)->rerun_done();
  people = (*db)->people(1);
  REQUIRE(people.size() == 2);
  CHECK(people[0].faces == 4);
  CHECK(people[1].faces == 4);
  CHECK(((people[0].name == "Anna" && people[1].name == "Ben") || (people[0].name == "Ben" && people[1].name == "Anna")));
}

// ---- the People bench (plan/17 "People model") ------------------------------------------
//
// mv_ai_tests "[.people-bench]" with MV_FACE_EVAL=<file>: real face vectors
// (tools: an LFW export, uint32 n, uint32 dim, n int32 identity labels, n x dim
// float32 L2-normalised rows) through faces.db exactly as the engine files
// them: online add in a shuffled order, then the settle (a full refinement
// with no focus, up to three calls) and the merge. BCubed precision / recall
// of the people against the labels, before and after the settle. Thresholds:
// MV_FACE_TUNING="same keep keep_weak margin ambiguous merge_at" (SFace's
// defaults otherwise). Prints; asserts nothing about the numbers.

namespace {

struct bcubed_score {
  double p = 0, r = 0, f = 0;
  std::size_t people = 0, unfiled = 0;
};

// Unfiled faces each count as a person of their own (they are found by
// nobody's name), so they cost recall, never precision.
bcubed_score bcubed(const std::vector<std::int32_t>& label, const std::vector<std::int64_t>& person) {
  std::map<std::pair<std::int64_t, std::int32_t>, double> pc;
  std::map<std::int64_t, double> cs;
  std::map<std::int32_t, double> ls;
  bcubed_score out;
  std::int64_t solo = -1;
  std::vector<std::int64_t> p = person;
  for (std::int64_t& x : p) {
    if (x == 0) {
      x = solo--;
      ++out.unfiled;
    }
  }
  for (std::size_t i = 0; i < p.size(); ++i) {
    pc[{p[i], label[i]}] += 1;
    cs[p[i]] += 1;
    ls[label[i]] += 1;
  }
  for (std::size_t i = 0; i < p.size(); ++i) {
    const double c = pc[{p[i], label[i]}];
    out.p += c / cs[p[i]];
    out.r += c / ls[label[i]];
  }
  out.p /= static_cast<double>(p.size());
  out.r /= static_cast<double>(p.size());
  out.f = 2 * out.p * out.r / (out.p + out.r);
  for (const auto& [id, n] : cs) out.people += id > 0 ? 1 : 0;
  return out;
}

}  // namespace

TEST_CASE("People bench: real face vectors through faces.db, online and settled", "[.people-bench]") {
  const char* file = std::getenv("MV_FACE_EVAL");
  if (!file) {
    WARN("MV_FACE_EVAL is not set");
    return;
  }
  std::FILE* fp = std::fopen(file, "rb");
  REQUIRE(fp);
  std::uint32_t head[2] = {0, 0};
  REQUIRE(std::fread(head, sizeof head, 1, fp) == 1);
  const std::uint32_t n = head[0], dim = head[1];
  std::vector<std::int32_t> label(n);
  std::vector<float> emb(std::size_t{n} * dim);
  REQUIRE(std::fread(label.data(), sizeof(std::int32_t), n, fp) == n);
  REQUIRE(std::fread(emb.data(), sizeof(float), emb.size(), fp) == emb.size());
  std::fclose(fp);

  mv::ai::face_tuning t;
  if (const char* tv = std::getenv("MV_FACE_TUNING")) {
    std::sscanf(tv, "%f %f %f %f %f %f", &t.same_person, &t.keep, &t.keep_weak, &t.margin, &t.ambiguous,
                &t.merge_at);
  }
  const std::string path = temp_db("bench.db");
  auto db = mv::ai::faces_db::open(path, t, dim, "bench/1");
  REQUIRE(db);
  std::vector<std::uint32_t> order(n);
  for (std::uint32_t i = 0; i < n; ++i) order[i] = i;
  std::mt19937 rng(7);
  std::shuffle(order.begin(), order.end(), rng);
  // One face per asset; the asset id is the row, so the label is found again.
  for (std::uint32_t i : order) {
    mv::ai::face_in f;
    f.x = 0.3f;
    f.y = 0.3f;
    f.w = 0.3f;
    f.h = 0.3f;
    f.score = 0.95f;
    f.quality = 0.8f;
    f.emb.assign(emb.begin() + std::ptrdiff_t{i} * dim, emb.begin() + std::ptrdiff_t{i + 1} * dim);
    REQUIRE((*db)->add(i, "f", -1, std::span<const mv::ai::face_in>(&f, 1)));
  }
  const auto measure = [&] {
    std::vector<std::int64_t> person(n, 0);
    for (const mv::ai::person_row& p : (*db)->people(1)) {
      for (const mv::ai::face_row& f : (*db)->faces_of(p.id)) person[static_cast<std::size_t>(f.asset)] = p.id;
    }
    return bcubed(label, person);
  };
  const auto report = [&](const char* what, const bcubed_score& s) {
    std::printf("%-10s BCubed P %.4f R %.4f F %.4f  people %zu  unfiled %zu\n", what, s.p, s.r, s.f, s.people,
                s.unfiled);
  };
  report("online", measure());
  (void)(*db)->consolidate(t.merge_at);
  report("+merge", measure());
  const auto t0 = std::chrono::steady_clock::now();
  for (int round = 0; round < 3; ++round) {
    const mv::ai::refine_snapshot snap = (*db)->refine_begin(true);
    refine_input in = input_of(snap);
    in.dim = dim;
    const mv::ai::refine_stats st = (*db)->refine_commit(snap, mv::ai::refine_people(in, t.refine()));
    std::printf("settle %d: evicted %u moved %u admitted %u regrouped %u (groups %u)\n", round, st.evicted,
                st.moved, st.admitted, st.regrouped, st.groups);
    if (!st.changed()) break;
  }
  (void)(*db)->consolidate(t.merge_at);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  report("settled", measure());
  std::printf("settle took %.0f ms for %u faces of %u-d\n", ms, n, dim);
}
