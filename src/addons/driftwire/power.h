// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #530: while bytes move, neither machine goes to sleep from idleness: a
// transfer holds a request for the system to stay awake (the display may
// still turn off and the screen lock). The port behind it is per OS:
// PowerCreateRequest / PowerSetRequest(PowerRequestSystemRequired) on
// Windows, an IOKit assertion (PreventUserIdleSystemSleep) on the Mac, a
// systemd-logind "sleep" inhibitor on Linux. It cannot stop a closed lid, a
// shutdown or a flat battery: a receive that stops then is resumed
// (resume.h).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace mv::driftwire::power {

class stay_awake {
 public:
  virtual ~stay_awake() = default;  // released
};

// Held until the result is destroyed; nullptr when the OS refused (the
// transfer runs anyway). `receiving`: the reason the OS shows (powercfg
// /requests, pmset -g assertions, systemd-inhibit --list) says which way.
[[nodiscard]] std::unique_ptr<stay_awake> hold(bool receiving) noexcept;

namespace detail {
inline std::atomic<const char*> owner{"MediaViewer"};
}

// Who the reason names, for the whole process: "MediaViewer" (the add-on,
// the default) or the driftwire command line's "Driftwire". Set once at
// start, before any transfer; `name` must outlive the process (a literal).
inline void set_owner(const char* name) noexcept { detail::owner.store(name ? name : "MediaViewer"); }
[[nodiscard]] inline const char* owner() noexcept { return detail::owner.load(); }

// The reason each OS shows: "MediaViewer is receiving files (Driftwire)",
// or "Driftwire is sending files" when Driftwire is itself the owner.
[[nodiscard]] inline std::string reason(bool receiving) {
  const std::string_view who = owner();
  std::string out(who);
  out += receiving ? " is receiving files" : " is sending files";
  if (who != "Driftwire") out += " (Driftwire)";
  return out;
}

}  // namespace mv::driftwire::power
