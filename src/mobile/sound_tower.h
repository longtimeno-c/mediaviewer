// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's sound text tower (docs/plans/mobile.md §9, "Sound search"): which one, where it
// comes from, and where it lives on the phone. Header only, so the link (which downloads it) and
// the search (which loads it) read one pin.
//
// A catalogue already carries every clip's sound vectors, from the Mac's CLAP audio tower. What a
// phone lacks is the text half that puts a typed query in the same space. It downloads it once,
// from the project's own release, and keeps it only if every file is the size and SHA-256 pinned
// here: the app's signature is what makes the pin trusted, as the sample library's key is
// (sample.h). tools/ios/clap_text_phone.py makes the files and prints these lines.
#pragma once

#include <array>
#include <span>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace mv::mobile::sound {

// The index key the files answer: the Mac's CLAP (model.json's id and precision, "pre1"). A
// catalogue whose clips were heard by another tower gets no download.
inline constexpr std::string_view kSpec = "clap-general/fp16/pre1";

// The project's release; a Debug build may point elsewhere (MV_SOUND_BASE, in the app).
inline constexpr std::string_view kBase =
    "https://github.com/longtimeno-c/mediaviewer/releases/download/clap-text-phone-1/";

struct pinned_file {
  std::string_view name;    // the file's name in the folder, and in the release
  std::string_view sha256;  // lowercase hex
  std::uint64_t size;
};

inline constexpr std::array<pinned_file, 4> kFiles{{
    {"model.json", "31721abccdb35c8ed2db1bc7e14761a62d184c1aff45854cc9d2e5fac861a522", 567},
    {"text.onnx", "d0297571b4db9a9037f4c2ee745aeccecd354cc91328e15e67f746e50cf4984b", 152346942},
    {"vocab.json", "ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e", 798293},
    {"merges.txt", "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5", 456318},
}};

// Every function below takes the pin, kFiles unless a test gives another.
using pin = std::span<const pinned_file>;

[[nodiscard]] inline constexpr std::uint64_t total_bytes(pin files = kFiles) noexcept {
  std::uint64_t n = 0;
  for (const pinned_file& f : files) n += f.size;
  return n;
}

// Written last, after every file's hash matched: names each file's hash, so a folder from an
// older pin is not taken for this one.
inline constexpr std::string_view kVerified = "verified.txt";

[[nodiscard]] inline std::string verified_text(pin files = kFiles) {
  std::string out;
  for (const pinned_file& f : files) {
    out.append(f.name).append(" ").append(f.sha256).append("\n");
  }
  return out;
}

inline std::filesystem::path utf8_path(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// <library>/sound/clap-general: inside the library's folder, so Forget removes it.
[[nodiscard]] inline std::string folder(const std::string& library_dir) { return library_dir + "/sound/clap-general"; }

// True when `dir` holds this pin's files, verified: the record says so and every size is right.
// Sizes only, as a generation's files are at open (a hash at every launch would cost seconds).
[[nodiscard]] inline bool ready(const std::string& dir, pin files = kFiles) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::ifstream in(utf8_path(dir) / std::string(kVerified), std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  if (text.str() != verified_text(files)) return false;
  for (const pinned_file& f : files) {
    const auto size = fs::file_size(utf8_path(dir) / std::string(f.name), ec);
    if (ec || size != f.size) return false;
  }
  return true;
}

// Bytes this pin keeps on the phone, partial downloads included (for the reported cache size).
[[nodiscard]] inline std::uint64_t bytes_on_disk(const std::string& library_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::uint64_t n = 0;
  const fs::path root = utf8_path(library_dir) / "sound";
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) n += it->file_size(e2);
  }
  return n;
}

}  // namespace mv::mobile::sound
