// SPDX-License-Identifier: GPL-2.0-or-later
//
// PR 8 verify: "confirm telemetry stays off unless explicitly enabled", and
// rule 6: nothing about a user's files leaves the machine.
//
// The payload gate is pure, so it is tested directly rather than inferred from
// a call site that happens to behave today.
#include <catch2/catch_test_macros.hpp>

#include "shell/telemetry.h"

using namespace mv::shell::telemetry;

TEST_CASE("telemetry: the gate rejects anything that could name a file", "[telemetry]") {
  // The things plan/13 says must never leave the machine.
  CHECK(looks_like_user_data("C:\\Users\\alice\\DCIM"));
  CHECK(looks_like_user_data("/home/alice/photos"));
  CHECK(looks_like_user_data("IMG_0001.JPG"));
  CHECK(looks_like_user_data("holiday.heic"));
  CHECK(looks_like_user_data("%USERPROFILE%"));
  CHECK(looks_like_user_data("~/Pictures"));
  CHECK(looks_like_user_data("alice@example.com"));
  CHECK(looks_like_user_data("https:"));
  CHECK(looks_like_user_data("D:"));
  // A hash of a path is still a stable identifier for a private file, but it
  // is also just hex - the length cap is what stops it.
  CHECK_FALSE(tag_allowed("9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"));
  // Non-ASCII: a wide path transcoded, or a name.
  CHECK(looks_like_user_data("\xc3\xa9t\xc3\xa9"));
  // Control characters would also break the line format.
  CHECK(looks_like_user_data(std::string("a\nb")));
  CHECK(looks_like_user_data(""));

  // The whitelisted vocabulary plan/13 does allow.
  CHECK(tag_allowed("heif"));
  CHECK(tag_allowed("jpeg"));
  CHECK(tag_allowed("libde265"));
  CHECK(tag_allowed("LibRaw"));
  CHECK(tag_allowed("NVIDIA"));
  CHECK(tag_allowed("ILCE-7M3"));     // camera model: the one metadata exception
  CHECK(tag_allowed("Canon_EOS_R6"));
  CHECK(tag_allowed("slideshow"));
}

TEST_CASE("telemetry: a formatted event carries no free text", "[telemetry]") {
  const std::string line =
      format_event(event::decode_failed, "heif", {{"status", -3}, {"width", 4032}},
                   "0123456789abcdef0123456789abcdef", "0.1.0");
  CHECK(line ==
        "{\"event\":1,\"tag\":\"heif\",\"install\":\"0123456789abcdef0123456789abcdef\","
        "\"version\":\"0.1.0\",\"status\":-3,\"width\":4032}");
  // Nothing in a well-formed line can be a path: every field is either an id,
  // a gated tag, a hex install id, a version, or a number.
  CHECK(line.find("C:\\") == std::string::npos);
}

TEST_CASE("telemetry: an unknown metric name drops the whole event", "[telemetry]") {
  // Not a rename to fix at the call site: a name outside the table is how a
  // path would arrive with a plausible-looking key.
  CHECK_FALSE(record(event::feature_used, "slideshow", {{"path", 1}}));
  CHECK_FALSE(record(event::feature_used, "slideshow", {{"filename", 1}}));
}

TEST_CASE("telemetry: off by default, and records nothing while off", "[telemetry]") {
  // The suite does not opt in, so this is the shipped default: plan/13's
  // "Default off. Opt-in, once, honestly." The first-run screen has not been
  // answered either.
  CHECK_FALSE(enabled());
  CHECK_FALSE(asked());
  // No install id exists on a machine that never opted in.
  CHECK(install_id().empty());
  // And every recording attempt is inert, whatever it is handed.
  CHECK_FALSE(record(event::session_started, "startup", {}));
  CHECK_FALSE(record(event::decode_failed, "heif", {{"status", -3}}));
  CHECK_FALSE(record(event::frame_pacing, "NVIDIA", {{"p99_us", 8200}}));
}
