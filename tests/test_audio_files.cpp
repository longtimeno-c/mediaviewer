// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Audio files (docs/plans/audio-and-documents.md §2.2): recognised by magic
// bytes, routed the clip's way by extension, and given a picture — the cover
// art or the card — by the poster path. No device, so every platform runs it.
#include "catch_compat.h"

#include <cstdint>
#include <string>
#include <vector>

#include "clip_fixture.h"
#include "import_fixture.h"
#include "io/pairing.h"
#include "codec/card.h"
#include "edit/clip_sample.h"
#include "meta/search.h"
#include "player/container_probe.h"
#include "player/poster.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;
using mv::player::container;

namespace {

std::vector<std::uint8_t> padded(std::vector<std::uint8_t> head) {
  head.resize(1024, 0);
  return head;
}

// A 128 kb/s, 44.1 kHz MPEG-1 Layer III header and its frame size (417 bytes).
std::vector<std::uint8_t> mp3_frames(int count) {
  std::vector<std::uint8_t> out;
  for (int i = 0; i < count; ++i) {
    std::vector<std::uint8_t> frame(417, 0x55);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x64;
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

std::vector<std::uint8_t> iso(const char* major) {
  std::vector<std::uint8_t> b = {0, 0, 0, 0x20, 'f', 't', 'y', 'p'};
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(major[i]));
  b.insert(b.end(), {0, 0, 0, 0, 'i', 's', 'o', 'm', 'm', 'p', '4', '2'});
  return padded(b);
}

// #355: an iTunes M4A with a song's tags and, when `cover`, a 64 x 48 red JPEG
// as its attached picture (the "covr" atom), from FFmpeg's own encoders.
bool make_tagged_audio(const std::string& path, bool cover) {
  AVFormatContext* out = nullptr;
  if (avformat_alloc_output_context2(&out, nullptr, "ipod", path.c_str()) < 0) return false;
  av_dict_set(&out->metadata, "title", "Hey Jude", 0);
  av_dict_set(&out->metadata, "artist", "The Beatles", 0);
  av_dict_set(&out->metadata, "album", "Past Masters", 0);
  av_dict_set(&out->metadata, "genre", "Rock", 0);
  const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!ac) return false;
  AVCodecContext* aenc = avcodec_alloc_context3(ac);
  aenc->sample_rate = 48000;
  aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
  aenc->bit_rate = 128000;
  av_channel_layout_default(&aenc->ch_layout, 2);
  aenc->time_base = AVRational{1, 48000};
  if (out->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (avcodec_open2(aenc, ac, nullptr) < 0) return false;
  AVStream* ast = avformat_new_stream(out, nullptr);
  avcodec_parameters_from_context(ast->codecpar, aenc);
  ast->time_base = aenc->time_base;
  AVPacket* art = av_packet_alloc();
  AVStream* pst = nullptr;
  if (cover) {
    const AVCodec* jc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jc) return false;
    AVCodecContext* jenc = avcodec_alloc_context3(jc);
    jenc->width = 64;
    jenc->height = 48;
    jenc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    jenc->time_base = AVRational{1, 25};
    if (avcodec_open2(jenc, jc, nullptr) < 0) return false;
    AVFrame* f = av_frame_alloc();
    f->format = jenc->pix_fmt;
    f->width = 64;
    f->height = 48;
    av_frame_get_buffer(f, 0);
    // Pure red in full-range BT.601: Y 76, Cb 85, Cr 255.
    for (int y = 0; y < 48; ++y) std::memset(f->data[0] + y * f->linesize[0], 76, 64);
    for (int y = 0; y < 24; ++y) {
      std::memset(f->data[1] + y * f->linesize[1], 85, 32);
      std::memset(f->data[2] + y * f->linesize[2], 255, 32);
    }
    f->pts = 0;
    const bool ok = avcodec_send_frame(jenc, f) >= 0 && avcodec_send_frame(jenc, nullptr) >= 0 &&
                    avcodec_receive_packet(jenc, art) >= 0;
    av_frame_free(&f);
    pst = avformat_new_stream(out, nullptr);
    avcodec_parameters_from_context(pst->codecpar, jenc);
    pst->disposition |= AV_DISPOSITION_ATTACHED_PIC;
    avcodec_free_context(&jenc);
    if (!ok) return false;
  }
  if (avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;
  if (avformat_write_header(out, nullptr) < 0) return false;
  bool ok = true;
  if (pst) {
    art->stream_index = pst->index;
    art->flags |= AV_PKT_FLAG_KEY;
    ok = av_write_frame(out, art) >= 0;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* af = av_frame_alloc();
  af->format = AV_SAMPLE_FMT_FLTP;
  af->nb_samples = aenc->frame_size;
  af->sample_rate = 48000;
  av_channel_layout_copy(&af->ch_layout, &aenc->ch_layout);
  av_frame_get_buffer(af, 0);
  for (std::int64_t at = 0; ok && at < 48000; at += af->nb_samples) {
    av_frame_make_writable(af);
    for (int c = 0; c < 2; ++c) std::memset(af->data[c], 0, sizeof(float) * static_cast<std::size_t>(af->nb_samples));
    af->pts = at;
    ok = avcodec_send_frame(aenc, af) >= 0 && fx::detail::drain(aenc, out, ast, pkt);
  }
  ok = ok && avcodec_send_frame(aenc, nullptr) >= 0 && fx::detail::drain(aenc, out, ast, pkt);
  ok = ok && av_write_trailer(out) >= 0;
  av_frame_free(&af);
  av_packet_free(&pkt);
  av_packet_free(&art);
  avcodec_free_context(&aenc);
  avio_closep(&out->pb);
  avformat_free_context(out);
  return ok;
}

std::size_t glyph_pixels(const mv::codec::card& card) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < card.rgba.size(); i += 4) n += card.rgba[i] > 120 ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("MP3 is recognised by its ID3 tag or a confirmed frame header", "[audio][probe]") {
  using mv::player::probe;
  CHECK(probe(padded({'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0})) == container::mp3);
  CHECK(probe(padded({'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0})) == container::mp3);
  CHECK(probe(padded(mp3_frames(2))) == container::mp3);
  // One plausible header followed by garbage is not enough.
  auto lone = mp3_frames(1);
  lone.resize(1024, 0x00);
  CHECK(probe(lone) == container::unknown);
  // JPEG and a bad bitrate index are not MP3.
  CHECK(probe(padded({0xFF, 0xD8, 0xFF, 0xE0})) == container::unknown);
  CHECK(probe(padded({0xFF, 0xFB, 0xF0, 0x64})) == container::unknown);
  CHECK(mv::player::is_audio(container::mp3));
  CHECK(mv::player::is_video(container::mp3));  // routed the clip's way
}

TEST_CASE("iTunes audio brands probe as m4a, not as a video", "[audio][probe]") {
  using mv::player::probe;
  CHECK(probe(iso("M4A ")) == container::m4a);
  CHECK(probe(iso("M4P ")) == container::m4a);
  CHECK(probe(iso("M4B ")) == container::m4a);
  CHECK(probe(iso("isom")) == container::mp4);
  CHECK(mv::player::is_audio(container::m4a));
  CHECK_FALSE(mv::player::is_audio(container::mp4));
}

TEST_CASE("Audio extensions route through the player", "[audio][pairing]") {
  using mv::io::is_audio_name;
  using mv::io::is_video_name;
  for (const char* name : {"song.mp3", "SONG.MP3", "a.m4a", "old.M4P"}) {
    CAPTURE(name);
    CHECK(is_audio_name(name));
    CHECK(is_video_name(name));
  }
  CHECK_FALSE(is_audio_name("clip.mp4"));
  CHECK_FALSE(is_audio_name("song.mp3.jpg"));
}

TEST_CASE("Settings can leave audio and documents out of a listing", "[audio][pairing][settings]") {
  using mv::io::is_hidden_kind;
  using mv::io::kHideAudio;
  using mv::io::kHideDocuments;
  CHECK(mv::io::is_document_name("Report.PDF"));
  CHECK(mv::io::is_document_name("letter.docx"));
  CHECK_FALSE(mv::io::is_document_name("photo.jpg"));
  CHECK_FALSE(is_hidden_kind("song.mp3", 0));
  CHECK(is_hidden_kind("song.mp3", kHideAudio));
  CHECK_FALSE(is_hidden_kind("song.mp3", kHideDocuments));
  CHECK(is_hidden_kind("a.pdf", kHideDocuments));
  CHECK_FALSE(is_hidden_kind("a.pdf", kHideAudio));
  for (const char* name : {"photo.jpg", "clip.mp4", "raw.nef"}) {
    CAPTURE(name);
    CHECK_FALSE(is_hidden_kind(name, kHideAudio | kHideDocuments));
  }
}

TEST_CASE("The audio card is opaque, even-sized and draws its glyph", "[audio][card]") {
  using mv::codec::card_kind;
  const auto music = mv::codec::make_card(card_kind::music, 641, 361);
  CHECK(music.width == 640);
  CHECK(music.height == 360);
  REQUIRE(music.rgba.size() == 640u * 360u * 4u);
  for (std::size_t i = 3; i < music.rgba.size(); i += 4) REQUIRE(music.rgba[i] == 255);
  // The corner is background; the glyph covers some, not most, of the card.
  CHECK(music.rgba[0] < 60);
  const auto lit = glyph_pixels(music);
  CHECK(lit > 640u * 360u / 200u);
  CHECK(lit < 640u * 360u / 4u);
  const auto lock = mv::codec::make_card(card_kind::locked, 640, 360);
  CHECK(glyph_pixels(lock) > 640u * 360u / 200u);
  CHECK(lock.rgba != music.rgba);
}

TEST_CASE("An audio file's poster is the card when it has no art", "[audio][poster]") {
  scratch_dir dir("audio_poster");
  const auto plain = utf8(dir / "tone.m4a");
  const auto locked = utf8(dir / "tone.m4p");
  REQUIRE(fx::make_audio(plain, 1.0));
  REQUIRE(fx::make_audio(locked, 1.0, true));

  auto a = mv::player::poster_frame(plain.c_str(), 512);
  REQUIRE(a);
  CHECK(a.value().width == 512);
  CHECK(a.value().height == 288);
  auto b = mv::player::poster_frame(locked.c_str(), 512);
  REQUIRE(b);
  CHECK(b.value().rgba != a.value().rgba);  // the padlock, not the note
}

TEST_CASE("Local search reads an audio file's cover art and song tags (#355)", "[audio][search]") {
  scratch_dir dir("audio_search");
  const auto art = utf8(dir / "with_art.m4a");
  const auto bare = utf8(dir / "bare.m4a");
  REQUIRE(make_tagged_audio(art, true));
  REQUIRE(make_tagged_audio(bare, false));

  // The cover, fitted, never enlarged, and red; none is not_found (never the card).
  auto img = mv::edit::clip::cover_art_rgb(art, 32);
  REQUIRE(img);
  CHECK(img.value().width == 32);
  CHECK(img.value().height == 24);
  REQUIRE(img.value().rgb.size() == 32u * 24u * 3u);
  const std::uint8_t* mid = img.value().rgb.data() + (12u * 32u + 16u) * 3u;
  CHECK(mid[0] > 200);
  CHECK(mid[1] < 60);
  CHECK(mid[2] < 60);
  auto full = mv::edit::clip::cover_art_rgb(art, 512);
  REQUIRE(full);
  CHECK(full.value().width == 64);
  auto none = mv::edit::clip::cover_art_rgb(bare, 512);
  REQUIRE_FALSE(none);
  CHECK(none.error() == mv::status::not_found);

  // Title, artist, album and genre are search fields.
  auto facts = mv::meta::read_search_facts(bare);
  REQUIRE(facts);
  std::string joined;
  for (const std::string& f : facts.value().fields) joined += f + "|";
  for (const char* want : {"Hey Jude", "The Beatles", "Past Masters", "Rock"}) {
    INFO(want);
    CHECK(joined.find(want) != std::string::npos);
  }
}
