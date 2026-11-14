// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire serve` and its local housekeeping (docs/design/28 "The NAS
// service"): the only part of the command line that runs without a terminal.
#pragma once

#include "addons/driftwire/cli/args.h"

namespace mv::driftwire::cli {

// Runs a.cmd == command::serve. Exit codes as the rest of the program: 0, 1
// when it could not run, 2 for a usage error. `test_hooks`: --test-loopback
// and a config with port 0 are honoured (the test build). `version`: this
// program's, for `serve self-test` and the package's daily update check.
[[nodiscard]] int run_serve(const args& a, bool test_hooks, const char* version);

}  // namespace mv::driftwire::cli
