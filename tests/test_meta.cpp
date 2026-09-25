// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 9 verify (plan/10): a JPEG with EXIF, a PNG with XMP, a HEIC and an MP4
// all populate; missing metadata renders as empty fields, never an error.
// Fixtures are built in the test — nothing here needs a corpus.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <cstring>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.h"
#include "meta/af.h"
#include "meta/meta.h"
#include "meta/tables.h"

namespace {

namespace fs = std::filesystem;

struct temp_file {
  fs::path path;
  explicit temp_file(const std::string& name) {
    path = fs::temp_directory_path() / ("mv_meta_test_" + name);
  }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  [[nodiscard]] std::string utf8() const { return path.string(); }
  void write(const std::vector<std::uint8_t>& bytes) const {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

std::vector<std::uint8_t> flat_rgb(std::uint32_t w, std::uint32_t h) {
  return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 3, 128);
}

std::vector<std::uint8_t> bytes_of(Exiv2::Image& image) {
  Exiv2::BasicIo& io = image.io();
  io.open();
  Exiv2::DataBuf buf = io.read(io.size());
  return {buf.c_data(), buf.c_data() + buf.size()};
}

// A real JPEG carrying real EXIF, written the way a camera would.
std::vector<std::uint8_t> jpeg_with_exif() {
  auto rgb = flat_rgb(64, 48);
  auto jpeg = fixtures::jpeg_rgb(64, 48, rgb.data());
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  Exiv2::ExifData exif;
  exif["Exif.Image.Make"] = "Canon";
  exif["Exif.Image.Model"] = "Canon EOS R5";
  exif["Exif.Image.Orientation"] = uint16_t{6};
  exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 14:03:22";
  exif["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 250);
  exif["Exif.Photo.FNumber"] = Exiv2::URational(28, 10);
  exif["Exif.Photo.ISOSpeedRatings"] = uint16_t{400};
  exif["Exif.Photo.FocalLength"] = Exiv2::URational(85, 1);
  exif["Exif.Photo.LensModel"] = "RF85mm F1.2 L USM";
  exif["Exif.Photo.SubjectArea"] = "32 24 16 12";
  exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
  exif["Exif.GPSInfo.GPSLatitude"] = "48/1 51/1 3012/100";
  exif["Exif.GPSInfo.GPSLongitudeRef"] = "E";
  exif["Exif.GPSInfo.GPSLongitude"] = "2/1 17/1 4012/100";
  image->setExifData(exif);
  image->writeMetadata();
  return bytes_of(*image);
}

std::vector<std::uint8_t> png_with_xmp() {
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 200);
  auto png = fixtures::png_rgba(16, 16, rgba.data());
  auto image = Exiv2::ImageFactory::open(png.data(), png.size());
  Exiv2::XmpData xmp;
  xmp["Xmp.dc.creator"] = "Test Author";
  xmp["Xmp.xmp.CreateDate"] = "2023-12-24T18:30:00";
  xmp["Xmp.xmp.Rating"] = "4";
  image->setXmpData(xmp);
  image->writeMetadata();
  return bytes_of(*image);
}

const mv::meta::property* find_tag(const mv::meta::metadata& m, const char* raw_tag) {
  for (const auto& p : m.properties) {
    if (p.raw_tag == raw_tag) return &p;
  }
  return nullptr;
}

// A real (tiny) MP4: ten grey MPEG-4 frames at 30 fps and one AAC packet, made
// with FFmpeg's built-in encoders and muxer — no external clip, no corpus.
bool encode_into(AVFormatContext* oc, AVCodecContext* enc, AVStream* st, AVFrame* frame) {
  if (avcodec_send_frame(enc, frame) < 0) return false;
  AVPacket* pkt = av_packet_alloc();
  bool ok = true;
  for (;;) {
    const int r = avcodec_receive_packet(enc, pkt);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
    if (r < 0) {
      ok = false;
      break;
    }
    av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
    pkt->stream_index = st->index;
    if (av_interleaved_write_frame(oc, pkt) < 0) ok = false;
  }
  av_packet_free(&pkt);
  return ok;
}

bool write_mp4(const std::string& path, const char* muxer = "mp4") {
  AVFormatContext* oc = nullptr;
  if (avformat_alloc_output_context2(&oc, nullptr, muxer, path.c_str()) < 0) return false;
  const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!vcodec || !acodec) return false;

  AVStream* vs = avformat_new_stream(oc, nullptr);
  AVCodecContext* venc = avcodec_alloc_context3(vcodec);
  venc->width = 320;
  venc->height = 180;
  venc->pix_fmt = AV_PIX_FMT_YUV420P;
  venc->time_base = {1, 30};
  venc->framerate = {30, 1};
  venc->gop_size = 10;
  venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVStream* as = avformat_new_stream(oc, nullptr);
  AVCodecContext* aenc = avcodec_alloc_context3(acodec);
  aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
  aenc->sample_rate = 44100;
  av_channel_layout_default(&aenc->ch_layout, 2);
  aenc->time_base = {1, 44100};
  aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  bool ok = avcodec_open2(venc, vcodec, nullptr) >= 0 && avcodec_open2(aenc, acodec, nullptr) >= 0 &&
            avcodec_parameters_from_context(vs->codecpar, venc) >= 0 &&
            avcodec_parameters_from_context(as->codecpar, aenc) >= 0;
  vs->time_base = venc->time_base;
  as->time_base = aenc->time_base;
  av_dict_set(&oc->metadata, "title", "Fixture clip", 0);
  av_dict_set(&oc->metadata, "creation_time", "2024-05-01T14:03:22.000000Z", 0);
  av_dict_set(&as->metadata, "language", "eng", 0);
  ok = ok && avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0 &&
       avformat_write_header(oc, nullptr) >= 0;

  if (ok) {
    AVFrame* vf = av_frame_alloc();
    vf->format = venc->pix_fmt;
    vf->width = venc->width;
    vf->height = venc->height;
    ok = av_frame_get_buffer(vf, 0) >= 0;
    for (int i = 0; ok && i < 10; ++i) {
      av_frame_make_writable(vf);
      for (int y = 0; y < vf->height; ++y) std::memset(vf->data[0] + y * vf->linesize[0], 128, static_cast<size_t>(vf->width));
      for (int y = 0; y < vf->height / 2; ++y) {
        std::memset(vf->data[1] + y * vf->linesize[1], 128, static_cast<size_t>(vf->width / 2));
        std::memset(vf->data[2] + y * vf->linesize[2], 128, static_cast<size_t>(vf->width / 2));
      }
      vf->pts = i;
      ok = encode_into(oc, venc, vs, vf);
    }
    ok = ok && encode_into(oc, venc, vs, nullptr);
    av_frame_free(&vf);

    AVFrame* af = av_frame_alloc();
    af->format = aenc->sample_fmt;
    af->nb_samples = aenc->frame_size;
    av_channel_layout_copy(&af->ch_layout, &aenc->ch_layout);
    ok = ok && av_frame_get_buffer(af, 0) >= 0;
    for (int i = 0; ok && i < 2; ++i) {
      av_frame_make_writable(af);
      for (int c = 0; c < 2; ++c) std::memset(af->data[c], 0, static_cast<size_t>(af->nb_samples) * sizeof(float));
      af->pts = static_cast<int64_t>(i) * af->nb_samples;
      ok = encode_into(oc, aenc, as, af);
    }
    ok = ok && encode_into(oc, aenc, as, nullptr);
    av_frame_free(&af);
    ok = ok && av_write_trailer(oc) >= 0;
  }
  if (oc->pb) avio_closep(&oc->pb);
  avcodec_free_context(&venc);
  avcodec_free_context(&aenc);
  avformat_free_context(oc);
  return ok;
}

}  // namespace

TEST_CASE("a JPEG with EXIF fills the summary card", "[meta]") {
  temp_file f("exif.jpg");
  f.write(jpeg_with_exif());
  auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  const auto& s = m->s;
  CHECK(s.width == 64);
  CHECK(s.height == 48);
  CHECK(s.format == "JPEG");
  CHECK(s.camera == "Canon EOS R5");  // "Canon Canon EOS R5" would be the bug
  CHECK(s.lens == "RF85mm F1.2 L USM");
  CHECK(s.exposure == "1/250 s");
  CHECK(s.aperture == "f/2.8");
  CHECK(s.iso == "ISO 400");
  CHECK(s.focal_length == "85 mm");
  CHECK(s.date_taken == "2024-05-01 14:03:22");
  CHECK(s.date_taken_key == *mv::meta::parse_date_key("2024-05-01T14:03:22"));
  CHECK(s.orientation == 6);
  CHECK(s.gps == "48.85837\xC2\xB0 N, 2.29448\xC2\xB0 E");
  CHECK(s.file_size > 0);
  CHECK_FALSE(m->is_clip);
  // From PR 10 the JPEG decoder applies EXIF orientation on the display path
  // (codec/orient.h), so AF quads are rotated by it too.
  CHECK(m->display_orientation == 6);
}

TEST_CASE("the full tree keeps every tag with its origin", "[meta]") {
  temp_file f("tree.jpg");
  f.write(jpeg_with_exif());
  auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  const auto* p = find_tag(*m, "Exif.Photo.ExposureTime");
  REQUIRE(p != nullptr);
  CHECK(p->space == mv::meta::origin::exif);
  CHECK(p->group == "Exif.Photo");
  CHECK(p->name == "ExposureTime");
  CHECK(p->value == "1/250 s");
  CHECK(p->raw == "1/250");
  CHECK(m->properties.size() >= 10);
}

TEST_CASE("EXIF SubjectArea becomes an AF quad in stored-pixel space", "[meta][af]") {
  temp_file f("af.jpg");
  f.write(jpeg_with_exif());
  auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  REQUIRE(m->af_points.size() == 1);
  const auto& p = m->af_points[0];
  CHECK(p.x == Catch::Approx(24.0 / 64).margin(1e-4));
  CHECK(p.y == Catch::Approx(18.0 / 48).margin(1e-4));
  CHECK(p.w == Catch::Approx(16.0 / 64).margin(1e-4));
  CHECK(p.h == Catch::Approx(12.0 / 48).margin(1e-4));
}

TEST_CASE("a PNG with XMP populates from XMP", "[meta]") {
  temp_file f("xmp.png");
  f.write(png_with_xmp());
  auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  CHECK(m->s.width == 16);
  CHECK(m->s.format == "PNG");
  CHECK(m->s.date_taken == "2023-12-24 18:30:00");
  const auto* p = find_tag(*m, "Xmp.dc.creator");
  REQUIRE(p != nullptr);
  CHECK(p->space == mv::meta::origin::xmp);
  CHECK(p->group == "Xmp.dc");
  CHECK(p->value.find("Test Author") != std::string::npos);
}

TEST_CASE("a file with no metadata is empty fields, not an error", "[meta]") {
  SECTION("a plain JPEG") {
    temp_file f("plain.jpg");
    auto rgb = flat_rgb(8, 8);
    f.write(fixtures::jpeg_rgb(8, 8, rgb.data()));
    auto m = mv::meta::read(f.utf8());
    REQUIRE(m);
    CHECK(m->s.camera.empty());
    CHECK(m->s.exposure.empty());
    CHECK(m->s.date_taken.empty());
    CHECK(m->s.date_taken_key == 0);
    CHECK(m->af_points.empty());
    CHECK(m->s.orientation == 1);
    CHECK(m->s.width == 8);
  }
  SECTION("a BMP") {
    temp_file f("plain.bmp");
    std::vector<std::uint8_t> rgba(4 * 4 * 4, 90);
    f.write(fixtures::bmp_rgba(4, 4, rgba.data()));
    auto m = mv::meta::read(f.utf8());
    REQUIRE(m);
    CHECK(m->s.camera.empty());
  }
  SECTION("a zero-length file") {
    temp_file f("empty.jpg");
    f.write({});
    auto m = mv::meta::read(f.utf8());
    REQUIRE(m);
    CHECK(m->properties.empty());
  }
  SECTION("bytes nobody recognises") {
    temp_file f("junk.bin");
    f.write(std::vector<std::uint8_t>(512, 0x5A));
    auto m = mv::meta::read(f.utf8());
    REQUIRE(m);
    CHECK(m->properties.empty());
    CHECK(m->s.width == 0);
  }
  SECTION("a truncated JPEG keeps what was readable") {
    temp_file f("cut.jpg");
    auto bytes = jpeg_with_exif();
    bytes.resize(bytes.size() / 2);
    f.write(bytes);
    auto m = mv::meta::read(f.utf8());
    REQUIRE(m);  // whatever it managed to read; never an error
  }
}

TEST_CASE("a missing file is the one real error", "[meta]") {
  auto m = mv::meta::read("/nonexistent/definitely/not/here.jpg");
  CHECK_FALSE(m);
  CHECK(m.error() == mv::status::io);
}

TEST_CASE("an MP4 populates the container card and per-stream inspector", "[meta][clip]") {
  temp_file f("clip.mp4");
  REQUIRE(write_mp4(f.utf8()));
  auto m = mv::meta::read(f.utf8());
  REQUIRE(m);
  CHECK(m->is_clip);
  CHECK(m->s.width == 320);
  CHECK(m->s.height == 180);
  CHECK(m->s.codec == "mpeg4");
  CHECK(m->s.date_taken == "2024-05-01 14:03:22");
  CHECK(m->s.date_taken_key == *mv::meta::parse_date_key("2024-05-01T14:03:22"));
  REQUIRE(m->streams.size() == 2);
  CHECK(m->streams[0].kind == mv::meta::stream_kind::video);
  CHECK(m->streams[1].kind == mv::meta::stream_kind::audio);
  const auto field = [](const mv::meta::stream_info& s, const char* label) -> std::string {
    for (const auto& x : s.fields) {
      if (x.label == label) return x.value;
    }
    return {};
  };
  CHECK(field(m->streams[0], "Resolution") == "320 \xC3\x97 180");
  // Ten frames average to ~33 fps over their own edges; only the shape is
  // stable, and a track this short is never labelled variable.
  const std::string fps = field(m->streams[0], "Frame rate");
  CHECK(fps.size() > 4);
  CHECK(fps.find(" fps") != std::string::npos);
  CHECK(fps.find("variable") == std::string::npos);
  CHECK(field(m->streams[1], "Sample rate") == "44100 Hz");
  CHECK(field(m->streams[1], "Channels").rfind("2", 0) == 0);
  CHECK(field(m->streams[1], "Language") == "eng");
  const auto* title = find_tag(*m, "Container.title");
  REQUIRE(title != nullptr);
  CHECK(title->value == "Fixture clip");
}

TEST_CASE("a clip's facts are also numbers (PR 15, Spotlight)", "[meta][clip]") {
  for (const char* muxer : {"mp4", "matroska"}) {
    temp_file f(std::string("numbers.") + (muxer[0] == 'm' && muxer[1] == 'a' ? "mkv" : "mp4"));
    REQUIRE(write_mp4(f.utf8(), muxer));
    auto m = mv::meta::read(f.utf8());
    REQUIRE(m);
    CHECK(m->s.duration_seconds > 0.1);
    CHECK(m->s.duration_seconds < 10.0);
    CHECK(m->s.audio_channels == 2);
    CHECK(m->s.audio_sample_rate == 44100);
    REQUIRE(m->s.codecs.size() == 2);
    CHECK(m->s.codecs[0].find("MPEG-4") != std::string::npos);
    CHECK(m->s.codecs[1].find("AAC") != std::string::npos);
  }
}

TEST_CASE("a HEIC populates", "[meta][heif]") {
  const fs::path here = fs::path(__FILE__).parent_path() / "data" / "heif" / "iphone_like.heic";
  if (!fs::exists(here)) SKIP("tests/data/heif/iphone_like.heic not present");
  auto m = mv::meta::read(here.string());
  REQUIRE(m);
  CHECK(m->s.format == "HEIC");
  CHECK(m->s.width > 0);
  CHECK(m->s.height > 0);
}

TEST_CASE("date taken alone reads the sort key", "[meta][sort]") {
  temp_file jpg("d.jpg");
  jpg.write(jpeg_with_exif());
  const auto k = mv::meta::read_date_taken(jpg.utf8());
  REQUIRE(k.has_value());
  CHECK(*k == *mv::meta::parse_date_key("2024:05:01 14:03:22"));

  temp_file mp4("d.mp4");
  REQUIRE(write_mp4(mp4.utf8()));
  CHECK(mv::meta::read_date_taken(mp4.utf8()) == mv::meta::parse_date_key("2024-05-01T14:03:22Z"));

  temp_file plain("d2.jpg");
  auto rgb = flat_rgb(8, 8);
  plain.write(fixtures::jpeg_rgb(8, 8, rgb.data()));
  CHECK_FALSE(mv::meta::read_date_taken(plain.utf8()).has_value());
  CHECK_FALSE(mv::meta::read_date_taken("/nonexistent/x.jpg").has_value());
}

TEST_CASE("date parsing", "[meta][sort]") {
  using mv::meta::parse_date_key;
  CHECK(parse_date_key("1970:01:01 00:00:00") == 0);
  CHECK(parse_date_key("1970-01-02") == 86400);
  CHECK(parse_date_key("2024-05-01T14:03:22+02:00") == parse_date_key("2024:05:01 14:03:22"));
  CHECK(parse_date_key("2000-03-01T00:00:00Z").value() - parse_date_key("2000-02-28T00:00:00Z").value() ==
        2 * 86400);  // 2000 is a leap year
  CHECK_FALSE(parse_date_key("0000:00:00 00:00:00").has_value());  // clock never set
  CHECK_FALSE(parse_date_key("").has_value());
  CHECK_FALSE(parse_date_key("not a date").has_value());
  CHECK_FALSE(parse_date_key("2024:13:01 00:00:00").has_value());
}

TEST_CASE("AF geometry: Canon AFInfo2 centre-relative areas", "[meta][af]") {
  // 6000x4000 grid; point 1 is 200x200 and 600 right / 300 up-or-down of centre.
  const std::vector<std::int64_t> w{100, 200, 100}, h{100, 200, 100}, x{0, 600, -600},
      y{0, 300, 0}, focus{0b010}, selected{0b100};
  const auto pts = mv::meta::canon_af_points(6000, 4000, w, h, x, y, focus, selected);
  REQUIRE(pts.size() == 2);  // point 0 is neither in focus nor selected
  CHECK(pts[0].in_focus);
  CHECK(pts[0].x == Catch::Approx((3000 + 600 - 100) / 6000.0).margin(1e-5));
  CHECK(pts[0].y == Catch::Approx((2000 + 300 - 100) / 4000.0).margin(1e-5));
  CHECK(pts[0].w == Catch::Approx(200 / 6000.0).margin(1e-5));
  CHECK_FALSE(pts[1].in_focus);  // selected but not in focus
}

TEST_CASE("AF geometry: a box off the frame is clipped, one wholly off is dropped", "[meta][af]") {
  const auto edge = mv::meta::centre_af_point(0, 0, 100, 100, 1000, 1000, true);
  REQUIRE(edge.size() == 1);
  CHECK(edge[0].x == 0.0f);
  CHECK(edge[0].w == Catch::Approx(0.05f).margin(1e-5));
  CHECK(mv::meta::centre_af_point(-500, 500, 100, 100, 1000, 1000, true).empty());
  CHECK(mv::meta::centre_af_point(500, 500, 0, 0, 0, 1000, true).empty());  // no grid
}

TEST_CASE("EXIF orientation maps stored corners to displayed corners", "[meta][af]") {
  struct row {
    std::uint8_t o;
    float in_x, in_y, out_x, out_y;
  };
  const row rows[] = {
      {1, 0, 0, 0, 0}, {2, 0, 0, 1, 0}, {3, 0, 0, 1, 1}, {4, 0, 0, 0, 1},
      {5, 1, 0, 0, 1}, {6, 0, 0, 1, 0}, {6, 1, 0, 1, 1}, {7, 0, 0, 1, 1},
      {8, 0, 0, 0, 1}, {8, 1, 0, 0, 0},
  };
  for (const auto& r : rows) {
    float x = r.in_x, y = r.in_y;
    mv::meta::orient_point(r.o, x, y);
    INFO("orientation " << int(r.o));
    CHECK(x == r.out_x);
    CHECK(y == r.out_y);
  }
  CHECK(mv::meta::orientation_swaps(6));
  CHECK_FALSE(mv::meta::orientation_swaps(3));
}

TEST_CASE("the summary card lists every row for the kind, gaps included", "[meta][present]") {
  mv::meta::metadata empty;  // a file with nothing to say
  const auto photo = mv::meta::summary_rows(empty);
  const auto has = [](const std::vector<mv::meta::field>& rows, const char* label) {
    for (const auto& r : rows) {
      if (r.label == label) return true;
    }
    return false;
  };
  CHECK(has(photo, "Camera"));
  CHECK(has(photo, "Exposure"));
  CHECK_FALSE(has(photo, "Duration"));
  for (const auto& r : photo) CHECK(r.value.empty());  // shown as gaps, not hidden

  mv::meta::metadata clip;
  clip.is_clip = true;
  const auto clip_rows = mv::meta::summary_rows(clip);
  CHECK(has(clip_rows, "Duration"));
  CHECK(has(clip_rows, "Video codec"));
  CHECK_FALSE(has(clip_rows, "Exposure"));
}

TEST_CASE("overlay lines are empty when there is nothing to say", "[meta][present]") {
  mv::meta::metadata m;
  CHECK(mv::meta::overlay_camera_line(m).empty());
  CHECK(mv::meta::overlay_exposure_line(m).empty());
  CHECK(mv::meta::overlay_date_line(m).empty());
  m.s.camera = "Canon EOS R5";
  m.s.exposure = "1/250 s";
  m.s.aperture = "f/2.8";
  m.s.iso = "ISO 400";
  CHECK(mv::meta::overlay_camera_line(m) == "Canon EOS R5");  // no dangling separator
  CHECK(mv::meta::overlay_exposure_line(m) == "1/250 s   f/2.8   ISO 400");
  m.s.lens = "RF85";
  CHECK(mv::meta::overlay_camera_line(m) == "Canon EOS R5  |  RF85");
  CHECK(mv::meta::format_size(0).empty());
  CHECK(mv::meta::format_size(1536) == "2 KB");
  CHECK(mv::meta::format_dimensions(6000, 4000) == "6000 \xC3\x97 4000");
  CHECK(mv::meta::format_dimensions(0, 4000).empty());
}

TEST_CASE("AF quads follow the orientation the decoder applied", "[meta][af]") {
  mv::meta::metadata m;
  m.af_points.push_back({0.1f, 0.2f, 0.2f, 0.1f, true});  // stored grid
  SECTION("identity for JPEG/TIFF (not rotated on screen)") {
    m.display_orientation = 1;
    const auto q = mv::meta::displayed_af_points(m);
    REQUIRE(q.size() == 1);
    CHECK(q[0].x == Catch::Approx(0.1f));
    CHECK(q[0].y == Catch::Approx(0.2f));
    CHECK(q[0].in_focus);
  }
  SECTION("RAW stored rotated 90 clockwise (orientation 6)") {
    m.display_orientation = 6;
    const auto q = mv::meta::displayed_af_points(m);
    REQUIRE(q.size() == 1);
    // corners (0.1,0.2) and (0.3,0.3) -> (1-y, x): (0.8,0.1) and (0.7,0.3)
    CHECK(q[0].x == Catch::Approx(0.7f));
    CHECK(q[0].y == Catch::Approx(0.1f));
    CHECK(q[0].w == Catch::Approx(0.1f));
    CHECK(q[0].h == Catch::Approx(0.2f));
  }
}

// PR 9: the pane's three tables. One line per record, tabs between fields, and a
// value can never smuggle a separator in.
TEST_CASE("the pane tables are one record per line with flattened values", "[meta][tables]") {
  mv::meta::metadata m;
  m.s.camera = "Canon EOS R5";
  m.s.width = 6000;
  m.s.height = 4000;
  mv::meta::property p;
  p.space = mv::meta::origin::xmp;
  p.group = "Xmp.dc";
  p.label = "Description";
  p.value = "line one\nline\ttwo";  // a tab and a newline inside a value
  p.raw_tag = "Xmp.dc.description";
  m.properties.push_back(p);
  mv::meta::stream_info st;
  st.index = 1;
  st.kind = mv::meta::stream_kind::audio;
  st.codec = "aac";
  st.fields.push_back({"Channels", "2"});
  m.streams.push_back(st);
  m.chapters.push_back({61000, 90000, "Second\tact"});

  const std::string props = mv::meta::properties_table(m);
  CHECK(props == "xmp\tXmp.dc\tDescription\tline one line two\tXmp.dc.description\n");

  const std::string streams = mv::meta::streams_table(m);
  CHECK(streams == "S\t1\taudio\taac\nF\tChannels\t2\nC\t61000\tSecond act\n");

  // The summary always lists every row for the kind, so a gap shows as an empty
  // value rather than a missing line; a missing camera is not an error.
  const std::string summary = mv::meta::summary_table(m);
  CHECK(summary.find("Canon EOS R5") != std::string::npos);
  std::size_t lines = 0;
  for (char c : summary) lines += c == '\n';
  CHECK(lines == mv::meta::summary_rows(m).size());

  const mv::meta::metadata empty;
  CHECK(mv::meta::properties_table(empty).empty());
  CHECK(mv::meta::streams_table(empty).empty());
  CHECK_FALSE(mv::meta::summary_table(empty).empty());  // every row, all blank
}
