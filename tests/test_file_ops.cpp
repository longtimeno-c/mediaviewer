// SPDX-License-Identifier: GPL-2.0-or-later
// io/file_ops against a scratch folder. Nothing here sends a real file to the
// Recycle Bin: that would change the user's machine. The refusal rule is
// tested on the shell's flag word instead.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <shobjidl.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "io/file_ops.h"

namespace fs = std::filesystem;
using mv::io::transfer_file;
using mv::io::transfer_kind;

namespace {

std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

struct scratch {
  fs::path root;
  scratch() {
    root = fs::temp_directory_path() /
           ("mv_file_ops_" + std::to_string(::GetCurrentProcessId()) + "_" +
            std::to_string(::GetTickCount64()));
    fs::create_directories(root / "src");
    fs::create_directories(root / "dest");
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path write(const char* name, const std::string& bytes) {
    const fs::path p = root / "src" / name;
    std::ofstream(p, std::ios::binary) << bytes;
    return p;
  }
};

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("a delete may go ahead only when it goes to the Recycle Bin", "[io][files]") {
  REQUIRE(mv::io::detail::pre_delete_allowed(TSF_DELETE_RECYCLE_IF_POSSIBLE));
  REQUIRE(mv::io::detail::pre_delete_allowed(TSF_DELETE_RECYCLE_IF_POSSIBLE | TSF_COPY_LOCALIZED_NAME));
  REQUIRE_FALSE(mv::io::detail::pre_delete_allowed(TSF_NORMAL));
  REQUIRE_FALSE(mv::io::detail::pre_delete_allowed(TSF_COPY_LOCALIZED_NAME | TSF_FAIL_EXIST));
}

TEST_CASE("copy never overwrites and never touches the original", "[io][files]") {
  scratch s;
  const fs::path src = s.write("IMG_0001.JPG", "original");
  const std::ofstream taken(s.root / "dest" / "IMG_0001.JPG", std::ios::binary);

  const auto first = transfer_file(utf8(src), utf8(s.root / "dest"), transfer_kind::copy);
  REQUIRE(first);
  REQUIRE(fs::path(first.value()).filename() == "IMG_0001 (2).JPG");
  const auto second = transfer_file(utf8(src), utf8(s.root / "dest"), transfer_kind::copy);
  REQUIRE(second);
  REQUIRE(fs::path(second.value()).filename() == "IMG_0001 (3).JPG");
  REQUIRE(read(s.root / "dest" / "IMG_0001 (3).JPG") == "original");
  REQUIRE(read(src) == "original");

  // Copying into its own folder lands beside it, not on it.
  const auto beside = transfer_file(utf8(src), utf8(s.root / "src"), transfer_kind::copy);
  REQUIRE(beside);
  REQUIRE(fs::path(beside.value()).filename() == "IMG_0001 (2).JPG");
  REQUIRE(read(src) == "original");
}

TEST_CASE("move renames, avoids collisions, and reports bad input", "[io][files]") {
  scratch s;
  const fs::path a = s.write("a.png", "A");
  const fs::path b = s.write("b.png", "B");
  { std::ofstream(s.root / "dest" / "b.png", std::ios::binary) << "existing"; }

  const auto moved = transfer_file(utf8(a), utf8(s.root / "dest"), transfer_kind::move);
  REQUIRE(moved);
  REQUIRE_FALSE(fs::exists(a));
  REQUIRE(read(s.root / "dest" / "a.png") == "A");

  const auto collided = transfer_file(utf8(b), utf8(s.root / "dest"), transfer_kind::move);
  REQUIRE(collided);
  REQUIRE(fs::path(collided.value()).filename() == "b (2).png");
  REQUIRE(read(s.root / "dest" / "b.png") == "existing");

  REQUIRE_FALSE(transfer_file(utf8(s.root / "src" / "missing.png"), utf8(s.root / "dest"),
                              transfer_kind::copy));
  const fs::path c = s.write("c.png", "C");
  REQUIRE_FALSE(transfer_file(utf8(c), utf8(c), transfer_kind::copy));  // dest is a file
  REQUIRE_FALSE(transfer_file("", utf8(s.root / "dest"), transfer_kind::move));
  REQUIRE(fs::exists(c));
}
