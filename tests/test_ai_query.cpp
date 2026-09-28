// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Local search query language (src/addons/ai/query.h, plan/17 "Query
// syntax"): parsing, names, suggestions and the singular / plural pair. Pure
// C++, no models: every case runs everywhere.
#include "catch_compat.h"

#include <string>
#include <vector>

#include "addons/ai/query.h"

namespace q = mv::ai::query;

namespace {

const std::vector<q::person_name> kPeople = {
    {1, "Tristan"}, {2, "Nico"}, {3, "Aaryan"}, {7, "Josy"}, {12, "Mimi"}, {13, "Jolie"}, {20, "Anna Smith"},
    {21, "José"},   {30, "Carla"},
};

q::plan run(const char* text) { return q::resolve(q::parse(text), kPeople); }

std::vector<std::int64_t> ids(const std::vector<std::vector<std::int64_t>>& groups, std::size_t i) {
  return i < groups.size() ? groups[i] : std::vector<std::int64_t>{};
}

std::int64_t unix_day(std::int64_t y, unsigned m, unsigned d) { return q::days_from_civil(y, m, d) * 86400; }

}  // namespace

TEST_CASE("query: a scene alone is text for the towers", "[ai][query]") {
  const auto p = run("mountain lake");
  CHECK(p.text == "mountain lake");
  CHECK(p.people.empty());
  CHECK(p.phrases.empty());
  CHECK(p.kinds == 0);
  CHECK_FALSE(p.has_dates());
  CHECK(p.people_first.empty());
}

TEST_CASE("query: a person and a scene, and a person and words said", "[ai][query]") {
  auto p = run("Tristan beach");
  REQUIRE(p.people.size() == 1);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{1});
  CHECK(p.text == "beach");

  p = run("Tristan \"hello there\"");
  REQUIRE(p.people.size() == 1);
  CHECK(p.text.empty());
  REQUIRE(p.phrases.size() == 1);
  CHECK(p.phrases[0] == "hello there");
  CHECK_FALSE(p.last_phrase_open);

  // The owner's spelling: Name:"words".
  p = run("Tristan:\"hello\"");
  REQUIRE(p.people.size() == 1);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{1});
  REQUIRE(p.phrases.size() == 1);
  CHECK(p.phrases[0] == "hello");

  // Smart quotes, and a quote still being typed.
  p = run("tristan \xE2\x80\x9Chello\xE2\x80\x9D");
  REQUIRE(p.phrases.size() == 1);
  CHECK(p.phrases[0] == "hello");
  p = run("\"good mor");
  REQUIRE(p.phrases.size() == 1);
  CHECK(p.phrases[0] == "good mor");
  CHECK(p.last_phrase_open);

  p = run("said:hello");
  REQUIRE(p.phrases.size() == 1);
  CHECK(p.phrases[0] == "hello");
}

TEST_CASE("query: people side by side are all of them; or is any", "[ai][query]") {
  auto p = run("Tristan and Aaryan");
  REQUIRE(p.people.size() == 2);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{1});
  CHECK(ids(p.people, 1) == std::vector<std::int64_t>{3});
  CHECK(p.text.empty());  // "and" alone describes nothing

  p = run("Tristan or Aaryan");
  REQUIRE(p.people.size() == 1);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{1, 3});

  // A description's own "or" stays in it.
  p = run("beach or lake");
  CHECK(p.text == "beach or lake");

  // Names are case-insensitive, whole or by the first name, accents folded.
  p = run("anna smith on a boat");
  REQUIRE(p.people.size() == 1);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{20});
  CHECK(p.text == "on a boat");
  p = run("Anna skiing");
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{20});
  CHECK(p.text == "skiing");
  p = run("jose");
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{21});
  CHECK(p.text.empty());
}

TEST_CASE("query: leaving things out", "[ai][query]") {
  auto p = run("beach -Nico -dog -\"goodbye\"");
  CHECK(p.text == "beach");
  CHECK(p.not_people == std::vector<std::int64_t>{2});
  CHECK(p.not_text == std::vector<std::string>{"dog"});
  CHECK(p.not_phrases == std::vector<std::string>{"goodbye"});

  p = run("-video Tristan");
  CHECK(p.kinds == 1u);  // photos
  p = run("-@nic beach");
  CHECK(p.not_people == std::vector<std::int64_t>{2});
  // A hyphenated word and a lone dash are not exclusions.
  p = run("t-shirt - hat");
  CHECK(p.not_text.empty());
  CHECK(p.text == "t-shirt - hat");
}

TEST_CASE("query: kinds, first or last, or is:", "[ai][query]") {
  auto p = run("videos of Tristan");
  CHECK(p.kinds == 2u);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{1});
  CHECK(p.text.empty());  // "of" alone describes nothing

  p = run("beach photos");
  CHECK(p.kinds == 1u);
  CHECK(p.text == "beach");

  p = run("man holding a video camera");  // in the middle: a description
  CHECK(p.kinds == 0);
  CHECK(p.text == "man holding a video camera");

  p = run("is:video dog");
  CHECK(p.kinds == 2u);
  CHECK(p.text == "dog");

  p = run("video");  // a filter alone: every video in scope
  CHECK(p.kinds == 2u);
  CHECK(p.text.empty());

  p = run("is:video -is:video");
  CHECK(p.impossible);

  // A spoken request (plan/19): "photos of" asks for anything, as before.
  p = run("show me photos of Nico");
  CHECK(p.kinds == 0);
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{2});
  CHECK(p.text.empty());
  p = run("Pull up all the photos that include a dog on the beach");
  CHECK(p.kinds == 0);
  CHECK(p.text == "a dog on the beach");
  p = run("photo");  // alone it is last as well: a filter
  CHECK(p.kinds == 1u);
}

TEST_CASE("query: dates are the file's, in UTC", "[ai][query]") {
  auto p = run("in:2024 beach");
  CHECK(p.from_unix == unix_day(2024, 1, 1));
  CHECK(p.to_unix == unix_day(2025, 1, 1));
  CHECK(p.text == "beach");

  p = run("in:2024-02");
  CHECK(p.from_unix == unix_day(2024, 2, 1));
  CHECK(p.to_unix == unix_day(2024, 3, 1));
  p = run("in:2024-12");
  CHECK(p.to_unix == unix_day(2025, 1, 1));
  p = run("in:2024-02-29");
  CHECK(p.to_unix - p.from_unix == 86400);

  p = run("before:2025 after:2023");
  CHECK(p.from_unix == unix_day(2024, 1, 1));
  CHECK(p.to_unix == unix_day(2025, 1, 1));
  p = run("since:2023-05-01 until:2023-05");
  CHECK(p.from_unix == unix_day(2023, 5, 1));
  CHECK(p.to_unix == unix_day(2023, 6, 1));

  // Being typed, or nonsense: no filter, and never searched as words.
  p = run("in:20");
  CHECK_FALSE(p.has_dates());
  CHECK(p.text.empty());
  p = run("in:2024-13");
  CHECK_FALSE(p.has_dates());

  p = run("before:2020 after:2021");
  CHECK(p.impossible);

  CHECK(q::days_from_civil(1970, 1, 1) == 0);
  CHECK(q::days_from_civil(2000, 3, 1) == 11017);
}

TEST_CASE("query: file: matches the file's name", "[ai][query]") {
  auto p = run("file:IMG_12 beach");
  CHECK(p.files == std::vector<std::string>{"img_12"});
  CHECK(p.text == "beach");
  CHECK(p.narrows());
  CHECK(p.has_files());

  p = run("file:\"Trip José\" -file:copy");
  CHECK(p.files == std::vector<std::string>{"trip jose"});
  CHECK(p.not_files == std::vector<std::string>{"copy"});
  CHECK(p.text.empty());

  // Being typed: no filter, and "file" is never searched as a word.
  p = run("file:");
  CHECK_FALSE(p.has_files());
  CHECK(p.text.empty());

  const std::vector<std::string> want = {"img_12"};
  const std::vector<std::string> none;
  CHECK(q::name_matches("/Users/a/DCIM/IMG_1234.HEIC", want, none));
  CHECK(q::name_matches("C:\\Photos\\img_12.jpg", want, none));
  // The folder does not count, only the name.
  CHECK_FALSE(q::name_matches("/img_12/other.jpg", want, none));
  CHECK_FALSE(q::name_matches("/a/IMG_1234 copy.jpg", want, std::vector<std::string>{"copy"}));
  // A decomposed Mac name (e + U+0301) folds like a typed é.
  CHECK(q::name_matches("/a/Trip Jose\xCC\x81 1.mov", std::vector<std::string>{"trip jose 1"}, none));
  CHECK(q::name_matches("/a/Trip Jos\xC3\xA9.mov", std::vector<std::string>{"trip jose"}, none));
}

TEST_CASE("query: a lone word that starts a name shows them first", "[ai][query]") {
  auto p = run("Trist");
  CHECK(p.people.empty());
  CHECK(p.people_first == std::vector<std::int64_t>{1});
  CHECK(p.text == "Trist");  // and it still describes

  p = run("trist");
  CHECK(p.people_first == std::vector<std::int64_t>{1});

  p = run("Tristna");  // a typo
  CHECK(p.people_first == std::vector<std::int64_t>{1});
  p = run("Trsitan");
  CHECK(p.people_first == std::vector<std::int64_t>{1});

  p = run("jo");  // Josy, Jolie and José all start with it
  CHECK(p.people_first == std::vector<std::int64_t>{7, 13, 21});

  p = run("car");  // Carla first, and cars after her
  CHECK(p.people_first == std::vector<std::int64_t>{30});
  CHECK(p.text == "car");

  // Not inside a description, not a stop word, not far off.
  CHECK(run("red car").people_first.empty());
  CHECK(run("Trist beach").people_first.empty());
  CHECK(run("the").people_first.empty());
  CHECK(run("mountain").people_first.empty());
  CHECK(run("lake").people_first.empty());

  // @ is explicit: a prefix or a near spelling filters, and a name nobody has
  // matches nothing rather than turning into a description.
  p = run("@tri beach");
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{1});
  CHECK(p.text == "beach");
  p = run("person:\"anna smith\"");
  CHECK(ids(p.people, 0) == std::vector<std::int64_t>{20});
  p = run("@zzzz");
  CHECK(p.impossible);
}

TEST_CASE("query: suggestions complete the word being typed", "[ai][query]") {
  auto s = q::suggest("Trist", kPeople);
  REQUIRE(s.size() == 1);
  CHECK(s[0].id == 1);
  CHECK(s[0].completion == "Tristan ");

  s = q::suggest("beach with jo", kPeople);
  REQUIRE(s.size() == 3);
  CHECK(s[0].completion == "beach with Josy ");

  s = q::suggest("-Ni", kPeople);
  REQUIRE(s.size() == 1);
  CHECK(s[0].completion == "-Nico ");

  s = q::suggest("@ann", kPeople);
  REQUIRE(s.size() == 1);
  CHECK(s[0].completion == "@\"Anna Smith\" ");

  s = q::suggest("Tirs", kPeople);  // a typo still finds Tristan
  REQUIRE_FALSE(s.empty());
  CHECK(s[0].id == 1);

  CHECK(q::suggest("Tristan", kPeople).empty());   // already whole
  CHECK(q::suggest("Trist ", kPeople).empty());    // the word is finished
  CHECK(q::suggest("mountain", kPeople).empty());
  CHECK(q::suggest("\"hel", kPeople).empty());     // quoted words are said, not names
  CHECK(q::suggest("in:20", kPeople).empty());
  CHECK(q::suggest("a", kPeople).empty());
}

TEST_CASE("query: singular and plural embed as one pair", "[ai][query]") {
  CHECK(q::number_forms("mountain") == std::vector<std::string>{"mountain", "mountains"});
  CHECK(q::number_forms("mountains") == std::vector<std::string>{"mountain", "mountains"});
  CHECK(q::number_forms("snowy mountains") == std::vector<std::string>{"snowy mountain", "snowy mountains"});
  CHECK(q::number_forms("beach") == q::number_forms("beaches"));
  CHECK(q::number_forms("city") == q::number_forms("cities"));
  CHECK(q::number_forms("child") == q::number_forms("children"));
  CHECK(q::number_forms("dog on a beach") == std::vector<std::string>{"dog on a beach", "dog on a beaches"});
  CHECK(q::number_forms("boy") == std::vector<std::string>{"boy", "boys"});
  // Nothing to pair: a mass noun, a verb, a number, a short word.
  CHECK(q::number_forms("snow") == std::vector<std::string>{"snow"});
  CHECK(q::number_forms("people running") == std::vector<std::string>{"people running"});
  CHECK(q::number_forms("2024") == std::vector<std::string>{"2024"});
  CHECK(q::number_forms("  sea ") == std::vector<std::string>{"sea", "seas"});
  CHECK(q::number_forms("glass") == std::vector<std::string>{"glass", "glasses"});
  CHECK(q::number_forms("glasses") == std::vector<std::string>{"glasses"});  // eyewear, not two glasses
  CHECK(q::other_number("bus") == "buses");
  CHECK(q::other_number("grass").empty());
}

TEST_CASE("query: phrases match consecutive transcript words", "[ai][query]") {
  const auto words = q::words_of("Well, we're now landing in Lisbon!");
  CHECK(words == std::vector<std::string>{"well", "were", "now", "landing", "in", "lisbon"});
  const auto phrase = q::words_of("landing in");
  CHECK(q::contains_phrase(words, phrase, false));
  CHECK_FALSE(q::contains_phrase(words, q::words_of("in landing"), false));
  CHECK_FALSE(q::contains_phrase(words, q::words_of("landing in lis"), false));
  CHECK(q::contains_phrase(words, q::words_of("landing in lis"), true));
  CHECK_FALSE(q::contains_phrase(words, {}, true));
}

TEST_CASE("query: edit distance with transpositions", "[ai][query]") {
  CHECK(q::edit_distance("tristan", "tristan", 2) == 0);
  CHECK(q::edit_distance("tristna", "tristan", 2) == 1);
  CHECK(q::edit_distance("trstan", "tristan", 2) == 1);
  CHECK(q::edit_distance("kitten", "sitting", 5) == 3);
  CHECK(q::edit_distance("abc", "xyzzy", 1) == 2);  // stops past the limit
  CHECK(q::fold("JOSÉ Ærø") == "jose aero");
}
