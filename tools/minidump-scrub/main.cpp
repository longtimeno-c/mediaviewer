// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_minidump_scrub — run the app's privacy scrub (src/shell/minidump_scrub)
// on one dump, outside the app. For the PR 7 verify and the PR 15 upload step.
//
//   mv_minidump_scrub <in.dmp> <out.dmp> [--identity NAME]... [--no-local-identities]
//
// By default the current username and computer name are scrubbed too, exactly
// as the app's startup pass does. Exit 0 on success, 2 on usage/I/O, 3 when the
// input is not a minidump.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "shell/minidump_scrub.h"

namespace {

std::string utf8(const wchar_t* w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fputs("usage: mv_minidump_scrub <in.dmp> <out.dmp> [--identity NAME]... "
               "[--no-local-identities]\n", stderr);
    return 2;
  }
  mv::shell::scrub_options options;
  bool local = true;
  for (int i = 3; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--identity" && i + 1 < argc) options.identities.push_back(utf8(argv[++i]));
    else if (a == L"--no-local-identities") local = false;
    else { std::fputs("unrecognised argument\n", stderr); return 2; }
  }
  if (local) {
    wchar_t name[256]{};
    DWORD n = 256;
    if (::GetUserNameW(name, &n)) options.identities.push_back(utf8(name));
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1]{};
    n = MAX_COMPUTERNAME_LENGTH + 1;
    if (::GetComputerNameW(host, &n)) options.identities.push_back(utf8(host));
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { std::fputs("cannot read input\n", stderr); return 2; }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  const auto r = mv::shell::scrub_minidump(bytes, options);
  if (!r.valid) { std::fputs("not a minidump\n", stderr); return 3; }
  std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) { std::fputs("cannot write output\n", stderr); return 2; }
  std::printf("scrubbed: memory_ranges=%u zeroed_bytes=%llu paths=%u names=%u identities=%u%s\n",
              r.memory_ranges, static_cast<unsigned long long>(r.zeroed_bytes), r.paths_masked,
              r.names_masked, r.identities_masked, r.already_scrubbed ? " (was already marked)" : "");
  return 0;
}
