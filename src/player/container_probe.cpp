// SPDX-License-Identifier: GPL-2.0-or-later
#include "player/container_probe.h"

#include <cstring>

namespace mv::player {
namespace {

[[nodiscard]] bool has(std::span<const std::uint8_t> b, std::size_t offset,
                       const char* literal, std::size_t length) noexcept {
  if (offset + length > b.size()) return false;
  return std::memcmp(b.data() + offset, literal, length) == 0;
}

// ISO base media files start with a box: a 4-byte big-endian size, then the
// four-character type. 'ftyp' should be first, and its major brand tells MP4
// from QuickTime.
[[nodiscard]] container probe_iso_bmff(std::span<const std::uint8_t> b) noexcept {
  if (!has(b, 4, "ftyp", 4)) return container::unknown;
  if (b.size() < 12) return container::unknown;

  // 'qt  ' is classic QuickTime. Everything else in the wild that phones and
  // cameras write — isom, mp41, mp42, avc1, iso2, M4V , mmp4 — is MP4-shaped,
  // and FFmpeg opens them all through the same demuxer, so the distinction is
  // for reporting rather than routing.
  if (has(b, 8, "qt  ", 4)) return container::quicktime;
  // ISO-BMFF also stores still images; do not route HEIF/AVIF to video.
  if (has(b, 8, "avif", 4) || has(b, 8, "avis", 4) || has(b, 8, "heic", 4) ||
      has(b, 8, "heix", 4) || has(b, 8, "mif1", 4) || has(b, 8, "msf1", 4)) return container::unknown;
  return container::mp4;
}

// Matroska and WebM share the EBML magic; only DocType separates them, and it
// sits a little way into the header rather than at a fixed offset.
[[nodiscard]] container probe_ebml(std::span<const std::uint8_t> b) noexcept {
  const std::size_t limit = b.size() < 256 ? b.size() : 256;
  for (std::size_t i = 0; i + 4 <= limit; ++i) {
    if (std::memcmp(b.data() + i, "webm", 4) == 0) return container::webm;
  }
  for (std::size_t i = 0; i + 8 <= limit; ++i) {
    if (std::memcmp(b.data() + i, "matroska", 8) == 0) return container::matroska;
  }
  // EBML with no DocType we recognise. Matroska is the safe assumption: WebM is
  // a subset, so treating one as the other costs nothing at the demuxer.
  return container::matroska;
}

// A single 0x47 byte is meaningless — it is a common value. A transport stream
// is 0x47 every 188 bytes, so confirm the cadence before claiming it. Without
// this check, roughly one file in 256 probes as MPEG-TS on its first byte.
[[nodiscard]] bool probe_mpeg_ts(std::span<const std::uint8_t> b) noexcept {
  constexpr std::size_t packet = 188;
  if (b.empty() || b[0] != 0x47) return false;
  int confirmed = 0;
  for (std::size_t offset = packet; offset < b.size(); offset += packet) {
    if (b[offset] != 0x47) return false;
    ++confirmed;
  }
  // At least two more sync bytes, i.e. three packets seen. One repeat is still
  // plausible coincidence.
  return confirmed >= 2;
}

}  // namespace

container probe(std::span<const std::uint8_t> head) noexcept {
  if (head.size() < 12) return container::unknown;

  if (const container iso = probe_iso_bmff(head); iso != container::unknown) return iso;

  // EBML: 0x1A45DFA3.
  if (head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) {
    return probe_ebml(head);
  }

  // RIFF....AVI  — the form type at offset 8 is what makes it AVI rather than
  // WAV, which shares the RIFF container entirely.
  if (has(head, 0, "RIFF", 4) && has(head, 8, "AVI ", 4)) return container::avi;

  if (probe_mpeg_ts(head)) return container::mpeg_ts;

  return container::unknown;
}

const char* container_name(container c) noexcept {
  switch (c) {
    case container::unknown:   return "unknown";
    case container::mp4:       return "mp4";
    case container::quicktime: return "quicktime";
    case container::matroska:  return "matroska";
    case container::webm:      return "webm";
    case container::avi:       return "avi";
    case container::mpeg_ts:   return "mpegts";
  }
  return "unknown";
}

}  // namespace mv::player
