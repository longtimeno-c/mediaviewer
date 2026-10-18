// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv-timeline: a Final Cut Pro project's timeline from the command line
// (docs/design/29), both platforms.
//
//   mv-timeline describe FILE [--project NAME]
//       the model: one line per item and marker
//   mv-timeline roundtrip FILE... [--write DIR]
//       per file: parse and write back (must be byte-identical), then a new
//       version (must describe the same timeline, and keep every item's
//       bytes); --write puts each new version in DIR for an import test
//   mv-timeline version FILE --out OUT [--name NAME] [--project NAME]
//       writes a new version of the project ("<name> (Claude 1)" by default)
//
// Exit 0 when everything held, 1 when a round trip did not, 2 on a usage or
// read error.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nle/timeline.h"

namespace {

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(f);
}

std::string base_name(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? path : path.substr(cut + 1);
}

// The description without its first line (the project's name differs in a version).
std::string items_of(const std::string& description) {
  const std::size_t nl = description.find('\n');
  return nl == std::string::npos ? std::string() : description.substr(nl + 1);
}

int usage() {
  std::fprintf(stderr,
               "mv-timeline describe FILE [--project NAME]\n"
               "mv-timeline roundtrip FILE... [--write DIR]\n"
               "mv-timeline version FILE --out OUT [--name NAME] [--project NAME]\n");
  return 2;
}

// One file's round trip; prints a line and returns whether it held.
bool roundtrip(const std::string& path, const std::string& write_dir) {
  std::string text;
  if (!read_file(path, text)) {
    std::printf("FAIL %s: cannot read\n", path.c_str());
    return false;
  }
  mv::nle::timeline t;
  std::string error;
  if (!t.load(text, &error)) {
    std::printf("FAIL %s: %s\n", base_name(path).c_str(), error.c_str());
    return false;
  }
  if (t.doc().write() != text) {
    std::printf("FAIL %s: written back unchanged is not byte-identical\n", base_name(path).c_str());
    return false;
  }
  const std::string name = mv::nle::next_version_name(t.project().name, t.project_names());
  const std::string version = t.write_version(name);
  mv::nle::timeline v;
  if (!v.load(version, &error)) {
    std::printf("FAIL %s: the new version does not read back: %s\n", base_name(path).c_str(), error.c_str());
    return false;
  }
  if (v.project().name != name || !v.project().uid.empty()) {
    std::printf("FAIL %s: the new version is not renamed or keeps the uid\n", base_name(path).c_str());
    return false;
  }
  if (items_of(v.describe()) != items_of(t.describe())) {
    std::printf("FAIL %s: the new version's timeline differs\n", base_name(path).c_str());
    return false;
  }
  // Every item (effects, colour, audio components, metadata) is the same bytes.
  for (std::size_t i = 0; i < t.items().size(); ++i) {
    if (t.doc().write(t.items()[i].node) != v.doc().write(v.items()[i].node)) {
      std::printf("FAIL %s: item %zu changed bytes\n", base_name(path).c_str(), i);
      return false;
    }
  }
  if (!write_dir.empty()) {
    const std::string out = write_dir + "/" + base_name(path);
    if (!write_file(out, version)) {
      std::printf("FAIL %s: cannot write %s\n", base_name(path).c_str(), out.c_str());
      return false;
    }
  }
  std::printf("ok   %s: %zu items, %s, new version \"%s\"\n", base_name(path).c_str(), t.items().size(),
              mv::nle::format_time(t.length()).c_str(), name.c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string verb = argv[1];
  std::vector<std::string> files;
  std::string project, out, name, write_dir;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--project") {
      project = next();
    } else if (a == "--out") {
      out = next();
    } else if (a == "--name") {
      name = next();
    } else if (a == "--write") {
      write_dir = next();
    } else if (!a.empty() && a[0] == '-') {
      return usage();
    } else {
      files.push_back(a);
    }
  }
  if (files.empty()) return usage();
  if (verb == "roundtrip") {
    bool ok = true;
    for (const std::string& f : files) ok = roundtrip(f, write_dir) && ok;
    return ok ? 0 : 1;
  }
  std::string text;
  if (!read_file(files[0], text)) {
    std::fprintf(stderr, "cannot read %s\n", files[0].c_str());
    return 2;
  }
  mv::nle::timeline t;
  std::string error;
  if (!t.load(std::move(text), &error, project)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  if (verb == "describe") {
    std::fputs(t.describe().c_str(), stdout);
    return 0;
  }
  if (verb == "version") {
    if (out.empty()) return usage();
    if (name.empty()) name = mv::nle::next_version_name(t.project().name, t.project_names());
    if (!write_file(out, t.write_version(name))) {
      std::fprintf(stderr, "cannot write %s\n", out.c_str());
      return 2;
    }
    return 0;
  }
  return usage();
}
