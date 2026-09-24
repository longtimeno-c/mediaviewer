// SPDX-License-Identifier: GPL-2.0-or-later
// Layouts, rename templates, safe names, presets (plan/18; PR 18 verify: "a
// rename template yields identical names on Windows and Mac for the same
// card" -- these are pure functions with no OS input, so this test running on
// both is that check).
#include "catch_compat.h"

#include "addons/import/model.h"
#include "addons/import/naming.h"

using namespace mv::import;

namespace {
// 2026-09-21 14:03:05, as a wall clock.
constexpr std::int64_t kSat = 1789999385;
}  // namespace

TEST_CASE("calendar arithmetic is exact and zone-free", "[import][naming]") {
  const civil c = civil_from_seconds(kSat);
  REQUIRE(c.year == 2026);
  REQUIRE(c.month == 9);
  REQUIRE(c.day == 21);
  REQUIRE(c.hour == 14);
  REQUIRE(c.minute == 3);
  REQUIRE(c.second == 5);
  REQUIRE(day_key(0) == "1970-01-01");
  REQUIRE(day_key(951782400) == "2000-02-29");
  std::int64_t days = 0;
  REQUIRE(parse_day("2000-02-29", days));
  REQUIRE(days * 86400 == 951782400);
  REQUIRE_FALSE(parse_day("2000-13-01", days));
  REQUIRE_FALSE(parse_day("20000229", days));
}

TEST_CASE("names are legal everywhere and identical everywhere", "[import][naming]") {
  REQUIRE(sanitize_component("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
  REQUIRE(sanitize_component("trailing. . ") == "trailing");
  REQUIRE(sanitize_component("CON") == "CON_");
  REQUIRE(sanitize_component("lpt1.jpg") == "lpt1_.jpg");
  REQUIRE(sanitize_component("COM10") == "COM10");
  REQUIRE(sanitize_component("") == "_");
  REQUIRE(sanitize_component("Café ✓") == "Café ✓");
}

TEST_CASE("stems keep pairs and sidecars together", "[import][naming]") {
  REQUIRE(stem_of("IMG_0001.CR3") == "IMG_0001");
  REQUIRE(stem_of("IMG_0001.CR3.xmp") == "IMG_0001");
  REQUIRE(stem_of("IMG_0001.xmp") == "IMG_0001");
  REQUIRE(stem_of("my.trip.jpg") == "my.trip");
  REQUIRE(stem_of(".hidden") == ".hidden");
  REQUIRE(with_stem("IMG_0001.CR3.xmp", "IMG_0001", "2026-09-21_0007") == "2026-09-21_0007.CR3.xmp");
  REQUIRE(with_stem("C0001M01.XML", "C0001", "Trip") == "TripM01.XML");
  REQUIRE(with_stem("img_0001.jpg", "IMG_0001", "X") == "X.jpg");  // case-insensitive
  REQUIRE(with_stem(clash_stem("IMG_0001", 2) + ".CR3", clash_stem("IMG_0001", 2), "Y") == "Y.CR3");
  REQUIRE(clash_stem("IMG_0001", 2) == "IMG_0001 (2)");
}

TEST_CASE("every layout", "[import][naming]") {
  preset p;
  layout_input in;
  in.taken = kSat;
  in.camera = "Canon EOS R5";
  in.primary_type = file_type::jpeg;
  in.source_rel_dir = "DCIM/100CANON";
  REQUIRE(layout_folder(p, in) == "2026/2026-09-21");
  p.layout = layout_kind::year_month_day;
  REQUIRE(layout_folder(p, in) == "2026/09/21");
  p.layout = layout_kind::day;
  REQUIRE(layout_folder(p, in) == "2026-09-21");
  p.layout_camera = true;
  REQUIRE(layout_folder(p, in) == "2026-09-21/Canon EOS R5");
  p.layout_type = true;
  REQUIRE(layout_folder(p, in) == "2026-09-21/Canon EOS R5/JPEG");
  in.has_raw = true;  // a RAW+JPEG pair files under RAW, together
  REQUIRE(layout_folder(p, in) == "2026-09-21/Canon EOS R5/RAW");
  p.layout = layout_kind::card;
  p.layout_type = false;
  REQUIRE(layout_folder(p, in) == "DCIM/100CANON");
  p.layout = layout_kind::flat;
  REQUIRE(layout_folder(p, in).empty());
  in.camera = "";
  p.layout = layout_kind::year_day;
  p.layout_camera = true;
  REQUIRE(layout_folder(p, in) == "2026/2026-09-21/Unknown camera");
}

TEST_CASE("rename templates", "[import][naming]") {
  const rename_input in{kSat, "ILCE-7M4", 7, "DSC01234"};
  REQUIRE(render_stem("{date}_{seq}", in) == "2026-09-21_0007");
  REQUIRE(render_stem("{date}_{time}_{camera}_{original}", in) ==
          "2026-09-21_140305_ILCE-7M4_DSC01234");
  REQUIRE(render_stem("{unknown}-{seq}", in) == "{unknown}-0007");
  REQUIRE(render_stem("a/b:{seq}", in) == "a_b_0007");
  REQUIRE(template_uses_seq("x{seq}"));
  REQUIRE_FALSE(template_uses_seq("{date}"));
}

TEST_CASE("presets round-trip and reject nonsense", "[import][preset]") {
  preset p;
  p.name = "Wedding: to NAS + backup";
  p.destination = "/Volumes/NAS/Photos";
  p.backup = "/Volumes/Backup";
  p.layout = layout_kind::year_month_day;
  p.layout_camera = true;
  p.rename = "{date}_{seq}";
  p.types = kTypeRaw | kTypeVideo;
  p.scope = dup_scope::library;
  p.full_verify = false;
  p.selection = selection_mode::date_range;
  p.range_from = "2026-09-01";
  preset q;
  REQUIRE(parse_preset(preset_to_json(p), q));
  REQUIRE(preset_to_json(q) == preset_to_json(p));
  REQUIRE(q.types == (kTypeRaw | kTypeVideo));

  preset r;
  REQUIRE_FALSE(parse_preset(R"({"name":"x","layout":"sideways"})", r));
  REQUIRE_FALSE(parse_preset(R"({"name":""})", r));
  REQUIRE_FALSE(parse_preset(R"({"name":"x","fast":"yes"})", r));
  REQUIRE(parse_preset(R"({"name":"x","future_key":1})", r));  // unknown keys are ignored
  // Overwrite is never an option: there is no key for it to parse into.
  REQUIRE(preset_to_json(preset{}).find("overwrite") == std::string::npos);
}

TEST_CASE("file types by extension", "[import][naming]") {
  REQUIRE(classify("IMG_0001.CR3") == file_type::raw);
  REQUIRE(classify("x.JPG") == file_type::jpeg);
  REQUIRE(classify("x.HIF") == file_type::heic);
  REQUIRE(classify("C0001.MP4") == file_type::video);
  REQUIRE(classify("x.png") == file_type::other);
  REQUIRE(classify("MVI_0001.THM") == file_type::sidecar);
  REQUIRE(classify("GL010001.LRV") == file_type::sidecar);
  REQUIRE(classify("C0001M01.XML") == file_type::sidecar);
  REQUIRE(classify("x.xmp") == file_type::sidecar);
  REQUIRE(classify("INDEX.BDM") == file_type::none);
  REQUIRE(classify("x.jxl") == file_type::none);  // not in D5's v1 set
  REQUIRE(classify("noext") == file_type::none);
}
