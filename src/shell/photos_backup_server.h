// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Photos backup into a self-hosted server's shared folder (docs/design/26 "Backup to a
// server"): backup::server_folder over remote::library, for a folder key this computer is
// paired with and may change. Every call blocks; the backup's own thread only.
#pragma once

#include <memory>
#include <string>

#include "shell/photos_backup.h"

namespace mv::remote {
class library;
}

namespace mv::shell::backup {

// `folder_key`: a folder key inside a named folder ("remote:<server>/<root>/…/"). Null for
// anything else (a server's top holds no files of its own). `lib` outlives the run.
[[nodiscard]] std::shared_ptr<server_folder> make_server_folder(remote::library& lib, std::string folder_key);

// Where a server folder's manifest lives on this machine: one folder per key under `base`
// (the key itself names nothing readable, so the folder is its hash).
[[nodiscard]] std::string server_state_dir(std::string_view base, std::string_view folder_key);

}  // namespace mv::shell::backup
