// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 12 verify (plan/10): write-then-read round-trips preserve maker notes
// byte-for-byte; a write that dies mid-way leaves the original intact; a
// rating written to a file (or its sidecar) reads back the same wherever the
// file is copied. Fixtures are built here — nothing needs the corpus.
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "codec/exif.h"
#include "corpus.h"
#include "fixtures.h"
#include "io/file.h"
#include "meta/meta.h"
#include "meta/write.h"

namespace {

namespace fs = std::filesystem;
using mv::meta::change;
using mv::meta::write_fields;
using mv::meta::write_target;

std::vector<std::uint8_t> bytes_of(Exiv2::Image& image) {
  Exiv2::BasicIo& io = image.io();
  io.open();
  Exiv2::DataBuf buf = io.read(io.size());
  return {buf.c_data(), buf.c_data() + buf.size()};
}

// A directory of our own per test, removed with it.
struct scratch_dir {
  fs::path dir;
  explicit scratch_dir(const std::string& name) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = fs::temp_directory_path() /
          ("mv_meta_write_" + name + "_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~scratch_dir() {
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
  }
  [[nodiscard]] std::string path(const std::string& file) const { return (dir / file).string(); }
  [[nodiscard]] std::string snapshots() const { return (dir / "snapshots").string(); }
  void write(const std::string& file, const std::vector<std::uint8_t>& bytes) const {
    std::ofstream f(dir / file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  [[nodiscard]] std::vector<std::uint8_t> read(const std::string& file) const {
    auto b = mv::io::read_all((dir / file).string());
    return b ? *b : std::vector<std::uint8_t>{};
  }
};

std::vector<std::uint8_t> flat_jpeg(std::uint32_t w = 64, std::uint32_t h = 48) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3, 128);
  return fixtures::jpeg_rgb(w, h, rgb.data());
}

// Bytes that stand in for a camera's private maker note: opaque, with values
// that would be offsets if anyone tried to interpret them.
const std::vector<std::uint8_t>& opaque_note() {
  static const std::vector<std::uint8_t> note = [] {
    std::vector<std::uint8_t> n;
    for (int i = 0; i < 300; ++i) n.push_back(static_cast<std::uint8_t>((i * 37 + 11) & 0xFF));
    return n;
  }();
  return note;
}

std::vector<std::uint8_t> jpeg_from(const std::vector<std::uint8_t>& base, bool with_exif, bool with_note,
                                    bool with_orientation) {
  auto image = Exiv2::ImageFactory::open(base.data(), base.size());
  Exiv2::ExifData exif;
  if (with_exif) {
    exif["Exif.Image.Make"] = "ACME";
    exif["Exif.Image.Model"] = "Test Camera";
    exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 14:03:22";
    exif["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 250);
    exif["Exif.Photo.ISOSpeedRatings"] = uint16_t{400};
    if (with_orientation) exif["Exif.Image.Orientation"] = uint16_t{1};
  }
  if (with_note) {
    Exiv2::Value::UniquePtr v = Exiv2::Value::create(Exiv2::undefined);
    v->read(opaque_note().data(), opaque_note().size(), Exiv2::littleEndian);
    exif.add(Exiv2::ExifKey("Exif.Photo.MakerNote"), v.get());
  }
  if (!exif.empty()) image->setExifData(exif);
  Exiv2::XmpData xmp;
  xmp["Xmp.dc.creator"] = "Test Author";
  image->setXmpData(xmp);
  image->writeMetadata();
  return bytes_of(*image);
}

// A JPEG with a real Canon maker note (Exiv2 knows its layout).
std::vector<std::uint8_t> canon_jpeg() {
  auto base = flat_jpeg();
  auto image = Exiv2::ImageFactory::open(base.data(), base.size());
  Exiv2::ExifData exif;
  exif["Exif.Image.Make"] = "Canon";
  exif["Exif.Image.Model"] = "Canon EOS R5";
  exif["Exif.Canon.OwnerName"] = "Alice Example";
  exif["Exif.Canon.FirmwareVersion"] = "Firmware Version 1.8.2";
  image->setExifData(exif);
  image->writeMetadata();
  return bytes_of(*image);
}

// Every EXIF value of the group, as bytes, for a byte-for-byte comparison.
std::vector<std::vector<std::uint8_t>> values_of(const std::vector<std::uint8_t>& jpeg, const char* group) {
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  image->readMetadata();
  std::vector<std::vector<std::uint8_t>> out;
  for (const auto& d : image->exifData()) {
    if (d.groupName() != group) continue;
    std::vector<std::uint8_t> v(d.size());
    d.copy(reinterpret_cast<Exiv2::byte*>(v.data()), Exiv2::littleEndian);
    out.push_back(std::move(v));
  }
  return out;
}

// Everything that is not APPn / COM: the picture itself.
std::vector<std::uint8_t> picture_bytes(const std::vector<std::uint8_t>& j) {
  std::vector<std::uint8_t> out;
  std::size_t i = 2;
  out.insert(out.end(), j.begin(), j.begin() + 2);
  while (i + 4 <= j.size() && j[i] == 0xFF) {
    const std::uint8_t m = j[i + 1];
    if (m == 0xDA) {
      out.insert(out.end(), j.begin() + static_cast<std::ptrdiff_t>(i), j.end());
      return out;
    }
    const std::size_t len = (static_cast<std::size_t>(j[i + 2]) << 8) | j[i + 3];
    const bool meta = (m >= 0xE1 && m <= 0xED && m != 0xE2) || m == 0xFE;
    if (!meta) out.insert(out.end(), j.begin() + static_cast<std::ptrdiff_t>(i), j.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
    i += 2 + len;
  }
  return out;
}

template <typename T>
std::string why(const mv::result<T>& r) {
  return r ? "ok" : mv::status_name(r.error());
}

write_fields rating(int r) {
  write_fields f;
  f.rating = r == 0 ? change<int>::remove() : change<int>::to(r);
  return f;
}
write_fields comment(const std::string& c) {
  write_fields f;
  f.comment = c.empty() ? change<std::string>::remove() : change<std::string>::to(c);
  return f;
}
write_fields orientation(int o) {
  write_fields f;
  f.orientation = change<int>::to(o);
  return f;
}

}  // namespace

TEST_CASE("a rating on a plain JPEG is written in the file and reads back", "[meta][write]") {
  scratch_dir d("rating");
  d.write("a.jpg", jpeg_from(flat_jpeg(), true, false, true));

  auto target = mv::meta::write_target_for(d.path("a.jpg"));
  REQUIRE(target);
  CHECK(*target == write_target::in_file);

  const auto before = d.read("a.jpg");
  auto r = mv::meta::write(d.path("a.jpg"), rating(4), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::in_file);
  CHECK_FALSE(fs::exists(mv::meta::sidecar_path_for(d.path("a.jpg"))));  // nothing beside it

  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK(m->s.rating == 4);
  CHECK(m->s.camera == "ACME Test Camera");  // the rest of the file is still there
  CHECK(m->s.iso == "ISO 400");
  CHECK(picture_bytes(d.read("a.jpg")) == picture_bytes(before));  // the picture is not touched
  CHECK(d.read("a.jpg") != before);

  // 0 removes it; "unrated" is the absence of a tag.
  REQUIRE(mv::meta::write(d.path("a.jpg"), rating(0), d.snapshots()));
  m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK(m->s.rating == 0);
  bool has_rating_tag = false;
  for (const auto& p : m->properties) has_rating_tag |= p.raw_tag == "Xmp.xmp.Rating";
  CHECK_FALSE(has_rating_tag);
}

TEST_CASE("a rating leaves the EXIF block alone", "[meta][write]") {
  // XMP carries the rating: the EXIF segment is not rewritten at all.
  scratch_dir d("rating_exif");
  const auto original = jpeg_from(flat_jpeg(), true, true, true);
  d.write("a.jpg", original);
  REQUIRE(mv::meta::write(d.path("a.jpg"), rating(3), d.snapshots()));
  const auto after = d.read("a.jpg");

  const auto exif_a = mv::codec::find_jpeg_exif(original);
  const auto exif_b = mv::codec::find_jpeg_exif(after);
  REQUIRE(exif_a);
  REQUIRE(exif_b);
  REQUIRE(exif_a->size == exif_b->size);
  CHECK(std::equal(original.begin() + static_cast<std::ptrdiff_t>(exif_a->offset),
                   original.begin() + static_cast<std::ptrdiff_t>(exif_a->offset + exif_a->size),
                   after.begin() + static_cast<std::ptrdiff_t>(exif_b->offset)));
}

TEST_CASE("maker notes survive a comment and an orientation byte for byte", "[meta][write]") {
  scratch_dir d("makernote");

  SECTION("an opaque note") {
    d.write("a.jpg", jpeg_from(flat_jpeg(), true, true, false));  // no Orientation: IFD0 must grow
    const auto before = values_of(d.read("a.jpg"), "Photo");
    write_fields f;
    f.comment = change<std::string>::to("Shot on the pier");
    f.orientation = change<int>::to(6);
    REQUIRE(mv::meta::write(d.path("a.jpg"), f, d.snapshots()));

    auto image_bytes = d.read("a.jpg");
    auto image = Exiv2::ImageFactory::open(image_bytes.data(), image_bytes.size());
    image->readMetadata();
    const auto it = image->exifData().findKey(Exiv2::ExifKey("Exif.Photo.MakerNote"));
    REQUIRE(it != image->exifData().end());
    std::vector<std::uint8_t> note(it->size());
    it->copy(reinterpret_cast<Exiv2::byte*>(note.data()), Exiv2::littleEndian);
    CHECK(note == opaque_note());

    auto m = mv::meta::read(d.path("a.jpg"));
    REQUIRE(m);
    CHECK(m->s.comment == "Shot on the pier");
    CHECK(m->s.orientation == 6);
    CHECK(mv::codec::jpeg_orientation(image_bytes) == 6);
    (void)before;
  }

  SECTION("a Canon note Exiv2 understands") {
    const auto original = canon_jpeg();
    d.write("a.jpg", original);
    const auto before = values_of(original, "Canon");
    REQUIRE_FALSE(before.empty());
    write_fields f;
    f.comment = change<std::string>::to("hello");
    f.orientation = change<int>::to(3);
    REQUIRE(mv::meta::write(d.path("a.jpg"), f, d.snapshots()));
    CHECK(values_of(d.read("a.jpg"), "Canon") == before);
  }
}

TEST_CASE("a JPEG with no EXIF takes an orientation", "[meta][write]") {
  // PR 10 refused this ("growing IFD0 is PR 12's writer's job").
  scratch_dir d("noexif");
  d.write("a.jpg", flat_jpeg());
  REQUIRE(mv::meta::write(d.path("a.jpg"), orientation(8), d.snapshots()));
  CHECK(mv::codec::jpeg_orientation(d.read("a.jpg")) == 8);
  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK(m->s.orientation == 8);
}

TEST_CASE("comments round-trip, ASCII and not", "[meta][write]") {
  scratch_dir d("comment");
  d.write("a.jpg", jpeg_from(flat_jpeg(), true, false, true));

  for (const std::string& text : {std::string("plain ascii"), std::string("caf\xC3\xA9 \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"),
                                 std::string("two\nlines")}) {
    INFO(text);
    REQUIRE(mv::meta::write(d.path("a.jpg"), comment(text), d.snapshots()));
    auto m = mv::meta::read(d.path("a.jpg"));
    REQUIRE(m);
    CHECK(m->s.comment == text);
  }
  REQUIRE(mv::meta::write(d.path("a.jpg"), comment(""), d.snapshots()));
  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK(m->s.comment.empty());
}

TEST_CASE("values that cannot be written are refused before anything is touched", "[meta][write]") {
  scratch_dir d("invalid");
  const auto original = jpeg_from(flat_jpeg(), true, false, true);
  d.write("a.jpg", original);

  write_fields bad;
  bad.rating = change<int>::to(6);
  auto r = mv::meta::write(d.path("a.jpg"), bad, d.snapshots());
  CHECK_FALSE(r);
  CHECK(r.error() == mv::status::invalid_arg);
  bad = {};
  bad.orientation = change<int>::to(9);
  CHECK_FALSE(mv::meta::write(d.path("a.jpg"), bad, d.snapshots()));
  bad = {};
  bad.comment = change<std::string>::to(std::string("bad \xFF utf8"));
  CHECK_FALSE(mv::meta::write(d.path("a.jpg"), bad, d.snapshots()));
  bad.comment = change<std::string>::to(std::string(mv::meta::kMaxCommentBytes + 1, 'x'));
  CHECK_FALSE(mv::meta::write(d.path("a.jpg"), bad, d.snapshots()));
  CHECK_FALSE(mv::meta::write(d.path("a.jpg"), write_fields{}, d.snapshots()));  // nothing to do
  CHECK(d.read("a.jpg") == original);
}

TEST_CASE("anything but a plain JPEG gets a sidecar and the original is never opened", "[meta][write]") {
  scratch_dir d("sidecar");
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 200);
  const auto png = fixtures::png_rgba(16, 16, rgba.data());
  d.write("IMG_0001.png", png);

  auto target = mv::meta::write_target_for(d.path("IMG_0001.png"));
  REQUIRE(target);
  CHECK(*target == write_target::sidecar);

  write_fields f;
  f.rating = change<int>::to(5);
  f.comment = change<std::string>::to("sidecar words");
  auto r = mv::meta::write(d.path("IMG_0001.png"), f, d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::sidecar);
  CHECK(r->sidecar_path == d.path("IMG_0001.xmp"));
  CHECK(fs::exists(d.path("IMG_0001.xmp")));
  CHECK(d.read("IMG_0001.png") == png);  // byte for byte

  auto m = mv::meta::read(d.path("IMG_0001.png"));
  REQUIRE(m);
  CHECK(m->s.rating == 5);
  CHECK(m->s.comment == "sidecar words");

  // The whole file goes when its last field does.
  write_fields clear;
  clear.rating = change<int>::remove();
  clear.comment = change<std::string>::remove();
  REQUIRE(mv::meta::write(d.path("IMG_0001.png"), clear, d.snapshots()));
  CHECK_FALSE(fs::exists(d.path("IMG_0001.xmp")));
  CHECK(d.read("IMG_0001.png") == png);
}

TEST_CASE("a sidecar somebody else wrote keeps everything they put in it", "[meta][write]") {
  scratch_dir d("merge");
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 90);
  d.write("IMG_7.png", fixtures::png_rgba(16, 16, rgba.data()));

  // A Lightroom-style sidecar: a bag, a langAlt, a struct-ish namespace of its own.
  const std::string theirs =
      "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
      " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
      "  <rdf:Description rdf:about=\"\"\n"
      "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
      "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
      "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
      "   xmp:Rating=\"2\" xmp:CreatorTool=\"Adobe Lightroom\" crs:Exposure2012=\"+0.55\" crs:Version=\"15.0\">\n"
      "   <dc:subject><rdf:Bag><rdf:li>pier</rdf:li><rdf:li>sunset</rdf:li></rdf:Bag></dc:subject>\n"
      "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">The Pier</rdf:li></rdf:Alt></dc:title>\n"
      "  </rdf:Description>\n"
      " </rdf:RDF>\n"
      "</x:xmpmeta>\n"
      "<?xpacket end=\"w\"?>";
  d.write("IMG_7.xmp", std::vector<std::uint8_t>(theirs.begin(), theirs.end()));

  auto m = mv::meta::read(d.path("IMG_7.png"));
  REQUIRE(m);
  CHECK(m->s.rating == 2);  // read from their sidecar

  REQUIRE(mv::meta::write(d.path("IMG_7.png"), rating(5), d.snapshots()));
  m = mv::meta::read(d.path("IMG_7.png"));
  REQUIRE(m);
  CHECK(m->s.rating == 5);
  const auto has = [&](const char* tag) {
    for (const auto& p : m->properties) {
      if (p.raw_tag == tag) return true;
    }
    return false;
  };
  CHECK(has("Xmp.dc.subject"));
  CHECK(has("Xmp.dc.title"));
  CHECK(has("Xmp.xmp.CreatorTool"));
  CHECK(has("Xmp.crs.Exposure2012"));
}

TEST_CASE("a sidecar that is not XMP is never overwritten", "[meta][write]") {
  scratch_dir d("badsidecar");
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 90);
  d.write("IMG_8.png", fixtures::png_rgba(16, 16, rgba.data()));
  const std::string junk = "this is somebody's notes, not XMP";
  d.write("IMG_8.xmp", std::vector<std::uint8_t>(junk.begin(), junk.end()));
  auto r = mv::meta::write(d.path("IMG_8.png"), rating(3), d.snapshots());
  CHECK_FALSE(r);
  const auto kept = d.read("IMG_8.xmp");
  CHECK(std::string(kept.begin(), kept.end()) == junk);
}

TEST_CASE("a JPEG that in-place editing would harm gets a sidecar instead", "[meta][write]") {
  scratch_dir d("harm");
  const auto plain = jpeg_from(flat_jpeg(), true, false, true);

  SECTION("a Multi-Picture table") {
    // APP2 "MPF\0": its offsets point into the file, so a resized header breaks them.
    std::vector<std::uint8_t> mpf = {0xFF, 0xE2, 0x00, 0x10, 'M', 'P', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto j = plain;
    j.insert(j.begin() + 2, mpf.begin(), mpf.end());
    d.write("a.jpg", j);
    auto t = mv::meta::write_target_for(d.path("a.jpg"));
    REQUIRE(t);
    CHECK(*t == write_target::sidecar);
    REQUIRE(mv::meta::write(d.path("a.jpg"), rating(2), d.snapshots()));
    CHECK(d.read("a.jpg") == j);
    auto m = mv::meta::read(d.path("a.jpg"));
    REQUIRE(m);
    CHECK(m->s.rating == 2);
  }

  SECTION("bytes after the final EOI") {
    auto j = plain;
    for (int i = 0; i < 100; ++i) j.push_back(static_cast<std::uint8_t>(i));  // a motion photo's video
    d.write("a.jpg", j);
    auto t = mv::meta::write_target_for(d.path("a.jpg"));
    REQUIRE(t);
    CHECK(*t == write_target::sidecar);
    REQUIRE(mv::meta::write(d.path("a.jpg"), rating(1), d.snapshots()));
    CHECK(d.read("a.jpg") == j);
  }
}

TEST_CASE("a sidecar beside a JPEG is kept in step with the file", "[meta][write]") {
  // The reader lets a sidecar win, so a stale one would shadow a new rating.
  scratch_dir d("step");
  d.write("a.jpg", jpeg_from(flat_jpeg(), true, false, true));
  const std::string theirs =
      "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"1\"/>"
      "</rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";
  d.write("a.xmp", std::vector<std::uint8_t>(theirs.begin(), theirs.end()));

  auto r = mv::meta::write(d.path("a.jpg"), rating(4), d.snapshots());
  REQUIRE(r);
  CHECK(r->target == write_target::in_file);
  CHECK(r->sidecar_touched);
  auto m = mv::meta::read(d.path("a.jpg"));
  REQUIRE(m);
  CHECK(m->s.rating == 4);
}

TEST_CASE("revert puts the fields back to the session's first snapshot", "[meta][write][snapshot]") {
  scratch_dir d("revert");
  mv::meta::reset_snapshot_session();

  SECTION("in a JPEG") {
    auto base = jpeg_from(flat_jpeg(), true, false, true);
    {
      auto image = Exiv2::ImageFactory::open(base.data(), base.size());
      image->readMetadata();
      Exiv2::ExifData e = image->exifData();
      e["Exif.Photo.UserComment"] = "charset=Ascii original words";
      image->setExifData(e);
      Exiv2::XmpData x = image->xmpData();
      x["Xmp.xmp.Rating"] = "2";
      image->setXmpData(x);
      image->writeMetadata();
      base = bytes_of(*image);
    }
    d.write("a.jpg", base);
    CHECK_FALSE(mv::meta::has_snapshot(d.path("a.jpg"), d.snapshots()));

    write_fields f;
    f.rating = change<int>::to(5);
    f.comment = change<std::string>::to("changed");
    f.orientation = change<int>::to(6);
    const auto w1 = mv::meta::write(d.path("a.jpg"), f, d.snapshots());
    INFO(why(w1));
    REQUIRE(w1);
    REQUIRE(mv::meta::write(d.path("a.jpg"), rating(1), d.snapshots()));  // a second write: no new snapshot
    CHECK(mv::meta::has_snapshot(d.path("a.jpg"), d.snapshots()));

    REQUIRE(mv::meta::revert(d.path("a.jpg"), d.snapshots()));
    auto m = mv::meta::read(d.path("a.jpg"));
    REQUIRE(m);
    CHECK(m->s.rating == 2);
    CHECK(m->s.comment == "original words");
    CHECK(m->s.orientation == 1);
  }

  SECTION("in a sidecar it created") {
    std::vector<std::uint8_t> rgba(16 * 16 * 4, 90);
    d.write("b.png", fixtures::png_rgba(16, 16, rgba.data()));
    REQUIRE(mv::meta::write(d.path("b.png"), rating(4), d.snapshots()));
    CHECK(fs::exists(d.path("b.xmp")));
    const auto rv = mv::meta::revert(d.path("b.png"), d.snapshots());
    INFO(why(rv));
    REQUIRE(rv);
    CHECK_FALSE(fs::exists(d.path("b.xmp")));  // there was none, so there is none
  }

  SECTION("with no snapshot there is nothing to revert") {
    d.write("c.jpg", flat_jpeg());
    CHECK_FALSE(mv::meta::revert(d.path("c.jpg"), d.snapshots()));
  }
}

TEST_CASE("a write that cannot be swapped in leaves the original whole", "[meta][write][crash]") {
#if defined(_WIN32)
  SKIP("directory permissions differ on Windows; the kill test below is the crash check there");
#else
  scratch_dir d("readonly");
  const auto original = jpeg_from(flat_jpeg(), true, false, true);
  d.write("a.jpg", original);
  fs::permissions(d.dir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  if (::access(d.dir.c_str(), W_OK) == 0) {
    fs::permissions(d.dir, fs::perms::owner_all, fs::perm_options::add);
    SKIP("running with rights that ignore directory permissions");
  }
  // The snapshot store lives elsewhere, so only the swap can fail.
  scratch_dir store("readonly_store");
  auto r = mv::meta::write(d.path("a.jpg"), comment("never lands"), store.path("snaps"));
  fs::permissions(d.dir, fs::perms::owner_all, fs::perm_options::add);
  CHECK_FALSE(r);
  CHECK(d.read("a.jpg") == original);
  for (const auto& e : fs::directory_iterator(d.dir)) CHECK(e.path().filename() == "a.jpg");  // no litter
#endif
}

#if !defined(_WIN32)
TEST_CASE("a process killed mid-write leaves the original or the finished file, never half", "[meta][write][crash]") {
  scratch_dir d("kill");
  // Large enough that the write takes a moment: the kill lands at a different
  // point each round.
  std::vector<std::uint8_t> big_rgb(2400u * 1800u * 3u);
  for (std::size_t i = 0; i < big_rgb.size(); ++i) big_rgb[i] = static_cast<std::uint8_t>((i * 31 + (i >> 9)) & 0xFF);
  const auto original = jpeg_from(fixtures::jpeg_rgb(2400, 1800, big_rgb.data()), true, true, true);
  d.write("a.jpg", original);
  const std::string path = d.path("a.jpg");
  const std::string store = d.snapshots();

  int old_seen = 0, new_seen = 0;
  for (int round = 0; round < 24; ++round) {
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      // Child: write until killed. Never returns into the test runner.
      for (int i = 0;; ++i) (void)mv::meta::write(path, comment("round " + std::to_string(i)), store);
    }
    ::usleep(static_cast<useconds_t>(5000 + (round * 5300) % 140000));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    const auto now = d.read("a.jpg");
    REQUIRE_FALSE(now.empty());
    // Whole and readable, with the same picture and the same maker note.
    auto image = Exiv2::ImageFactory::open(now.data(), now.size());
    REQUIRE(image);
    image->readMetadata();
    CHECK(picture_bytes(now) == picture_bytes(original));
    const auto it = image->exifData().findKey(Exiv2::ExifKey("Exif.Photo.MakerNote"));
    REQUIRE(it != image->exifData().end());
    std::vector<std::uint8_t> note(it->size());
    it->copy(reinterpret_cast<Exiv2::byte*>(note.data()), Exiv2::littleEndian);
    CHECK(note == opaque_note());
    (now == original ? old_seen : new_seen)++;
  }
  WARN("kill test: original " << old_seen << ", rewritten " << new_seen);
  // Only temp files a killed child left behind may be extra; none is ever the target.
  for (const auto& e : fs::directory_iterator(d.dir)) {
    const std::string name = e.path().filename().string();
    CHECK((name == "a.jpg" || name == "snapshots" || name.find(".mvtmp") != std::string::npos));
  }
}
#endif

#if !defined(_WIN32)
TEST_CASE("a process killed mid-sidecar never leaves a half sidecar", "[meta][write][crash]") {
  scratch_dir d("kill_side");
  std::vector<std::uint8_t> rgba(64 * 64 * 4, 33);
  const auto png = fixtures::png_rgba(64, 64, rgba.data());
  d.write("IMG_9.png", png);
  const std::string path = d.path("IMG_9.png");
  const std::string store = d.snapshots();

  for (int round = 0; round < 40; ++round) {
    fs::remove(d.path("IMG_9.xmp"));  // every round starts with no sidecar: the create path is the one at risk
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      for (int i = 0;; ++i) {
        (void)mv::meta::write(path, rating(1 + i % 5), store);
        fs::remove(d.path("IMG_9.xmp"));
      }
    }
    ::usleep(static_cast<useconds_t>(200 + (round * 373) % 6000));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    CHECK(d.read("IMG_9.png") == png);
    if (fs::exists(d.path("IMG_9.xmp"))) {
      // Whatever is there under the sidecar's name is a whole, readable sidecar.
      auto m = mv::meta::read(path);
      REQUIRE(m);
      CHECK(m->s.rating >= 1);
      CHECK(m->s.rating <= 5);
    }
  }
}
#endif

TEST_CASE("the same write on the same file gives the same bytes anywhere", "[meta][write][platform]") {
  // Rating a file on one platform and reading it on the other only holds if the
  // bytes do not depend on the machine: no clock, no path, no address.
  scratch_dir a("same_a"), b("same_b");
  const auto jpeg = jpeg_from(flat_jpeg(), true, true, true);
  a.write("x.jpg", jpeg);
  b.write("y.jpg", jpeg);
  write_fields f;
  f.rating = change<int>::to(3);
  f.comment = change<std::string>::to("same everywhere \xE2\x9C\x93");
  REQUIRE(mv::meta::write(a.path("x.jpg"), f, a.snapshots()));
  REQUIRE(mv::meta::write(b.path("y.jpg"), f, b.snapshots()));
  CHECK(a.read("x.jpg") == b.read("y.jpg"));

  std::vector<std::uint8_t> rgba(16 * 16 * 4, 60);
  const auto png = fixtures::png_rgba(16, 16, rgba.data());
  a.write("IMG_1.png", png);
  b.write("IMG_1.png", png);
  REQUIRE(mv::meta::write(a.path("IMG_1.png"), f, a.snapshots()));
  REQUIRE(mv::meta::write(b.path("IMG_1.png"), f, b.snapshots()));
  CHECK(a.read("IMG_1.xmp") == b.read("IMG_1.xmp"));
}

TEST_CASE("sidecar names and rating text", "[meta][write]") {
  CHECK(mv::meta::sidecar_path_for("/photos/IMG_1234.CR2") == "/photos/IMG_1234.xmp");
  CHECK(mv::meta::sidecar_path_for("C:\\photos\\IMG_1234.CR2") == "C:\\photos\\IMG_1234.xmp");
  CHECK(mv::meta::sidecar_path_for("/photos/a.b.c.mov") == "/photos/a.b.c.xmp");
  CHECK(mv::meta::sidecar_path_for("/photos/noext") == "/photos/noext.xmp");
  CHECK(mv::meta::sidecar_path_for("/photos.d/noext") == "/photos.d/noext.xmp");
  CHECK(mv::meta::sidecar_path_for("/photos/.hidden") == "/photos/.hidden.xmp");

  CHECK(mv::meta::format_rating(0).empty());
  CHECK(mv::meta::format_rating(-1) == "Rejected");
  CHECK(mv::meta::format_rating(3) == "\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x86\xE2\x98\x86");
  CHECK(mv::meta::format_rating(5) == "\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85");
}

TEST_CASE("the summary card carries the rating and comment rows", "[meta][write][present]") {
  mv::meta::metadata m;
  m.s.rating = 4;
  m.s.comment = "kept";
  const auto rows = mv::meta::summary_rows(m);
  const auto value = [&](const char* label) -> std::string {
    for (const auto& r : rows) {
      if (r.label == label) return r.value;
    }
    return "<missing>";
  };
  CHECK(value("Rating") == mv::meta::format_rating(4));
  CHECK(value("Comment") == "kept");
  mv::meta::metadata clip;
  clip.is_clip = true;
  bool has = false;
  for (const auto& r : mv::meta::summary_rows(clip)) has |= r.label == "Rating";
  CHECK(has);  // a clip's rating lives in its sidecar
}

// ---- The real corpus (tools/testmedia/raw: five cameras, CC0, not in git) ----

namespace {

std::vector<std::string> raw_corpus() {
  std::vector<std::string> found;
  for (const char* name : {"canon_eos7dmk2.cr2", "nikon_d7500.nef", "sony_ilce7rm3.arw", "canon_eosr6.cr3",
                           "pentax_k50.dng"}) {
    const std::string p = corpus::path_of((std::string("raw/") + name).c_str());
    if (!p.empty()) found.push_back(p);
  }
  return found;
}

}  // namespace

TEST_CASE("real RAW files are never touched: the rating goes to a sidecar and reads back", "[meta][write][raw]") {
  const auto files = raw_corpus();
  if (files.empty()) {
    if (corpus::corpus_required()) FAIL("MV_REQUIRE_CORPUS is set and tools/testmedia/raw is empty");
    SKIP("tools/testmedia/raw not fetched. This test proved NOTHING.");
  }
  scratch_dir d("rawcorpus");
  for (const std::string& src : files) {
    const std::string name = fs::path(src).filename().string();
    INFO(name);
    fs::copy_file(src, d.dir / name);
    const auto original = d.read(name);
    REQUIRE_FALSE(original.empty());

    auto target = mv::meta::write_target_for(d.path(name));
    REQUIRE(target);
    CHECK(*target == write_target::sidecar);

    write_fields f;
    f.rating = change<int>::to(4);
    f.comment = change<std::string>::to("field notes \xE2\x80\x94 dawn");
    f.orientation = change<int>::to(6);
    const auto r = mv::meta::write(d.path(name), f, d.snapshots());
    INFO(why(r));
    REQUIRE(r);
    CHECK(d.read(name) == original);  // byte for byte

    auto m = mv::meta::read(d.path(name));
    REQUIRE(m);
    CHECK(m->s.rating == 4);
    CHECK(m->s.comment == "field notes \xE2\x80\x94 dawn");
    CHECK(m->s.camera.size() > 0);  // the camera's own metadata is still read

    REQUIRE(mv::meta::revert(d.path(name), d.snapshots()));
    CHECK(d.read(name) == original);
    CHECK_FALSE(fs::exists(mv::meta::sidecar_path_for(d.path(name))));
  }
}

TEST_CASE("real camera maker notes survive an in-file write value for value", "[meta][write][raw]") {
  // The EXIF of each real RAW (maker note included, as Exiv2 carries it into a
  // JPEG) becomes the EXIF of a JPEG; a comment, an orientation and a rating
  // are written into that; every maker-note value must come back identical.
  const auto files = raw_corpus();
  if (files.empty()) {
    if (corpus::corpus_required()) FAIL("MV_REQUIRE_CORPUS is set and tools/testmedia/raw is empty");
    SKIP("tools/testmedia/raw not fetched. This test proved NOTHING.");
  }
  scratch_dir d("makernote_corpus");
  int with_note = 0;
  for (const std::string& src : files) {
    const std::string name = fs::path(src).filename().string();
    INFO(name);
    auto raw = mv::io::read_all(src);
    REQUIRE(raw);
    const auto carried = mv::meta::read_carried(*raw);
    if (carried.exif.empty() || carried.exif.size() > 65533 - 8) continue;

    auto jpeg = flat_jpeg();
    std::vector<std::uint8_t> app1 = {0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), carried.exif.begin(), carried.exif.end());
    const std::size_t len = app1.size() - 2;
    app1[2] = static_cast<std::uint8_t>(len >> 8);
    app1[3] = static_cast<std::uint8_t>(len & 0xFF);
    jpeg.insert(jpeg.begin() + 2, app1.begin(), app1.end());
    d.write(name + ".jpg", jpeg);

    // Every group that is not the plain TIFF/EXIF ones is a maker note's.
    const auto groups_of = [&](const std::vector<std::uint8_t>& j) {
      std::map<std::string, std::vector<std::vector<std::uint8_t>>> out;
      auto image = Exiv2::ImageFactory::open(j.data(), j.size());
      image->readMetadata();
      for (const auto& e : image->exifData()) {
        const std::string g = e.groupName();
        if (g == "Image" || g == "Photo" || g == "GPSInfo" || g == "Iop" || g == "Thumbnail") continue;
        std::vector<std::uint8_t> v(e.size());
        e.copy(reinterpret_cast<Exiv2::byte*>(v.data()), Exiv2::littleEndian);
        out[g + "." + e.tagName()].push_back(std::move(v));
      }
      return out;
    };
    const auto before = groups_of(d.read(name + ".jpg"));
    if (before.empty()) continue;  // this camera's note did not survive the carry: nothing to test
    ++with_note;

    write_fields f;
    f.rating = change<int>::to(5);
    f.comment = change<std::string>::to("caf\xC3\xA9");
    f.orientation = change<int>::to(8);
    const auto r = mv::meta::write(d.path(name + ".jpg"), f, d.snapshots());
    INFO(why(r));
    REQUIRE(r);
    // If Exiv2 would not carry it faithfully, the writer says so by refusing
    // the file and using the sidecar for rating / comment (orientation is
    // refused outright). It never writes a JPEG whose note differs.
    CHECK(r->target == write_target::in_file);
    auto after = groups_of(d.read(name + ".jpg"));
    for (auto& kv : after) {
      auto& v = kv.second;
      std::sort(v.begin(), v.end());
    }
    auto want = before;
    for (auto& kv : want) std::sort(kv.second.begin(), kv.second.end());
    CHECK(after == want);
  }
  WARN("cameras with a maker note that reached the JPEG: " << with_note << " of " << files.size());
  CHECK(with_note > 0);
}
