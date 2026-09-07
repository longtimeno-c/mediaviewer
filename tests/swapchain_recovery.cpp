// SPDX-License-Identifier: GPL-2.0-or-later
// Real-GPU lifecycle regression check; run on the frame-time runner only.
#include <windows.h>
#include <cstdio>
#include "gfx/swapchain.h"

int wmain() {
  const auto instance = ::GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = ::DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"MediaViewer.SwapchainRecoveryTest";
  if (!::RegisterClassW(&wc)) return 2;
  HWND window = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"",
                                  WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
  if (!window) return 2;
  mv::gfx::device device;
  if (!device.create(window)) { ::DestroyWindow(window); return 2; }
  mv::gfx::swapchain sc;
  DWORD handles_before = 0, handles_after = 0;
  int result = 0;
  for (int i = 0; i <= 50; ++i) {
    const auto created = sc.create(device, window, {64, 64, false});
    if (!created || !sc.back_buffer_rtv()) {
      std::printf("create failed: %s\n", mv::status_name(created.error()));
      result = 1;
      break;
    }
    // D3D11 cannot allocate a UINT_MAX-wide texture. The old view must survive.
    const auto invalid = sc.resize(0xffffffffu, 64);
    if (invalid || !sc.back_buffer_rtv() || sc.width() != 64) {
      std::printf("failed resize recovery: status %s, rtv %p, width %u\n",
                  mv::status_name(invalid.error()), static_cast<void*>(sc.back_buffer_rtv()), sc.width());
      result = 1;
      break;
    }
    const auto same = sc.resize(64, 64);
    const auto next = sc.resize(128, 96);
    if (!same || !next || !sc.back_buffer_rtv()) {
      std::printf("retry failed: same %s, next %s, rtv %p\n", mv::status_name(same.error()),
                  mv::status_name(next.error()), static_cast<void*>(sc.back_buffer_rtv()));
      result = 1;
      break;
    }
    sc.destroy();
    if (i == 0 && !::GetProcessHandleCount(::GetCurrentProcess(), &handles_before)) result = 2;
  }
  sc.destroy();
  if (!::GetProcessHandleCount(::GetCurrentProcess(), &handles_after)) result = 2;
  // Allow a few lazily initialized driver handles, not one leaked per rebuild.
  if (handles_after > handles_before + 4) result = 1;
  device.destroy();
  ::DestroyWindow(window);
  std::printf("swapchain recovery: %s; handles %lu -> %lu\n",
              result == 0 ? "PASS" : "FAIL", handles_before, handles_after);
  return result;
}
