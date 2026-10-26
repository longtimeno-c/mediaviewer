// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's run gate (#297): the memory a run may use, against a model's
// `run_bytes`. A port (D9): memory_mac.cpp, memory_win.cpp, memory_linux.cpp.
#pragma once

#include <cstdint>
#include <optional>

namespace mv::enhance {

// Physical memory available now without paging out (free plus reclaimable);
// nullopt when the OS does not say.
[[nodiscard]] std::optional<std::uint64_t> available_memory() noexcept;

}  // namespace mv::enhance
