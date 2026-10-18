// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <dxgi.h>
#include <intrin.h>

#include <cstring>
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

machine_facts machine() noexcept {
  machine_facts m;
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) m.ram_bytes = mem.ullTotalPhys;
  int regs[4] = {};
  char brand[49] = {};
  __cpuid(regs, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
    for (int i = 0; i < 3; ++i) {
      __cpuid(regs, static_cast<int>(0x80000002 + i));
      memcpy(brand + i * 16, regs, 16);
    }
    m.chip = brand;
    while (!m.chip.empty() && m.chip.front() == ' ') m.chip.erase(m.chip.begin());
  }
  // An NVIDIA adapter (PCI vendor 0x10DE) whose driver installed CUDA's.
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(adapter->GetDesc1(&d)) && d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        m.nvidia = true;
      }
      adapter->Release();
    }
    factory->Release();
  }
  if (m.nvidia) {
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    m.nvidia = cuda != nullptr;
    if (cuda) FreeLibrary(cuda);
  }
  SYSTEM_POWER_STATUS s{};
  m.laptop = GetSystemPowerStatus(&s) && s.BatteryFlag != 128 && s.BatteryFlag != 255;
  return m;
}

}  // namespace mv::ai::platform
