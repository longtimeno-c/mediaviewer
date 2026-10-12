// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #210: F2 rename of one stop, pair- and sidecar-aware (io/rename_stop.h).
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "io/rename_stop.h"

namespace fs = std::filesystem;
using mv::io::check_stem;
using mv::io::plan_rename;
using mv::io::stem_problem;

namespace {

std::string p(const char* name) { return (fs::path("/d") / name).string(); }

// A fake folder: `exists` answers from a set of paths.
struct fake_dir {
  std::set<std::string> files;
  bool operator()(std::string_view path) const { return files.count(std::string(path)) != 0; }
};

std::string to_of(const std::vector<mv::io::rename_move>& plan, const std::string& from) {
  for (const auto& m : plan) {
    if (m.from == from) return m.to;
  }
  return "<not moved>";
}

struct temp_dir {
  fs::path path;
  temp_dir() {
    path = fs::temp_directory_path() /
           ("mv-rename-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string file(const char* name, const char* body = "x") const {
    std::ofstream(path / name, std::ios::binary) << body;
    return (path / name).string();
  }
  [[nodiscard]] bool has(const char* name) const { return fs::exists(path / name); }
  [[nodiscard]] std::string read(const char* name) const {
    std::ifstream f(path / name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  }
};

}  // namespace

TEST_CASE("a new name is a file name on both platforms", "[io][rename]") {
  CHECK(check_stem("Beach day") == stem_problem::ok);
  CHECK(check_stem("été 2026") == stem_problem::ok);
  CHECK(check_stem("") == stem_problem::empty);
  CHECK(check_stem("   ") == stem_problem::empty);
  CHECK(check_stem("a/b") == stem_problem::separator);
  CHECK(check_stem("a\\b") == stem_problem::separator);
  CHECK(check_stem("what?") == stem_problem::reserved);
  CHECK(check_stem("a:b") == stem_problem::reserved);
  CHECK(check_stem("..") == stem_problem::dots);
  CHECK(check_stem("trailing.") == stem_problem::dots);
  CHECK(check_stem("trailing ") == stem_problem::dots);
  CHECK(check_stem("CON") == stem_problem::device);
  CHECK(check_stem("com3") == stem_problem::device);
  CHECK(check_stem("console") == stem_problem::ok);
  CHECK(check_stem(std::string(201, 'a')) == stem_problem::too_long);
  CHECK(std::string(mv::io::stem_problem_text(stem_problem::separator)).find('/') != std::string::npos);
  CHECK(mv::io::stem_of("/a/IMG_1234.CR2") == "IMG_1234");
  CHECK(mv::io::stem_of("/a/.hidden") == ".hidden");
}

TEST_CASE("a rename carries the pair and every sidecar under one stem", "[io][rename]") {
  fake_dir d;
  d.files = {p("IMG_1.JPG"), p("IMG_1.CR2"), p("IMG_1.xmp"), p("IMG_1.CR2.xmp"), p("IMG_2.JPG")};
  const auto plan = plan_rename(p("IMG_1.JPG"), p("IMG_1.CR2"), "Beach", d);
  REQUIRE(plan.size() == 4);
  CHECK(to_of(plan, p("IMG_1.JPG")) == p("Beach.JPG"));
  CHECK(to_of(plan, p("IMG_1.CR2")) == p("Beach.CR2"));
  CHECK(to_of(plan, p("IMG_1.xmp")) == p("Beach.xmp"));
  CHECK(to_of(plan, p("IMG_1.CR2.xmp")) == p("Beach.CR2.xmp"));

  // A Live Photo's motion goes with its still.
  fake_dir live;
  live.files = {p("IMG_9.HEIC"), p("IMG_9.MOV")};
  const auto lp = plan_rename(p("IMG_9.HEIC"), p("IMG_9.MOV"), "Wave", live);
  REQUIRE(lp.size() == 2);
  CHECK(to_of(lp, p("IMG_9.MOV")) == p("Wave.MOV"));
}

TEST_CASE("a taken name moves the whole group to name (2)", "[io][rename]") {
  fake_dir d;
  // Beach.JPG is free but Beach.CR2 is not: the pair would split, so both take (2).
  d.files = {p("IMG_1.JPG"), p("IMG_1.CR2"), p("Beach.CR2")};
  auto plan = plan_rename(p("IMG_1.JPG"), p("IMG_1.CR2"), "Beach", d);
  REQUIRE(plan.size() == 2);
  CHECK(to_of(plan, p("IMG_1.JPG")) == p("Beach (2).JPG"));
  CHECK(to_of(plan, p("IMG_1.CR2")) == p("Beach (2).CR2"));

  d.files.insert(p("Beach (2).xmp"));
  d.files.insert(p("IMG_1.xmp"));
  plan = plan_rename(p("IMG_1.JPG"), p("IMG_1.CR2"), "Beach", d);
  CHECK(to_of(plan, p("IMG_1.JPG")) == p("Beach (3).JPG"));
  CHECK(to_of(plan, p("IMG_1.xmp")) == p("Beach (3).xmp"));
}

TEST_CASE("the same name is no change; a case-only rename is not a collision", "[io][rename]") {
  fake_dir d;
  d.files = {p("IMG_1.JPG")};
  CHECK(plan_rename(p("IMG_1.JPG"), {}, "IMG_1", d).empty());
  // On a case-insensitive volume img_1.JPG "exists" because it is the file itself.
  const auto lowered = [](std::string_view in) {
    std::string s(in);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string only = lowered(p("IMG_1.JPG"));
  const auto ci = [&](std::string_view path) { return lowered(path) == only; };
  const auto plan = plan_rename(p("IMG_1.JPG"), {}, "img_1", ci);
  REQUIRE(plan.size() == 1);
  CHECK(plan[0].to == p("img_1.JPG"));
  // Nothing at the path: nothing to plan.
  CHECK(plan_rename(p("gone.JPG"), {}, "x", d).empty());
}

TEST_CASE("rename_stop renames on disk, never over a file, and keeps the bytes", "[io][rename]") {
  temp_dir d;
  const std::string jpg = d.file("IMG_1.JPG", "jpeg");
  const std::string raw = d.file("IMG_1.CR2", "raw");
  d.file("IMG_1.xmp", "<xmp/>");
  d.file("Beach.JPG", "someone else's");

  auto r = mv::io::rename_stop(jpg, raw, "Beach");
  REQUIRE(r);
  CHECK(r->moved == 3);
  CHECK(r->primary == (d.path / "Beach (2).JPG").string());
  CHECK(r->secondary == (d.path / "Beach (2).CR2").string());
  CHECK(d.read("Beach.JPG") == "someone else's");  // never overwritten
  CHECK(d.read("Beach (2).JPG") == "jpeg");
  CHECK(d.read("Beach (2).CR2") == "raw");
  CHECK(d.read("Beach (2).xmp") == "<xmp/>");
  CHECK_FALSE(d.has("IMG_1.JPG"));
  CHECK_FALSE(d.has("IMG_1.CR2"));
  CHECK_FALSE(d.has("IMG_1.xmp"));

  // Refusals change nothing.
  CHECK(mv::io::rename_stop(r->primary, r->secondary, "a/b").error() == mv::status::invalid_arg);
  CHECK(mv::io::rename_stop(r->primary, r->secondary, "Beach (2)").error() == mv::status::invalid_arg);
  CHECK(mv::io::rename_stop((d.path / "gone.JPG").string(), {}, "x").error() == mv::status::not_found);
  CHECK(d.has("Beach (2).JPG"));

  // A case-only rename, through a temporary on a case-insensitive volume.
  auto c = mv::io::rename_stop(r->primary, r->secondary, "beach (2)");
  REQUIRE(c);
  CHECK(c->primary == (d.path / "beach (2).JPG").string());
  CHECK(d.read("beach (2).JPG") == "jpeg");
}

TEST_CASE("the listing is told the new name before the file moves", "[io][rename]") {
  temp_dir d;
  const std::string jpg = d.file("IMG_7.JPG", "jpeg");
  std::string told_from;
  std::string told_to;
  bool moved_yet = true;
  auto r = mv::io::rename_stop(jpg, {}, "Seven", [&](const std::string& from, const std::string& to) {
    told_from = from;
    told_to = to;
    moved_yet = !fs::exists(from);
  });
  REQUIRE(r);
  CHECK(told_from == jpg);
  CHECK(told_to == r->primary);
  CHECK_FALSE(moved_yet);
}
