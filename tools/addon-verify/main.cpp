// SPDX-License-Identifier: GPL-2.0-or-later
// mv_addon_verify <folder> <public-key-hex> <platform>
//
// The app's own add-on verification (src/addon/manifest.cpp), run over an
// extracted package: manifest.json + manifest.json.sig + the files, nothing
// else. tools/package/test_addon_pack.py drives it so the Python signer is
// proved against the C++ verifier, not against a second Python copy of it.
// Prints the rejection name ("ok" when trusted); exit 0 only when trusted.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "addon/manifest.h"
#include "io/file_port.h"

namespace {
std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: mv_addon_verify <folder> <public-key-hex> <platform>\n");
    return 2;
  }
  const std::string dir = argv[1];
  const std::string hex = argv[2];
  std::vector<std::uint8_t> key;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    key.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  const auto manifest = slurp(mv::io::join_path(dir, "manifest.json"));
  const auto sig = slurp(mv::io::join_path(dir, "manifest.json.sig"));
  const auto d = mv::addon::check_manifest(manifest, sig, key, 1, argv[3]);
  if (!d.trusted()) {
    std::printf("%s\n", mv::addon::rejection_name(d.why));
    return 1;
  }
  const auto files = mv::addon::verify_files(dir, d.m);
  std::printf("%s\n", mv::addon::rejection_name(files));
  return files == mv::addon::rejection::none ? 0 : 1;
}
