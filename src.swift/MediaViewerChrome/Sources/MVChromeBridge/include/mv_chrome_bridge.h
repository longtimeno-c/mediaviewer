// SPDX-License-Identifier: GPL-2.0-or-later
// C bridge from Swift chrome to the present lab's input_snapshot
// (src/shell/input_state.h). Implemented in src/shell/main_mac.mm, not here:
// this header only declares the boundary, same shape rule as plan/14-abi.md
// (POD/void args, no C++ types, no exceptions across the line) scoped down
// to what PR 18's command-bar scaffold needs.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Bumps input_snapshot.fit_seq and wakes the render thread. [any-thread]
void mv_chrome_fit(void);

// Bumps input_snapshot.one_to_one_seq and wakes the render thread. [any-thread]
void mv_chrome_one_to_one(void);

#ifdef __cplusplus
}
#endif
