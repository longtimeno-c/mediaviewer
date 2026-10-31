// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #155: the AI pack's provider verdicts (provider.txt,
// src/addons/ai/provider_verdict.h), without a runtime or a model. A tower an
// accelerated provider failed here is reported failed on every later start
// (engine_deps::accelerated_failure), which makes Auto open the small tower
// instead of running the large one on CPU (the engine's half is in
// test_ai_engine.cpp, "[fallback]"); a pass, a forget or a new runtime clears it.
#include "catch_compat.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "addons/ai/provider_verdict.h"

namespace fs = std::filesystem;
using mv::ai::provider_verdicts;
using mv::ai::verdict_key;
using mv::ai::verdict_of;

namespace {

struct scratch {
  fs::path dir;
  explicit scratch(const char* name) : dir(fs::temp_directory_path() / name) {
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  // UTF-8, as the pack passes its data folder (a Windows temp folder may not be ASCII).
  std::string file() const {
    const std::u8string u = (dir / "provider.txt").u8string();
    return std::string(u.begin(), u.end());
  }
};

fs::path native(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string read_bytes(const std::string& path) {
  std::ifstream in(native(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_bytes(const std::string& path, const std::string& text) {
  std::ofstream out(native(path), std::ios::binary | std::ios::trunc);
  out << text;
}

const std::string kLarge = "clip-vit-l14/fp16/pre1";
const std::string kSmall = "clip-vit-b32/fp16/pre1";

}  // namespace

TEST_CASE("verdict keys name the runtime, the provider and the tower", "[ai][provider-verdict]") {
  CHECK(verdict_key("1.30.0", "coreml", kLarge) == "1.30.0|coreml|clip-vit-l14/fp16/pre1");
  CHECK(verdict_key("1.30.0", "cuda", kLarge) != verdict_key("1.30.0", "coreml", kLarge));
  CHECK(verdict_key("1.31.0", "coreml", kLarge) != verdict_key("1.30.0", "coreml", kLarge));
}

TEST_CASE("an open's verdict is ok without a message, or the fault with its message", "[ai][provider-verdict]") {
  const auto ok = verdict_of(true, "none", 17.25, "ignored");
  CHECK(ok.outcome == "ok");
  CHECK(ok.open_s == 17.25);
  CHECK(ok.detail.empty());
  const auto bad = verdict_of(false, "failed", 64.5, "Error compiling model <model>");
  CHECK(bad.outcome == "failed");
  CHECK(bad.open_s == 64.5);
  CHECK(bad.detail == "Error compiling model <model>");
}

TEST_CASE("a verdict written as faulty is reported failed on the next start", "[ai][provider-verdict]") {
  const scratch s("mv_ai_verdict_next_start");
  const std::string large = verdict_key("1.30.0", "coreml", kLarge);
  {
    provider_verdicts first(s.file());
    CHECK_FALSE(first.failure(large).has_value());  // never tried: no fallback
    first.record(large, verdict_of(false, "failed", 64.5, "Error compiling model <model>"));
    REQUIRE(first.failure(large).has_value());
  }
  // The next start: a new pack over the same data folder.
  const provider_verdicts next(s.file());
  const auto why = next.failure(large);
  REQUIRE(why.has_value());
  CHECK(*why == "Error compiling model <model>");
  const auto v = next.find(large);
  REQUIRE(v.has_value());
  CHECK(v->outcome == "failed");
  CHECK(v->open_s == 64.5);
  // Only that tower on that provider with that runtime.
  CHECK_FALSE(next.failure(verdict_key("1.30.0", "coreml", kSmall)).has_value());
  CHECK_FALSE(next.failure(verdict_key("1.30.0", "cuda", kLarge)).has_value());
  // A new runtime is a new key and tries again.
  CHECK_FALSE(next.failure(verdict_key("1.31.0", "coreml", kLarge)).has_value());

  // Every fault counts, and a fault with no message is still a failure ("").
  provider_verdicts again(s.file());
  for (const char* fault : {"not_in_build", "runtime_missing", "failed", "mismatch", "slower"}) {
    again.record(large, verdict_of(false, fault, 3, ""));
    const auto f = provider_verdicts(s.file()).failure(large);
    REQUIRE(f.has_value());
    CHECK(f->empty());
  }
}

TEST_CASE("a pass is no failure; forgetting a failure tries the provider again", "[ai][provider-verdict]") {
  const scratch s("mv_ai_verdict_forget");
  const std::string large = verdict_key("1.30.0", "coreml", kLarge);
  const std::string small = verdict_key("1.30.0", "coreml", kSmall);
  provider_verdicts v(s.file());
  v.record(small, verdict_of(true, "none", 17, ""));
  CHECK_FALSE(v.failure(small).has_value());
  REQUIRE(v.find(small).has_value());
  CHECK(v.find(small)->outcome == "ok");

  v.record(large, verdict_of(false, "mismatch", 70, "cosine 0.91"));
  CHECK(provider_verdicts(s.file()).failure(large) == std::optional<std::string>("cosine 0.91"));
  // A later open that passes replaces the fault.
  v.record(large, verdict_of(true, "none", 60, ""));
  CHECK_FALSE(provider_verdicts(s.file()).failure(large).has_value());

  // "Try the larger model again" (retry_large): forgotten, the next start has no verdict.
  v.record(large, verdict_of(false, "failed", 64, "boom"));
  v.forget(large);
  const provider_verdicts next(s.file());
  CHECK_FALSE(next.failure(large).has_value());
  CHECK_FALSE(next.find(large).has_value());
  CHECK(next.find(small).has_value());  // the others stay

  // Forgetting what was never recorded leaves the file as it was.
  const std::string before = read_bytes(s.file());
  v.forget(verdict_key("1.30.0", "cuda", kLarge));
  CHECK(read_bytes(s.file()) == before);
}

TEST_CASE("provider.txt keeps its format", "[ai][provider-verdict]") {
  const scratch s("mv_ai_verdict_format");
  provider_verdicts v(s.file());
  v.record(verdict_key("1.30.0", "coreml", kSmall), verdict_of(true, "none", 17.25, ""));
  v.record(verdict_key("1.30.0", "coreml", kLarge), verdict_of(false, "failed", 694.123456, "Error <model>"));
  // One header, one line per key in key order: key, outcome, seconds (six
  // significant digits), message, tab-separated.
  CHECK(read_bytes(s.file()) ==
        "mv-ai-provider 1\n"
        "1.30.0|coreml|clip-vit-b32/fp16/pre1\tok\t17.25\t\n"
        "1.30.0|coreml|clip-vit-l14/fp16/pre1\tfailed\t694.123\tError <model>\n");
  CHECK_FALSE(fs::exists(native(s.file() + ".tmp")));

  // A file an earlier build wrote reads the same; a short line is skipped.
  write_bytes(s.file(),
              "mv-ai-provider 1\n"
              "1.29.0|cuda|clip-vit-l14/fp16/pre1\truntime_missing\t-1\tcudnn64_9.dll not found\n"
              "garbage without tabs\n"
              "1.29.0|cuda|x\tok\t2\n"
              "1.29.0|cuda|clip-vit-b32/fp16/pre1\tok\t4.5\t\n");
  const auto all = mv::ai::read_verdicts(s.file());
  REQUIRE(all.size() == 2);
  const auto& cuda = all.at("1.29.0|cuda|clip-vit-l14/fp16/pre1");
  CHECK(cuda.outcome == "runtime_missing");
  CHECK(cuda.open_s == -1);
  CHECK(cuda.detail == "cudnn64_9.dll not found");
  CHECK(all.at("1.29.0|cuda|clip-vit-b32/fp16/pre1").open_s == 4.5);
  CHECK(v.failure("1.29.0|cuda|clip-vit-l14/fp16/pre1") == std::optional<std::string>("cudnn64_9.dll not found"));

  // Another version or no file: nothing recorded, so nothing failed.
  write_bytes(s.file(), "mv-ai-provider 2\n1.30.0|coreml|clip-vit-l14/fp16/pre1\tfailed\t1\tx\n");
  CHECK(mv::ai::read_verdicts(s.file()).empty());
  CHECK_FALSE(v.failure(verdict_key("1.30.0", "coreml", kLarge)).has_value());
  fs::remove(native(s.file()));
  CHECK(mv::ai::read_verdicts(s.file()).empty());
  CHECK_FALSE(v.failure(verdict_key("1.30.0", "coreml", kLarge)).has_value());
}

TEST_CASE("a provider's message is kept to one line of 400 bytes", "[ai][provider-verdict]") {
  const scratch s("mv_ai_verdict_detail");
  const std::string key = verdict_key("1.30.0", "coreml", kLarge);
  provider_verdicts v(s.file());
  v.record(key, verdict_of(false, "failed", 1, "line one\nline\ttwo\r\n" + std::string(500, 'x')));
  const auto why = provider_verdicts(s.file()).failure(key);
  REQUIRE(why.has_value());
  CHECK(why->size() == 400);
  CHECK(why->rfind("line one line two  xx", 0) == 0);
  CHECK(why->find_first_of("\t\r\n") == std::string::npos);
  CHECK(mv::ai::read_verdicts(s.file()).size() == 1);
}
