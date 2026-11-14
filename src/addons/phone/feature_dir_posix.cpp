// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// service.h feature_dir off Windows: io::phone_dir's place (io/paths_mac.cpp),
// kept so a library set up by the app before the add-on is found where it is.
#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "addons/phone/service.h"

namespace mv::phone {

result<std::string> feature_dir() {
  // The tests' own folder (tests/test_phone_addon.cpp), never the person's.
  if (const char* t = std::getenv("MV_PHONE_ACCESS_DIR"); t && t[0] == '/') return std::string(t);
  std::string home;
  if (const char* h = std::getenv("HOME"); h && h[0] == '/') {
    home = h;
  } else if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir && pw->pw_dir[0] == '/') {
    home = pw->pw_dir;
  }
  if (home.empty()) return err(status::io);
  return home + "/Library/Application Support/MediaViewer/Phone Access";
}

}  // namespace mv::phone
