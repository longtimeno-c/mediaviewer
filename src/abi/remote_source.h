// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// 0.42 (docs/plans/desktop-client.md): what the Windows core's session needs of
// the remote folder (remote/library.h), set up once per process by remote_abi.cpp.
// Every call that can block runs on a pool worker, never the UI or render thread.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "remote/library.h"

namespace mv::abi::remote_source {

// The process's library, made and loaded on first use (servers.json and the
// keys: call it from a worker the first time). Null when it cannot be made.
[[nodiscard]] remote::library* library();
// Starts loading it on a detached thread: at session creation, so the folder
// tree's rows are there when it opens.
void start();

// A folder's listing as the session lists it.
[[nodiscard]] result<remote::listing> list(std::string_view folder_key);
// A file's JPEG-512. BLOCKS.
[[nodiscard]] result<std::vector<std::uint8_t>> tile(std::string_view file_key, const job_context* ctx);
// The tile's bytes when it was made already (no network).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> cached_tile(std::string_view file_key);
// The original's local path, fetched into the cache when it is not there. BLOCKS;
// `ctx`'s cancel stops it.
[[nodiscard]] result<std::string> fetch(std::string_view file_key, const job_context* ctx);

}  // namespace mv::abi::remote_source
