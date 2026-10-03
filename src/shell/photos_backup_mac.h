// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Photos library as a backup::source (plan/26 "Backup"): PhotoKit through
// shell/photos_items_mac.h. macOS only.
#pragma once

#include <memory>

#include "shell/photos_backup.h"

namespace mv::shell::backup {

[[nodiscard]] std::unique_ptr<source> make_photos_source();

}  // namespace mv::shell::backup
