// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// No model downloader where there is no transport (fetch.h): the pack must
// carry its models (ai-models.py --upstream bundle).
#include "addons/ai/fetch.h"

namespace mv::ai {

std::unique_ptr<http_transport> make_http_transport() { return nullptr; }

}  // namespace mv::ai
