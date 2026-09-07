// SPDX-License-Identifier: GPL-2.0-or-later
// OWNER: mediaviewer-08 (5b). Already listed in CMakeLists.txt and already
// dispatched from present_lab.cpp's --av-soak flag, so you do not need to touch
// either to fill this in.
#include "shell/av_soak.h"

namespace mv::shell {

int run_av_soak(const av_soak_options&) {
  // Not implemented yet. Returning non-zero so an unimplemented soak can never
  // be mistaken for a passing one.
  return 2;
}

}  // namespace mv::shell
