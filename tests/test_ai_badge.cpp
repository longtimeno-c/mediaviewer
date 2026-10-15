// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #345 (docs/plans/metadata-search-and-place.md, slice 5): the canvas's AI badge --
// the setting's default, the evidence line each kind of provenance gives, the
// text both hosts show, and the store that answers it without the pane open.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "core/job_system.h"
#include "meta/generators.h"
#include "shell/ai_badge.h"
#include "shell/meta_store.h"
#include "shell/settings.h"

namespace {

constexpr std::string_view kTrained = "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia";
constexpr std::string_view kComposite =
    "http://cv.iptc.org/newscodes/digitalsourcetype/compositeWithTrainedAlgorithmicMedia";

mv::io::dir_entry entry(const std::string& path, std::int64_t mtime = 100, std::uint64_t size = 10) {
  mv::io::dir_entry e;
  e.path_utf8 = path;
  e.name_utf8 = path;
  e.mtime_unix = mtime;
  e.size = size;
  return e;
}

template <class F>
bool wait_for(F done) {
  for (int i = 0; i < 400 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return done();
}

}  // namespace

TEST_CASE("the AI badge setting is on by default and in words saved before it", "[ai-badge][settings]") {
  using mv::shell::view_settings;
  CHECK(view_settings{}.ai_badge);
  // Stored inverted: the default word carries no bit for it...
  CHECK((view_settings{}.flags() & mv::shell::kSettingHideAiBadge) == 0);
  // ...so a word saved before the bit existed reads as on.
  CHECK(view_settings::from_flags(mv::shell::kSettingWrap | mv::shell::kSettingFilmstripFolder).ai_badge);
  CHECK(view_settings::from_flags(0).ai_badge);
  view_settings off;
  off.ai_badge = false;
  CHECK((off.flags() & mv::shell::kSettingHideAiBadge) != 0);
  CHECK_FALSE(view_settings::from_flags(off.flags()).ai_badge);
  // Its own bit: nothing else moves with it.
  const auto round = view_settings::from_flags(off.flags());
  CHECK(round.wrap == off.wrap);
  CHECK(round.show_audio == off.show_audio);
  CHECK(round.sticky_zoom == off.sticky_zoom);
  CHECK(mv::shell::kSettingHideAiBadge == (1 << 14));  // SettingsView.swift, IslandHost.cs
}

TEST_CASE("evidence names the deciding fact, most authoritative first", "[ai-badge][provenance]") {
  using mv::meta::derive_provenance;
  mv::meta::parse::evidence none;
  CHECK(derive_provenance(none).evidence.empty());

  // A C2PA source type.
  mv::meta::parse::evidence c2pa;
  c2pa.c2pa_generator = "ChatGPT 1.0";
  c2pa.source_types.emplace_back(kTrained);
  auto p = derive_provenance(c2pa);
  CHECK(p.ai_generated);
  CHECK(p.made_with == "ChatGPT");
  CHECK(p.evidence == "Content Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)");

  // The same term in XMP only.
  mv::meta::parse::evidence xmp;
  xmp.source_types.emplace_back(kComposite);
  xmp.xmp_source_type = std::string(kComposite);
  p = derive_provenance(xmp);
  CHECK(p.ai_generated);
  CHECK(p.made_with.empty());
  CHECK(p.evidence ==
        "IPTC digital source type (XMP): Includes AI-generated elements (compositeWithTrainedAlgorithmicMedia)");

  // Both say it: the signed claim is named.
  mv::meta::parse::evidence both = xmp;
  both.source_types.emplace_back(kTrained);
  CHECK(derive_provenance(both).evidence == "Content Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)");
  // ... with the claim's own term, not XMP's, whichever came first.
  mv::meta::parse::evidence same = xmp;
  same.source_types.emplace_back(kComposite);
  CHECK(derive_provenance(same).evidence ==
        "Content Credentials (C2PA): Includes AI-generated elements (compositeWithTrainedAlgorithmicMedia)");

  // A generator named by a field, no source type.
  mv::meta::parse::evidence tool;
  tool.xmp_creator_tool = "Midjourney";
  CHECK(derive_provenance(tool).evidence == "XMP CreatorTool: Midjourney");
  mv::meta::parse::evidence agent;
  agent.c2pa_generator = "Microsoft Designer";
  agent.c2pa_agents.push_back("DALL-E 3");
  // The claim generator is a table entry itself, so it is the deciding field.
  CHECK(derive_provenance(agent).evidence == "C2PA claim generator: Microsoft Designer");
  mv::meta::parse::evidence soft;
  soft.exif_software = "Adobe Firefly 2.1";
  CHECK(derive_provenance(soft).evidence == "EXIF Software: Adobe Firefly 2.1");

  // The PNG generator chunks.
  mv::meta::parse::evidence sd;
  sd.png_parameters = true;
  CHECK(derive_provenance(sd).evidence == "PNG parameters chunk (Stable Diffusion settings)");
  mv::meta::parse::evidence comfy;
  comfy.png_comfyui = true;
  CHECK(derive_provenance(comfy).evidence == "PNG prompt / workflow chunks (a ComfyUI graph)");
  sd.png_generator_text.push_back("Version: v1.10.1, stable-diffusion-webui");
  CHECK(derive_provenance(sd).evidence == "PNG text: Version: v1.10.1, stable-diffusion-webui");

  // Positive only: an editor, a camera, a non-AI source type say nothing.
  mv::meta::parse::evidence camera;
  camera.exif_software = "Firmware Version 1.8.1";
  camera.xmp_creator_tool = "Adobe Lightroom";
  camera.c2pa_generator = "Adobe Photoshop 25.0";
  camera.source_types.push_back("http://cv.iptc.org/newscodes/digitalsourcetype/digitalCapture");
  p = derive_provenance(camera);
  CHECK_FALSE(p.ai_generated);
  CHECK(p.evidence.empty());
  CHECK(p.made_with == "Adobe Photoshop 25.0");

  // A prompt-length value is capped.
  const std::string line = mv::meta::evidence_line("PNG text", std::string(300, 'x'));
  CHECK(line.rfind("PNG text: x", 0) == 0);
  CHECK(line.size() <= std::string_view("PNG text: ").size() + 80 + 3);
}

TEST_CASE("the badge's text: shown on positive evidence and the setting only", "[ai-badge]") {
  using mv::shell::make_ai_badge;
  mv::meta::provenance chatgpt{"ChatGPT", true, "Content Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)"};
  auto b = make_ai_badge(chatgpt, true);
  CHECK(b.shown);
  CHECK(b.title == "Made with ChatGPT");
  CHECK(mv::shell::ai_badge_tooltip(b) ==
        "Made with ChatGPT\nContent Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)");
  CHECK(mv::shell::ai_badge_description(b) ==
        "Made with ChatGPT. Content Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)");

  // The setting off: nothing, whatever the file says.
  CHECK_FALSE(make_ai_badge(chatgpt, false).shown);
  CHECK(mv::shell::ai_badge_tooltip(make_ai_badge(chatgpt, false)).empty());

  // No provenance, or provenance that is not AI: nothing (never a "not AI" mark).
  CHECK_FALSE(make_ai_badge({}, true).shown);
  CHECK_FALSE(make_ai_badge({"Adobe Photoshop 25.0", false, ""}, true).shown);

  // AI by source type with no tool named.
  b = make_ai_badge({"", true, "IPTC digital source type (XMP): AI-generated (trainedAlgorithmicMedia)"}, true);
  CHECK(b.title == "AI-generated");
  CHECK(std::string(mv::shell::kAiBadgeName) == "AI-generated");
  // No evidence line: the title alone.
  CHECK(mv::shell::ai_badge_tooltip(make_ai_badge({"Midjourney", true, ""}, true)) == "Made with Midjourney");
}

TEST_CASE("provenance for the badge: one probe, cached, and a held record answers first", "[ai-badge][meta][store]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  std::atomic<int> probes{0};
  std::atomic<int> reads{0};
  mv::shell::meta_store store(
      [&](std::string_view) -> mv::result<mv::meta::metadata> {
        ++reads;
        mv::meta::metadata m;
        m.s.made_with = "Adobe Firefly";
        m.s.ai_generated = true;
        m.s.ai_evidence = "EXIF Software: Adobe Firefly";
        return m;
      },
      {},  // culls: the default reader, unused here
      [&](std::string_view path) {
        ++probes;
        mv::meta::provenance p;
        if (path == "ai.jpg") p = {"ChatGPT", true, "Content Credentials (C2PA): AI-generated (trainedAlgorithmicMedia)"};
        return p;
      });

  // A miss probes once; a second ask while it runs submits nothing.
  std::atomic<int> ready{0};
  CHECK_FALSE(store.provenance(entry("ai.jpg"), jobs, [&](std::string) { ++ready; }));
  (void)store.provenance(entry("ai.jpg"), jobs, {});
  REQUIRE(wait_for([&] { return ready.load() == 1; }));
  const auto hit = store.peek_provenance(entry("ai.jpg"));
  REQUIRE(hit);
  CHECK(hit->made_with == "ChatGPT");
  CHECK(hit->ai_generated);
  // Cached: no second probe, and never a full read.
  CHECK(store.provenance(entry("ai.jpg"), jobs, {}));
  CHECK(store.probes() == 1);
  CHECK(probes.load() == 1);
  CHECK(reads.load() == 0);

  // A camera file: an answer that says nothing, cached like any other.
  (void)store.provenance(entry("camera.jpg"), jobs, [&](std::string) { ++ready; });
  REQUIRE(wait_for([&] { return ready.load() == 2; }));
  REQUIRE(store.peek_provenance(entry("camera.jpg")));
  CHECK_FALSE(store.peek_provenance(entry("camera.jpg"))->ai_generated);

  // Peeking never probes.
  CHECK_FALSE(store.peek_provenance(entry("other.jpg")));
  CHECK(store.probes() == 2);

  // A held full record (the pane was open) answers without a probe.
  (void)store.get(entry("held.jpg"), jobs, [&](std::string) { ++ready; });
  REQUIRE(wait_for([&] { return ready.load() == 3; }));
  const auto held = store.provenance(entry("held.jpg"), jobs, {});
  REQUIRE(held);
  CHECK(held->made_with == "Adobe Firefly");
  CHECK(held->evidence == "EXIF Software: Adobe Firefly");
  CHECK(store.probes() == 2);

  // A rewrite (new mtime) is a new key; invalidate drops the path's answers.
  CHECK_FALSE(store.peek_provenance(entry("ai.jpg", 200)));
  store.invalidate("ai.jpg");
  CHECK_FALSE(store.peek_provenance(entry("ai.jpg")));
  jobs.shutdown();
}
