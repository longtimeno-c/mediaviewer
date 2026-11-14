// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// backup::library over the Import add-on's mv.import.1 table (Check library,
// docs/plans/check-library.md): the index is import.db, which the add-on owns.
// Portable: the table is plain C (mediaviewer_import.h). Nothing here runs on
// the UI thread; check() waits for the add-on's job on the backup's own thread.
#pragma once

#include <memory>

#include <mediaviewer/mediaviewer_import.h>

#include "shell/photos_backup.h"

namespace mv::shell::backup {

// Whether this Import has the Check library calls (an older one does not).
[[nodiscard]] bool import_has_library_check(const mv_import_api* api) noexcept;

// nullptr when `api` is null or too old. Every call the result makes into
// `api` passes one process-wide gate; making a library opens it.
[[nodiscard]] std::shared_ptr<library> make_import_library(const mv_import_api* api);

// [main] Before Import unloads: no new call enters `api` (each fails as
// cancelled), and this waits up to `seconds` for those already inside. True
// when none is. The check job a cancelled call leaves behind is cancelled too.
[[nodiscard]] bool close_import_library(double seconds);

}  // namespace mv::shell::backup
