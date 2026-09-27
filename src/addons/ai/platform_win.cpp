// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

#include <thread>

#include "addons/ai/platform.h"

namespace mv::ai::platform {

std::string self_dir() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&self_dir), &self)) {
    return {};
  }
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
  if (n == 0 || n >= path.size()) return {};
  path.resize(n);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  path.resize(slash);
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0,
                                        nullptr, nullptr);
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), out.data(), bytes, nullptr,
                      nullptr);
  return out;
}

power power_state() noexcept {
  power p;
  SYSTEM_POWER_STATUS s{};
  if (!GetSystemPowerStatus(&s)) return p;
  p.on_battery = s.ACLineStatus == 0;
  if (s.BatteryLifePercent <= 100) p.percent = s.BatteryLifePercent;
  return p;
}

void enter_background() noexcept {
  // Background mode lowers the thread's CPU, I/O and memory priority at once.
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
}

unsigned hardware_threads() noexcept { return std::thread::hardware_concurrency(); }

}  // namespace mv::ai::platform
