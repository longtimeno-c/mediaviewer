// SPDX-License-Identifier: GPL-2.0-or-later
// Loads the C# WinUI chrome and attaches a DesktopWindowXamlSource island.
//
// plan/10 PR 3, D1 amendment: the Win32 window and D3D11 swapchain stay the
// app; chrome is hosted inside them. C++ never talks to WinUI types — it loads
// MediaViewer.Chrome.dll through hostfxr and calls a handful of entry points.
// Canvas mouse-move does not cross this boundary (plan/02).
#pragma once

#include <windows.h>

#include <cstdint>

#include "core/result.h"

namespace mv::shell {

// Commands the island posts back. The C# side uses the same integers.
enum chrome_command : int {
  chrome_cmd_open = 1,
  chrome_cmd_fit = 2,
  chrome_cmd_one_to_one = 3,
  chrome_cmd_zoom_in = 4,
  chrome_cmd_zoom_out = 5,
  chrome_cmd_zoom_preset = 6,  // arg is the zoom factor (0.5, 1, 2, 4)
  chrome_cmd_overlay = 7,
};

using chrome_command_fn = void (*)(void* context, int command, float arg);

// Blittable attach payload. Layout is asserted against the C# struct; a silent
// drift here is a function-pointer read as a HWND.
struct chrome_attach_args {
  std::uint64_t parent_hwnd;
  std::uint64_t context;
  std::uint64_t on_command;
  std::int32_t  client_width;
  std::int32_t  client_height;
  std::int32_t  dpi;
  std::int32_t  reserved;
};

static_assert(sizeof(chrome_attach_args) == 40, "keep in sync with ChromeAttachArgs");

struct chrome_resize_args {
  std::int32_t width;
  std::int32_t height;
  std::int32_t dpi;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_resize_args) == 16, "keep in sync with ChromeResizeArgs");

struct chrome_navigate_args {
  std::int32_t reverse;  // non-zero = Shift+Tab
  std::int32_t reserved;
};

using chrome_entry_fn = int (*)(void* arg, std::int32_t arg_size_in_bytes);

// DIP height of the command-bar strip. Physical pixels = this * dpi / 96.
inline constexpr int kChromeBarDip = 48;

[[nodiscard]] inline int chrome_bar_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kChromeBarDip * static_cast<int>(dpi) + 48) / 96);
}

class chrome_host {
 public:
  chrome_host() = default;
  ~chrome_host();

  chrome_host(const chrome_host&) = delete;
  chrome_host& operator=(const chrome_host&) = delete;

  // Finds MediaViewer.Chrome.dll beside this exe, starts the runtime, and
  // resolves the entry points. Safe to call when the dll is missing — returns
  // an error and leaves the host idle.
  [[nodiscard]] expected load() noexcept;

  [[nodiscard]] bool loaded() const noexcept { return probe_ != nullptr; }
  [[nodiscard]] bool attached() const noexcept { return attached_; }

  // Returns the C# sizeof(ChromeAttachArgs) so a layout drift fails a test
  // rather than a window that never appears.
  [[nodiscard]] int probe() const noexcept;

  // `parent` is the top-level canvas HWND. The island is MoveAndResize'd into
  // the 48 DIP strip so flyouts are siblings of the swapchain, not clipped by
  // a short child window.
  [[nodiscard]] expected attach(HWND parent, void* context, chrome_command_fn on_command,
                                int width, int height, std::uint32_t dpi) noexcept;

  void resize(int width, int height, std::uint32_t dpi) noexcept;

  // True when the island consumed the message (do not Translate/Dispatch).
  [[nodiscard]] bool pre_translate(MSG* msg) noexcept;

  // Tab / Shift+Tab into the island. True if the island took focus.
  [[nodiscard]] bool navigate_focus(bool reverse) noexcept;

  void detach() noexcept;

 private:
  [[nodiscard]] bool resolve_paths() noexcept;
  [[nodiscard]] bool load_hostfxr() noexcept;
  [[nodiscard]] bool load_runtime() noexcept;
  [[nodiscard]] chrome_entry_fn get_entry(const wchar_t* method) noexcept;

  HMODULE hostfxr_ = nullptr;
  void* context_ = nullptr;  // hostfxr_handle
  using load_assembly_fn = int (*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*,
                                   void*, void**);
  load_assembly_fn load_fn_ = nullptr;
  chrome_entry_fn probe_ = nullptr;
  chrome_entry_fn attach_ = nullptr;
  chrome_entry_fn resize_ = nullptr;
  chrome_entry_fn detach_ = nullptr;
  chrome_entry_fn navigate_ = nullptr;
  using pre_translate_fn = BOOL(WINAPI*)(const MSG*);
  pre_translate_fn pre_translate_ = nullptr;
  bool attached_ = false;
  wchar_t chrome_dir_[MAX_PATH]{};
  wchar_t chrome_dll_[MAX_PATH]{};
  wchar_t runtime_config_[MAX_PATH]{};
  wchar_t hostfxr_path_[MAX_PATH]{};
  wchar_t dotnet_root_[MAX_PATH]{};
};

}  // namespace mv::shell
