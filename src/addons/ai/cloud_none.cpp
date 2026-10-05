// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// No cloud provider to ask (a portable build): cloud-only files stay unindexed.
#include "addons/ai/cloud_files.h"

namespace mv::ai {

std::unique_ptr<cloud_files> make_cloud_files() { return nullptr; }

}  // namespace mv::ai
