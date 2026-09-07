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
  chrome_cmd_open = 1,         // media picker: photos and clips
  chrome_cmd_fit = 2,
  chrome_cmd_one_to_one = 3,
  chrome_cmd_zoom_in = 4,
  chrome_cmd_zoom_out = 5,
  chrome_cmd_zoom_preset = 6,  // arg is the zoom factor (0.5, 1, 2, 4)
  chrome_cmd_overlay = 7,
  chrome_cmd_select_item = 8,  // arg is the folder index
  chrome_cmd_prev = 9,
  chrome_cmd_next = 10,
  chrome_cmd_open_folder = 11,     // folder picker
  chrome_cmd_toggle_gallery = 12,
  chrome_cmd_close_gallery = 13,
  chrome_cmd_gallery_activate = 14,  // arg is the folder index: select and leave the gallery
  chrome_cmd_set_settings = 15,      // arg is a view_settings flag word
  chrome_cmd_folder_ready = 16,      // arg is the item count the island just listed
  chrome_cmd_toggle_filmstrip = 17,
  chrome_cmd_video_active = 18,      // arg != 0 while a clip is open: show the transport
  chrome_cmd_set_rate = 19,          // arg is the playback rate the dropdown picked
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
  std::int32_t y;  // client-pixel top of the island (0 for the command bar)
};

static_assert(sizeof(chrome_resize_args) == 16, "keep in sync with ChromeResizeArgs");

struct chrome_filmstrip_args {
  std::uint64_t parent_hwnd;
  std::uint64_t context;
  std::uint64_t on_command;
  std::uint64_t session;  // borrowed mv_session_t
  std::int32_t  client_width;
  std::int32_t  client_height;
  std::int32_t  dpi;
  std::int32_t  reserved;
};

static_assert(sizeof(chrome_filmstrip_args) == 48, "keep in sync with ChromeFilmstripArgs");

// Show/hide plus the geometry for the state being entered. Native owns the
// layout maths for both states so the island never has to guess where
// "offscreen" is: hiding moves the bridge below the client area, which is the
// one place a stray child window cannot eat a click meant for the canvas.
struct chrome_show_args {
  std::int32_t visible;
  std::int32_t width;
  std::int32_t height;
  std::int32_t y;
};

static_assert(sizeof(chrome_show_args) == 16, "keep in sync with ChromeShowArgs");

struct chrome_flags_args {
  std::int32_t flags;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_flags_args) == 8, "keep in sync with ChromeFlagsArgs");

// Native owns the playback rate: the keyboard is the only router (plan/16), so
// the dropdown is a view of the rate rather than a second place it is decided.
// Same one-direction rule as the settings flags — the menu changes only after
// native has applied the change.
struct chrome_rate_args {
  float        rate;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_rate_args) == 8, "keep in sync with ChromeRateArgs");

struct chrome_navigate_args {
  std::int32_t reverse;  // non-zero = Shift+Tab
  std::int32_t reserved;
};

using chrome_entry_fn = int (*)(void* arg, std::int32_t arg_size_in_bytes);

// DIP height of the command-bar strip. Physical pixels = this * dpi / 96.
inline constexpr int kChromeBarDip = 48;
inline constexpr int kFilmstripDip = 112;
// The transport strip. It sits BELOW the canvas and above the filmstrip, and
// the canvas rectangle shrinks by exactly this much while it is up — plan/16
// ("do not grow an island over the canvas") and the user's own line: the
// transport must never cover the video.
inline constexpr int kTransportDip = 52;

[[nodiscard]] inline int chrome_bar_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kChromeBarDip * static_cast<int>(dpi) + 48) / 96);
}

[[nodiscard]] inline int chrome_filmstrip_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kFilmstripDip * static_cast<int>(dpi) + 48) / 96);
}

[[nodiscard]] inline int chrome_transport_height_px(std::uint32_t dpi) noexcept {
  if (dpi == 0) dpi = 96;
  return static_cast<int>((kTransportDip * static_cast<int>(dpi) + 48) / 96);
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

  [[nodiscard]] expected attach_filmstrip(HWND parent, void* context, chrome_command_fn on_command,
                                          void* session, int width, int height,
                                          std::uint32_t dpi) noexcept;
  void resize_filmstrip(int width, int client_height, std::uint32_t dpi) noexcept;
  [[nodiscard]] bool filmstrip_attached() const noexcept { return filmstrip_attached_; }

  // The strip stays attached when hidden: it owns the completion drain that
  // feeds both it and the gallery, so tearing it down to hide 112 DIP would
  // also stop the folder listening ([12](12-decision-log.md) 2026-09-07).
  void show_filmstrip(bool visible, int width, int client_height, std::uint32_t dpi) noexcept;
  [[nodiscard]] bool filmstrip_visible() const noexcept {
    return filmstrip_attached_ && filmstrip_visible_;
  }

  // Full-client thumbnail grid, below the command bar. Same island model as
  // the filmstrip — chrome over the swapchain, never a XAML canvas (rule 2).
  [[nodiscard]] expected attach_gallery(HWND parent, void* context, chrome_command_fn on_command,
                                        void* session, int width, int height,
                                        std::uint32_t dpi) noexcept;
  void resize_gallery(int width, int client_height, std::uint32_t dpi) noexcept;
  void show_gallery(bool visible, int width, int client_height, std::uint32_t dpi) noexcept;
  [[nodiscard]] bool gallery_attached() const noexcept { return gallery_attached_; }
  [[nodiscard]] bool gallery_visible() const noexcept {
    return gallery_attached_ && gallery_visible_;
  }

  // The playback transport: a bottom strip, its content centred, shown only
  // while a clip is open. `filmstrip_px` is how much bottom chrome is already
  // spoken for, so the two strips stack instead of overlapping.
  [[nodiscard]] expected attach_transport(HWND parent, void* context, chrome_command_fn on_command,
                                          void* session, int width, int height,
                                          std::uint32_t dpi) noexcept;
  void resize_transport(int width, int client_height, int filmstrip_px, std::uint32_t dpi) noexcept;
  void show_transport(bool visible, int width, int client_height, int filmstrip_px,
                      std::uint32_t dpi) noexcept;
  [[nodiscard]] bool transport_attached() const noexcept { return transport_attached_; }
  [[nodiscard]] bool transport_visible() const noexcept {
    return transport_attached_ && transport_visible_;
  }

  // Push the persisted toggles into the settings menu so the menu and the
  // keyboard cannot disagree about what is on.
  void apply_settings(std::int32_t flags) noexcept;

  // Push the current playback rate into the command bar's speed dropdown.
  void apply_rate(float rate) noexcept;

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
  chrome_entry_fn attach_filmstrip_ = nullptr;
  chrome_entry_fn resize_filmstrip_ = nullptr;
  chrome_entry_fn detach_filmstrip_ = nullptr;
  chrome_entry_fn show_filmstrip_ = nullptr;
  chrome_entry_fn attach_gallery_ = nullptr;
  chrome_entry_fn resize_gallery_ = nullptr;
  chrome_entry_fn show_gallery_ = nullptr;
  chrome_entry_fn detach_gallery_ = nullptr;
  chrome_entry_fn attach_transport_ = nullptr;
  chrome_entry_fn resize_transport_ = nullptr;
  chrome_entry_fn show_transport_ = nullptr;
  chrome_entry_fn detach_transport_ = nullptr;
  chrome_entry_fn apply_settings_ = nullptr;
  chrome_entry_fn apply_rate_ = nullptr;
  bool transport_attached_ = false;
  bool transport_visible_ = false;
  bool filmstrip_attached_ = false;
  bool filmstrip_visible_ = false;
  bool gallery_attached_ = false;
  bool gallery_visible_ = false;
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
