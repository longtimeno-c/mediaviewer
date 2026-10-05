// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Final Cut Pro search on / off (docs/design/23), for main_mac.mm. The bridge calls
// Swift uses (mv_fcp_*) are in mv_chrome_bridge.h; both are fcp_mac.mm.
#pragma once

// Once at launch, never on the launch path: a background block, seconds after
// launch, keeps the pieces in the state Settings last chose -- hides the
// extension from Final Cut Pro the first time this app runs, and re-registers
// the agent while it is on (an update may have changed its launchd job).
void MvFcpStart();
