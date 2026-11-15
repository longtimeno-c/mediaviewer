// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Local search as the phone catalogue's provider, for the host table's
// catalogue_* rows (docs/plans/phone-access-addon.md §2). The AI pack is not a
// slot add-on, so another add-on (Phone access) reaches its phone_publish,
// phone_cancel and phone_json through the host: both shells set the provider
// when the pack attaches as the writer (addon_abi.cpp, shell/addons_mac.mm)
// and clear it before the pack goes. Clearing waits out a call in flight, so
// the pack may unload once it returns. Portable (D9).
#pragma once

#include <functional>
#include <string>

#include "core/result.h"

struct mv_ai_api;

namespace mv::addon::catalogue {

struct provider {
  std::function<expected(const std::string& destination)> publish;
  std::function<void()> cancel;
  std::function<std::string()> json;  // "" when the pack could not say
  [[nodiscard]] explicit operator bool() const { return publish && cancel && json; }
};

// Any thread. An empty provider clears it (and waits out a call in flight).
void set_provider(provider p);
// The pack's phone_* entries, or an empty provider for a pack too old to have them.
[[nodiscard]] provider from_ai(const mv_ai_api* api);

// [worker-thread] status::not_found while no provider is set.
[[nodiscard]] expected publish(const std::string& destination);
[[nodiscard]] expected cancel();
[[nodiscard]] result<std::string> json();

}  // namespace mv::addon::catalogue
