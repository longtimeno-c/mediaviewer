// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Linux: the LAN share is not offered yet (Samba comes with the Linux host), so
// nothing is ever elevated (elevate.h).
#include "addons/driftwire/cli/elevate.h"

namespace mv::driftwire::cli {

elevated_run run_elevated(const std::string&, std::string_view, bool) {
  elevated_run r;
  r.why = "a network share is not offered on this computer yet (Samba comes with the Linux version)";
  return r;
}

bool helper_read(const std::string&, std::string&) { return false; }
bool helper_write(const std::string&, const std::string&) { return false; }

}  // namespace mv::driftwire::cli
