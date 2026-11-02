// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #606: GoPro GPMF payloads built byte for byte from the published format
// (github.com/gopro/gpmf-parser, "GPMF Introduction": an 8-byte KLV header of
// FourCC, type, struct size and a big-endian repeat count, big-endian data
// padded to 4 bytes, DEVC > STRM nesting), and an MP4 carrying them on a
// "GoPro MET" data track (codec tag `gpmd`), written in-process like
// cue_fixture.h. No camera file is committed: the owner's real clips are the
// verify line's, not CI's.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace mv::test::gpmf {

using bytes = std::vector<std::uint8_t>;

inline void put16(bytes& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v >> 8));
  b.push_back(static_cast<std::uint8_t>(v));
}
inline void put32(bytes& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}

// One KLV: `data` is struct_size * repeat bytes; padding is added here.
inline bytes klv(const char* key, char type, std::uint8_t struct_size, std::uint16_t repeat, const bytes& data) {
  bytes b(key, key + 4);
  b.push_back(static_cast<std::uint8_t>(type));
  b.push_back(struct_size);
  put16(b, repeat);
  b.insert(b.end(), data.begin(), data.end());
  while (b.size() % 4) b.push_back(0);
  return b;
}

// A nest (type 0) as GoPro writes it: struct size 1, repeat = byte length.
inline bytes nest(const char* key, const std::vector<bytes>& children) {
  bytes body;
  for (const bytes& c : children) body.insert(body.end(), c.begin(), c.end());
  return klv(key, 0, 1, static_cast<std::uint16_t>(body.size()), body);
}

inline bytes longs(std::initializer_list<std::int32_t> v) {
  bytes b;
  for (std::int32_t x : v) put32(b, static_cast<std::uint32_t>(x));
  return b;
}

struct fix {
  double lat = 0, lon = 0, alt = 0;
};

// A HERO5-HERO10 GPS stream: STNM, SCAL (one per GPS5 element), GPSF, GPSU,
// GPSP, then GPS5 with one sample per fix.
inline bytes gps5_strm(const std::vector<fix>& fixes, std::uint32_t gpsf = 3, std::uint16_t gpsp = 120,
                       bool with_scal = true) {
  std::vector<bytes> kids;
  const std::string name = "GPS (Lat., Long., Alt., 2D speed, 3D speed)";
  kids.push_back(klv("STNM", 'c', 1, static_cast<std::uint16_t>(name.size()), bytes(name.begin(), name.end())));
  if (with_scal) kids.push_back(klv("SCAL", 'l', 4, 5, longs({10000000, 10000000, 1000, 1000, 100})));
  bytes f;
  put32(f, gpsf);
  kids.push_back(klv("GPSF", 'L', 4, 1, f));
  const std::string utc = "240501140322.000";
  kids.push_back(klv("GPSU", 'U', 16, 1, bytes(utc.begin(), utc.end())));
  bytes p;
  put16(p, gpsp);
  kids.push_back(klv("GPSP", 'S', 2, 1, p));
  bytes samples;
  for (const fix& x : fixes) {
    put32(samples, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x.lat * 1e7))));
    put32(samples, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x.lon * 1e7))));
    put32(samples, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x.alt * 1000))));
    put32(samples, 1500);  // 1.5 m/s
    put32(samples, 160);
  }
  kids.push_back(klv("GPS5", 'l', 20, static_cast<std::uint16_t>(fixes.size()), samples));
  return nest("STRM", kids);
}

// A HERO11+ GPS9 stream: TYPE "lllllllSS", nine SCALs; fix and DOP per sample.
struct fix9 {
  double lat = 0, lon = 0, alt = 0;
  double dop = 1.2;
  int fix = 3;
};
inline bytes gps9_strm(const std::vector<fix9>& fixes) {
  std::vector<bytes> kids;
  const std::string type = "lllllllSS";
  kids.push_back(klv("TYPE", 'c', 1, static_cast<std::uint16_t>(type.size()), bytes(type.begin(), type.end())));
  kids.push_back(klv("SCAL", 'l', 4, 9, longs({10000000, 10000000, 1000, 1000, 100, 1, 1000, 100, 1})));
  bytes samples;
  for (const fix9& x : fixes) {
    put32(samples, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x.lat * 1e7))));
    put32(samples, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x.lon * 1e7))));
    put32(samples, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(x.alt * 1000))));
    put32(samples, 1500);
    put32(samples, 160);
    put32(samples, 8887);      // days since 2000-01-01 (2024-05-01)
    put32(samples, 50602000);  // 14:03:22.000 in ms
    put16(samples, static_cast<std::uint16_t>(std::lround(x.dop * 100)));
    put16(samples, static_cast<std::uint16_t>(x.fix));
  }
  kids.push_back(klv("GPS9", '?', 32, static_cast<std::uint16_t>(fixes.size()), samples));
  return nest("STRM", kids);
}

// An accelerometer stream beside the GPS one, as every GoPro payload has.
inline bytes accl_strm() {
  bytes samples;
  for (int i = 0; i < 4; ++i) {
    put16(samples, 100);
    put16(samples, 200);
    put16(samples, 300);
  }
  return nest("STRM", {klv("SCAL", 's', 2, 1, bytes{0x01, 0xA0}), klv("ACCL", 's', 6, 4, samples)});
}

// One packet: DEVC > (DVID, DVNM, STRM ACCL, STRM GPS).
inline bytes payload(const bytes& gps_strm) {
  const std::string dvnm = "HERO10 Black";
  return nest("DEVC", {klv("DVID", 'L', 4, 1, longs({1})),
                       klv("DVNM", 'c', 1, static_cast<std::uint16_t>(dvnm.size()), bytes(dvnm.begin(), dvnm.end())),
                       accl_strm(), gps_strm});
}

// An MP4 holding one "GoPro MET" data track (codec tag gpmd), one packet a
// second. libavformat's mov muxer writes a gpmd sample entry for that tag.
inline bool make_mp4(const std::string& path, const std::vector<bytes>& packets) {
  AVFormatContext* out = nullptr;
  if (avformat_alloc_output_context2(&out, nullptr, "mp4", path.c_str()) < 0 || !out) return false;
  AVStream* st = avformat_new_stream(out, nullptr);
  bool ok = st != nullptr;
  if (ok) {
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    st->codecpar->codec_tag = MKTAG('g', 'p', 'm', 'd');
    st->time_base = AVRational{1, 1000};
    av_dict_set(&st->metadata, "handler_name", "GoPro MET", 0);
  }
  if (ok) ok = avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0;
  if (ok) ok = avformat_write_header(out, nullptr) >= 0;
  AVPacket* pkt = av_packet_alloc();
  for (std::size_t i = 0; ok && i < packets.size(); ++i) {
    ok = av_new_packet(pkt, static_cast<int>(packets[i].size())) >= 0;
    if (!ok) break;
    std::memcpy(pkt->data, packets[i].data(), packets[i].size());
    pkt->stream_index = st->index;
    pkt->pts = pkt->dts = static_cast<std::int64_t>(i) * 1000;
    pkt->duration = 1000;
    pkt->flags |= AV_PKT_FLAG_KEY;
    ok = av_interleaved_write_frame(out, pkt) >= 0;
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  if (ok) ok = av_write_trailer(out) >= 0;
  if (out->pb) avio_closep(&out->pb);
  avformat_free_context(out);
  return ok;
}

}  // namespace mv::test::gpmf
