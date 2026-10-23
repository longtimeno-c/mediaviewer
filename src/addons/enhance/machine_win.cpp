// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The local check on Windows: memory (GlobalMemoryStatusEx) and the NVIDIA
// adapter with the most dedicated memory, counted only when its driver
// installed CUDA's library in System32, as the AI pack does
// (src/addons/ai/platform_win.cpp).
#include <windows.h>
#include <dxgi.h>

#include <cwchar>
#include <string>

#include "addons/enhance/machine.h"

namespace mv::enhance {
namespace {

std::string utf8_of(const wchar_t* s, std::size_t cap) {
  const int n = static_cast<int>(wcsnlen(s, cap));
  if (n == 0) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, n, out.data(), bytes, nullptr, nullptr);
  return out;
}

}  // namespace

platform_facts platform_probe() {
  platform_facts f;
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    f.ram_total = mem.ullTotalPhys;
    f.ram_free = mem.ullAvailPhys;
  }
  // The NVIDIA adapter (PCI vendor 0x10DE) with the most dedicated memory;
  // never the software rasteriser.
  DXGI_ADAPTER_DESC1 best{};
  bool found = false;
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(adapter->GetDesc1(&d)) && d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          (!found || d.DedicatedVideoMemory > best.DedicatedVideoMemory)) {
        best = d;
        found = true;
      }
      adapter->Release();
    }
    factory->Release();
  }
  // An accelerator only with its CUDA driver, loaded from System32 alone.
  if (found) {
    if (HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      FreeLibrary(cuda);
      f.accelerator = "nvidia";
      f.gpu = utf8_of(best.Description, sizeof(best.Description) / sizeof(best.Description[0]));
      f.vram = best.DedicatedVideoMemory;
    }
  }
  return f;
}

void enter_background() noexcept {
  // Background mode lowers the thread's CPU, I/O and memory priority at once.
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
}

}  // namespace mv::enhance
