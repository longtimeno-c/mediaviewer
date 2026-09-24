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
#include <string>

#include "core/result.h"
#include "shell/commands.h"
#include "shell/key_router.h"

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
  chrome_cmd_focus_changed = 20,     // arg is a focus_kind (key_router.h): which island, or text
  // Island notifications added after the command table filled the low ids
  // live above every command id, so the two ranges never meet.
  chrome_cmd_popup = 1000,           // arg != 0 while a `?` / go-to / find flyout is up
  chrome_cmd_rebind = 1001,          // packed: row | (key << 8) | (mods << 20)
  chrome_cmd_reset_keys = 1002,      // restore the default map
  // PR 8 updater. Chrome, not a command: no key, no row in `?` or Settings
  // (plan/16 — an update affordance is chrome). arg 0: the user clicked
  // "Update ready — restart"; arg 1: Update.exe is armed, close now.
  chrome_cmd_update_restart = 1003,
  // PR 9. tree_open: the user chose a folder in the tree; native pulls the path
  // with take_tree_path (the callback carries only a float). set_sort: arg is the
  // packed sort order (io/sort_order.h pack_sort).
  chrome_cmd_tree_open = 1004,
  chrome_cmd_set_sort = 1005,
  // PR 10. The export dialog was confirmed; arg is the packed choice
  // (edit_session.h pack_export). Cancel sends nothing.
  chrome_cmd_export = 1006,
};

static_assert(chrome_cmd_popup >= kCommandCount);

// Which flyout ShowPopup opens (0 closes whatever is up). The C# side mirrors it.
enum class chrome_popup : std::int32_t {
  close = 0,
  help = 1,
  palette = 2,  // unused; keep the value so go_to / find / settings stay put
  go_to = 3,
  find = 4,
  settings = 5,
  export_image = 6,  // PR 10; mode_mask carries the last packed choice to preselect
};

struct chrome_popup_args {
  std::int32_t kind;       // chrome_popup
  std::int32_t mode_mask;  // `?` lists the bindings live in these modes
};

static_assert(sizeof(chrome_popup_args) == 8, "keep in sync with ChromePopupArgs");

struct chrome_table_args {
  std::uint64_t utf8;  // describe_commands() text, valid for the call only
  std::int32_t length;
  std::int32_t reserved;
};

static_assert(sizeof(chrome_table_args) == 16, "keep in sync with ChromeTableArgs");

// Commands and island notifications share one id space (plan/16). These pin
// the values the island already sends; commands.h reserves the notifications.
static_assert(chrome_cmd_open == static_cast<int>(command_id::open));
static_assert(chrome_cmd_fit == static_cast<int>(command_id::fit));
static_assert(chrome_cmd_one_to_one == static_cast<int>(command_id::one_to_one));
static_assert(chrome_cmd_zoom_in == static_cast<int>(command_id::zoom_in));
static_assert(chrome_cmd_zoom_out == static_cast<int>(command_id::zoom_out));
static_assert(chrome_cmd_zoom_preset == static_cast<int>(command_id::zoom_preset));
static_assert(chrome_cmd_overlay == static_cast<int>(command_id::overlay));
static_assert(chrome_cmd_select_item == static_cast<int>(command_id::select_item));
static_assert(chrome_cmd_prev == static_cast<int>(command_id::prev));
static_assert(chrome_cmd_next == static_cast<int>(command_id::next));
static_assert(chrome_cmd_open_folder == static_cast<int>(command_id::open_folder));
static_assert(chrome_cmd_toggle_gallery == static_cast<int>(command_id::toggle_gallery));
static_assert(chrome_cmd_close_gallery == static_cast<int>(command_id::close_gallery));
static_assert(chrome_cmd_gallery_activate == static_cast<int>(command_id::gallery_activate));
static_assert(chrome_cmd_toggle_filmstrip == static_cast<int>(command_id::toggle_filmstrip));
// The panes' close buttons and the View menu send these keyed commands.
static_assert(static_cast<int>(command_id::folder_tree) == 78);
static_assert(static_cast<int>(command_id::metadata_pane) == 92);
static_assert(chrome_cmd_tree_open >= kCommandCount && chrome_cmd_set_sort >= kCommandCount);
static_assert(chrome_cmd_export >= kCommandCount);
static_assert(is_reserved_notification(chrome_cmd_set_settings));
static_assert(is_reserved_notification(chrome_cmd_folder_ready));
static_assert(is_reserved_notification(chrome_cmd_video_active));
static_assert(is_reserved_notification(chrome_cmd_set_rate));
static_assert(is_reserved_notification(chrome_cmd_focus_changed));

// The island's Command constants, hashed in declaration order. IslandHost.Probe
// computes the same over its own constants; a drift on either side fails
// tests/test_chrome_host.cpp instead of a menu item that runs the wrong thing.
[[nodiscard]] constexpr std::int32_t chrome_command_checksum() noexcept {
  constexpr int ids[] = {
      chrome_cmd_open, chrome_cmd_fit, chrome_cmd_one_to_one, chrome_cmd_zoom_in,
      chrome_cmd_zoom_out, chrome_cmd_zoom_preset, chrome_cmd_overlay, chrome_cmd_select_item,
      chrome_cmd_prev, chrome_cmd_next, chrome_cmd_open_folder, chrome_cmd_toggle_gallery,
      chrome_cmd_close_gallery, chrome_cmd_gallery_activate, chrome_cmd_set_settings,
      chrome_cmd_folder_ready, chrome_cmd_toggle_filmstrip, chrome_cmd_video_active,
      chrome_cmd_set_rate, chrome_cmd_focus_changed, chrome_cmd_popup, chrome_cmd_rebind,
      chrome_cmd_reset_keys, chrome_cmd_update_restart, chrome_cmd_tree_open,
      chrome_cmd_set_sort, chrome_cmd_export};
  std::uint32_t h = 17;
  for (const int id : ids) h = h * 31u + static_cast<std::uint32_t>(id);
  return static_cast<std::int32_t>(h);
}

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

// PR 9 panes. Native owns the geometry: x/y/width/height are client pixels, and a
// hidden pane is parked below the client area (the gallery's rule).
struct chrome_panel_args {
  std::int32_t visible;
  std::int32_t x;
  std::int32_t y;
  std::int32_t width;
  std::int32_t height;
  std::int32_t focus;  // non-zero: move keyboard focus into the pane (I, Ctrl+Shift+E)
};

static_assert(sizeof(chrome_panel_args) == 24, "keep in sync with ChromePanelArgs");

// The metadata pane's three tables (meta/tables.h), valid for the call only.
struct chrome_meta_args {
  std::uint64_t summary;
  std::uint64_t properties;
  std::uint64_t streams;
  std::int32_t summary_len;
  std::int32_t properties_len;
  std::int32_t streams_len;
  std::int32_t loading;
};

static_assert(sizeof(chrome_meta_args) == 40, "keep in sync with ChromeMetaArgs");

struct chrome_flags_args {
  std::int32_t flags;
  std::int32_t sort;  // PR 9: packed sort order, so Settings and View > Sort by show the truth
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

  // The island's command-id checksum (chrome_command_checksum), or 0.
  [[nodiscard]] std::int32_t probe_commands() const noexcept;

  // Caches each island's bridge HWND. Call after the islands attach; it is one
  // managed hop per island, never per key.
  void refresh_island_windows() noexcept;

  // Which island holds `focus`. The canvas only when `focus` is the canvas
  // window itself: a flyout's popup, a null or a foreign HWND is an island.
  [[nodiscard]] focus_kind classify_focus(HWND focus, HWND canvas) const noexcept;

  // True when the cursor is over a visible island's bridge window. Cheap
  // (GetCursorPos + GetWindowRect); asked by a timer, never per mouse-move.
  [[nodiscard]] bool cursor_over_island() const noexcept;

  // `parent` is the top-level canvas HWND. The island is MoveAndResize'd into
  // the 48 DIP strip so flyouts are siblings of the swapchain, not clipped by
  // a short child window.
  [[nodiscard]] expected attach(HWND parent, void* context, chrome_command_fn on_command,
                                int width, int height, std::uint32_t dpi) noexcept;

  void resize(int width, int height, std::uint32_t dpi) noexcept;

  // Fullscreen hides the command bar: move its bridge below the client area,
  // the same "offscreen" the other strips park in. resize() brings it back.
  void park_bar(int client_height) noexcept;

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

  // PR 9: the metadata pane (right) and folder tree (left), two islands that
  // float over the canvas and never inset it. Optional: an older chrome without
  // them still loads, and every call below is then a no-op.
  [[nodiscard]] expected attach_panels(HWND parent, void* context, chrome_command_fn on_command,
                                       void* session, int width, int height,
                                       std::uint32_t dpi) noexcept;
  [[nodiscard]] bool panels_attached() const noexcept { return panels_attached_; }
  void show_meta_pane(bool visible, int x, int y, int width, int height,
                      bool focus = false) noexcept;
  void show_folder_tree(bool visible, int x, int y, int width, int height,
                        bool focus = false) noexcept;
  [[nodiscard]] bool meta_pane_visible() const noexcept { return panels_attached_ && meta_visible_; }
  [[nodiscard]] bool folder_tree_visible() const noexcept { return panels_attached_ && tree_visible_; }
  // The record's tables, formatted once by the host; the pane re-renders from them.
  void set_meta_data(bool loading, const std::string& summary, const std::string& properties,
                     const std::string& streams) noexcept;
  void set_tree_root(const std::string& utf8_dir) noexcept;
  // The folder the user chose in the tree ("" if none pending).
  [[nodiscard]] std::string take_tree_path() noexcept;

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
  void apply_settings(std::int32_t flags, std::int32_t sort = 0) noexcept;

  // Push the current playback rate into the command bar's speed dropdown.
  void apply_rate(float rate) noexcept;

  // The command table for `?` (describe_commands). Once at attach.
  void set_command_table(const std::string& utf8) noexcept;

  // Opens a flyout on the command bar, or closes any (chrome_popup::close).
  void show_popup(chrome_popup kind, std::int32_t mode_mask) noexcept;
  // PR 10 export dialog (a flyout like `?`), preselecting `last_choice`
  // (pack_export). Confirming posts chrome_cmd_export.
  void show_export_dialog(std::int32_t last_choice) noexcept {
    show_popup(chrome_popup::export_image, last_choice);
  }
  void navigate_gallery(std::int32_t direction, std::int32_t index) noexcept;
  void scale_gallery(std::int32_t direction, std::int32_t index) noexcept;

  // True when the island consumed the message (do not Translate/Dispatch).
  [[nodiscard]] bool pre_translate(MSG* msg) noexcept;

  // Tab / Shift+Tab into the island. True if the island took focus.
  [[nodiscard]] bool navigate_focus(bool reverse) noexcept;

  // PR 8 updater. Hands the restart arguments (NUL-separated UTF-16) to the
  // managed updater, which arms Update.exe off the UI thread and then posts
  // chrome_cmd_update_restart(1). False if the entry is missing.
  bool request_update_restart(const std::wstring& args_blob) noexcept;

  // After the message loop (window gone): a staged update is applied once this
  // process exits. No restart. No-op in a dev build.
  void updater_exit() noexcept;

  void detach() noexcept;

  // Process exit only, after detach(): disposes the XAML runtime for this
  // thread (WindowsXamlManager, then the dispatcher queue). The chrome cannot
  // attach again in this process afterwards.
  void shutdown_for_exit() noexcept;

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
  chrome_entry_fn set_command_table_ = nullptr;
  chrome_entry_fn show_popup_ = nullptr;
  chrome_entry_fn attach_panels_ = nullptr;
  chrome_entry_fn detach_panels_ = nullptr;
  chrome_entry_fn show_meta_pane_ = nullptr;
  chrome_entry_fn show_folder_tree_ = nullptr;
  chrome_entry_fn set_meta_data_ = nullptr;
  chrome_entry_fn set_tree_root_ = nullptr;
  chrome_entry_fn take_tree_path_ = nullptr;
  chrome_entry_fn navigate_gallery_ = nullptr;
  chrome_entry_fn scale_gallery_ = nullptr;
  chrome_entry_fn update_restart_ = nullptr;
  chrome_entry_fn updater_exit_ = nullptr;
  bool transport_attached_ = false;
  bool transport_visible_ = false;
  bool filmstrip_attached_ = false;
  bool filmstrip_visible_ = false;
  bool gallery_attached_ = false;
  bool gallery_visible_ = false;
  bool panels_attached_ = false;
  bool meta_visible_ = false;
  bool tree_visible_ = false;
  chrome_entry_fn island_window_ = nullptr;
  chrome_entry_fn begin_detach_ = nullptr;  // unhooks static XAML events first
  chrome_entry_fn shutdown_for_exit_ = nullptr;
  // Indexed by focus_kind: [command_bar .. transport]. Refreshed after attach.
  HWND island_hwnds_[static_cast<int>(focus_kind::transport) + 1]{};
  HWND pane_hwnds_[2]{};  // metadata pane, folder tree (PR 9)
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
