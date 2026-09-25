// SPDX-License-Identifier: GPL-2.0-or-later
// Scratch folders and files for the Import / verified-copy / add-on tests.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace mv::test {

namespace fs = std::filesystem;

inline std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

// A path as the body of a JSON string: Windows separators are backslashes.
inline std::string json_path(const fs::path& p) {
  std::string out;
  for (const char c : utf8(p)) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

class scratch_dir {
 public:
  explicit scratch_dir(const char* tag) {
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = fs::temp_directory_path() /
            ("mv_" + std::string(tag) + "_" + std::to_string(stamp) + "_" + std::to_string(rd()));
    fs::create_directories(root_);
  }
  ~scratch_dir() {
    std::error_code ec;
    fs::permissions(root_, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(root_, ec);
  }
  scratch_dir(const scratch_dir&) = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  [[nodiscard]] const fs::path& root() const noexcept { return root_; }
  [[nodiscard]] fs::path operator/(const std::string& rel) const { return root_ / fs::path(rel); }

 private:
  fs::path root_;
};

inline void write_bytes(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline void write_text(const fs::path& p, const std::string& text) {
  write_bytes(p, std::vector<std::uint8_t>(text.begin(), text.end()));
}

inline std::vector<std::uint8_t> read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Deterministic pseudo-random content of `size` bytes; `seed` makes two files differ.
inline std::vector<std::uint8_t> pattern(std::size_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> v(size);
  std::uint32_t x = seed * 2654435761u + 12345u;
  for (auto& b : v) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b = static_cast<std::uint8_t>(x);
  }
  return v;
}

inline void set_mtime(const fs::path& p, std::int64_t unix_seconds) {
  const auto sys = std::chrono::system_clock::time_point(std::chrono::seconds(unix_seconds));
  const auto ft = std::chrono::clock_cast<std::chrono::file_clock>(sys);
  fs::last_write_time(p, ft);
}

// Files under `dir` (relative, '/'-separated), sorted.
inline std::vector<std::string> list_tree(const fs::path& dir) {
  std::vector<std::string> out;
  if (!fs::exists(dir)) return out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    std::string rel = utf8(fs::relative(e.path(), dir));
    for (char& c : rel) {
      if (c == '\\') c = '/';
    }
    out.push_back(rel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace mv::test
