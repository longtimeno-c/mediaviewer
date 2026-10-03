// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_addon_verify <folder> <public-key-hex> <platform>
//
// The app's own add-on verification (src/addon/manifest.cpp), run over an
// extracted package: manifest.json + manifest.json.sig + the files, nothing
// else. tools/package/test_addon_pack.py drives it so the Python signer is
// proved against the C++ verifier, not against a second Python copy of it.
// Prints the rejection name ("ok" when trusted); exit 0 only when trusted.
//
// mv_addon_verify --open <package.mvaddon> [<store-folder>]
//
// The same for an open add-on (plan/25): the app's own reading of a package
// tools/addon-sdk/mvaddon.py made. Prints what the consent sheet is fed (the
// inspect JSON); with a store folder it then installs there and prints the
// install's JSON. Exit 0 only when every step said ok.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "addon/manifest.h"
#include "addon/open_json.h"
#include "addon/open_store.h"
#include "core/json.h"
#include "io/file_port.h"

namespace {
std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

namespace {
int verify_open(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::fprintf(stderr, "usage: mv_addon_verify --open <package.mvaddon> [<store-folder>]\n");
    return 2;
  }
  // Without a folder to install into, one that does not exist: nothing is
  // installed, so the package is judged as a fresh install.
  const std::string root = argc == 4 ? argv[3] : mv::io::join_path(argv[2], "no-store");
  const mv::addon::open_store store(root);
  const std::string offer = mv::addon::open_inspect_json(store, argv[2]);
  std::printf("%s\n", offer.c_str());
  const auto doc = mv::json::parse(offer);
  if (!doc || !doc->boolean("ok").value_or(false)) return 1;
  if (argc != 4) return 0;
  const std::string* sha = doc->str("sha256");
  const std::string installed = mv::addon::open_install_json(store, argv[2], sha ? *sha : "");
  std::printf("%s\n", installed.c_str());
  const auto result = mv::json::parse(installed);
  return result && result->boolean("ok").value_or(false) ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--open") return verify_open(argc, argv);
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
