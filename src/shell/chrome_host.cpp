// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/chrome_host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "core/trace.h"

namespace mv::shell {

namespace {

using char_t = wchar_t;

struct hostfxr_initialize_parameters {
  std::size_t size;
  const char_t* host_path;
  const char_t* dotnet_root;
};

using hostfxr_initialize_for_runtime_config_fn =
    int (*)(const char_t* runtime_config_path, const hostfxr_initialize_parameters* parameters,
            void** host_context_handle);
using hostfxr_get_runtime_delegate_fn = int (*)(const void* host_context_handle, int type,
                                                void** delegate);

// hostfxr.h: hdt_load_assembly_and_get_function_pointer
constexpr int kLoadAssemblyAndGetFunctionPointer = 5;

// hostfxr StatusCode: Success, Success_HostAlreadyInitialized,
// Success_DifferentRuntimeProperties.
bool hostfxr_ok(int rc) noexcept { return rc == 0 || rc == 1 || rc == 2; }

bool module_directory(wchar_t* out, std::size_t cap) noexcept {
  const DWORD n = ::GetModuleFileNameW(nullptr, out, static_cast<DWORD>(cap));
  if (n == 0 || n >= cap) return false;
  wchar_t* slash = nullptr;
  for (wchar_t* p = out; *p; ++p) {
    if (*p == L'\\' || *p == L'/') slash = p;
  }
  if (!slash) return false;
  *slash = L'\0';
  return true;
}

bool join_path(wchar_t* out, std::size_t cap, const wchar_t* dir, const wchar_t* file) noexcept {
  const int n = ::swprintf_s(out, cap, L"%s\\%s", dir, file);
  return n > 0;
}

bool file_exists(const wchar_t* path) noexcept {
  const DWORD a = ::GetFileAttributesW(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int cmp_dotnet_version(const wchar_t* a, const wchar_t* b) noexcept {
  int av[4] = {};
  int bv[4] = {};
  (void)swscanf_s(a, L"%d.%d.%d.%d", &av[0], &av[1], &av[2], &av[3]);
  (void)swscanf_s(b, L"%d.%d.%d.%d", &bv[0], &bv[1], &bv[2], &bv[3]);
  for (int i = 0; i < 4; ++i) {
    if (av[i] != bv[i]) return av[i] < bv[i] ? -1 : 1;
  }
  return 0;
}

// hostfxr_initialize_for_runtime_config cannot load a self-contained
// component. Chrome is framework-dependent; hostfxr comes from the machine's
// .NET install (beside the exe is tried first for a copied hostfxr).
bool find_hostfxr(const wchar_t* exe_dir, wchar_t* out, std::size_t cap,
                  wchar_t* dotnet_root, std::size_t root_cap) noexcept {
  wchar_t roots[2][MAX_PATH]{};
  if (::GetEnvironmentVariableW(L"DOTNET_ROOT", roots[0], MAX_PATH) == 0) roots[0][0] = L'\0';
  if (::GetEnvironmentVariableW(L"ProgramW6432", roots[1], MAX_PATH) == 0) {
    wcsncpy_s(roots[1], L"C:\\Program Files", _TRUNCATE);
  }
  wchar_t program_files_dotnet[MAX_PATH]{};
  if (!join_path(program_files_dotnet, MAX_PATH, roots[1], L"dotnet")) return false;
  wcsncpy_s(roots[1], program_files_dotnet, _TRUNCATE);

  wchar_t best_dll[MAX_PATH]{};
  wchar_t best_ver[64]{};
  wchar_t best_root[MAX_PATH]{};
  for (const auto& root : roots) {
    if (root[0] == L'\0') continue;
    wchar_t fxr[MAX_PATH]{};
    if (!join_path(fxr, MAX_PATH, root, L"host\\fxr")) continue;
    wchar_t glob[MAX_PATH]{};
    if (!join_path(glob, MAX_PATH, fxr, L"*")) continue;
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW(glob, &fd);
    if (find == INVALID_HANDLE_VALUE) continue;
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
      if (fd.cFileName[0] == L'.') continue;
      wchar_t dll[MAX_PATH]{};
      if (::swprintf_s(dll, L"%s\\%s\\hostfxr.dll", fxr, fd.cFileName) < 0) continue;
      if (!file_exists(dll)) continue;
      if (best_dll[0] == L'\0' || cmp_dotnet_version(fd.cFileName, best_ver) > 0) {
        wcsncpy_s(best_dll, dll, _TRUNCATE);
        wcsncpy_s(best_ver, fd.cFileName, _TRUNCATE);
        wcsncpy_s(best_root, root, _TRUNCATE);
      }
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
  }
  if (best_dll[0] != L'\0') {
    wcsncpy_s(out, cap, best_dll, _TRUNCATE);
    if (dotnet_root && root_cap) wcsncpy_s(dotnet_root, root_cap, best_root, _TRUNCATE);
    return true;
  }

  // Last resort: a hostfxr copied beside the exe (self-contained layout).
  if (join_path(out, cap, exe_dir, L"hostfxr.dll") && file_exists(out)) {
    if (dotnet_root && root_cap) wcsncpy_s(dotnet_root, root_cap, exe_dir, _TRUNCATE);
    return true;
  }
  return false;
}

}  // namespace

chrome_host::~chrome_host() { detach(); }

bool chrome_host::resolve_paths() noexcept {
  if (!module_directory(chrome_dir_, MAX_PATH)) return false;
  if (!join_path(chrome_dll_, MAX_PATH, chrome_dir_, L"MediaViewer.Chrome.dll")) return false;
  if (!join_path(runtime_config_, MAX_PATH, chrome_dir_, L"MediaViewer.Chrome.runtimeconfig.json")) {
    return false;
  }
  return file_exists(chrome_dll_) && file_exists(runtime_config_);
}

bool chrome_host::load_hostfxr() noexcept {
  if (!find_hostfxr(chrome_dir_, hostfxr_path_, MAX_PATH, dotnet_root_, MAX_PATH)) {
    MV_LOG_WARN("chrome: hostfxr.dll not found (need a .NET 8 runtime)");
    return false;
  }
  hostfxr_ = ::LoadLibraryW(hostfxr_path_);
  if (!hostfxr_) {
    MV_LOG_WARN("chrome: LoadLibrary hostfxr failed (%ls)", hostfxr_path_);
    return false;
  }
  return true;
}

bool chrome_host::load_runtime() noexcept {
  auto init = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
      ::GetProcAddress(hostfxr_, "hostfxr_initialize_for_runtime_config"));
  auto get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
      ::GetProcAddress(hostfxr_, "hostfxr_get_runtime_delegate"));
  if (!init || !get_delegate) {
    MV_LOG_WARN("chrome: hostfxr exports missing");
    return false;
  }

  wchar_t host_path[MAX_PATH]{};
  if (!::GetModuleFileNameW(nullptr, host_path, MAX_PATH)) return false;

  hostfxr_initialize_parameters params{};
  params.size = sizeof(params);
  params.host_path = host_path;
  params.dotnet_root = dotnet_root_[0] ? dotnet_root_ : nullptr;

  const int rc = init(runtime_config_, &params, &context_);
  if (!hostfxr_ok(rc) || !context_) {
    MV_LOG_WARN("chrome: hostfxr_initialize_for_runtime_config failed (%d)", rc);
    context_ = nullptr;
    return false;
  }

  void* load = nullptr;
  const int drc = get_delegate(context_, kLoadAssemblyAndGetFunctionPointer, &load);
  if (!hostfxr_ok(drc) || !load) {
    MV_LOG_WARN("chrome: hostfxr_get_runtime_delegate failed (%d)", drc);
    return false;
  }
  load_fn_ = reinterpret_cast<load_assembly_fn>(load);
  return load_fn_ != nullptr;
}

chrome_entry_fn chrome_host::get_entry(const wchar_t* method) noexcept {
  if (!load_fn_) return nullptr;
  void* fn = nullptr;
  const int rc = load_fn_(chrome_dll_, L"MediaViewer.Chrome.IslandHost, MediaViewer.Chrome",
                          method, nullptr, nullptr, &fn);
  if (rc != 0 || !fn) {
    MV_LOG_WARN("chrome: failed to resolve IslandHost.%ls (%d)", method, rc);
    return nullptr;
  }
  return reinterpret_cast<chrome_entry_fn>(fn);
}

expected chrome_host::load() noexcept {
  if (loaded()) return {};
  if (!resolve_paths()) {
    MV_LOG_WARN("chrome: MediaViewer.Chrome.dll not published beside the exe");
    return err(status::io);
  }
  if (!load_hostfxr()) return err(status::io);
  if (!load_runtime()) return err(status::internal);

  probe_ = get_entry(L"Probe");
  attach_ = get_entry(L"Attach");
  resize_ = get_entry(L"Resize");
  detach_ = get_entry(L"Detach");
  navigate_ = get_entry(L"NavigateFocus");
  attach_filmstrip_ = get_entry(L"AttachFilmstrip");
  resize_filmstrip_ = get_entry(L"ResizeFilmstrip");
  detach_filmstrip_ = get_entry(L"DetachFilmstrip");
  show_filmstrip_ = get_entry(L"ShowFilmstrip");
  attach_gallery_ = get_entry(L"AttachGallery");
  resize_gallery_ = get_entry(L"ResizeGallery");
  show_gallery_ = get_entry(L"ShowGallery");
  detach_gallery_ = get_entry(L"DetachGallery");
  attach_transport_ = get_entry(L"AttachTransport");
  resize_transport_ = get_entry(L"ResizeTransport");
  show_transport_ = get_entry(L"ShowTransport");
  detach_transport_ = get_entry(L"DetachTransport");
  apply_settings_ = get_entry(L"ApplySettings");
  apply_rate_ = get_entry(L"ApplyRate");
  if (!probe_ || !attach_ || !resize_ || !detach_ || !navigate_ || !attach_filmstrip_ ||
      !resize_filmstrip_ || !detach_filmstrip_ || !show_filmstrip_ || !attach_gallery_ ||
      !resize_gallery_ || !show_gallery_ || !detach_gallery_ || !attach_transport_ ||
      !resize_transport_ || !show_transport_ || !detach_transport_ || !apply_settings_ ||
      !apply_rate_) {
    return err(status::internal);
  }

  MV_LOG_INFO("chrome: runtime loaded from %ls", chrome_dll_);
  return {};
}

int chrome_host::probe() const noexcept {
  if (!probe_) return -1;
  return probe_(nullptr, 0);
}

expected chrome_host::attach(HWND parent, void* context, chrome_command_fn on_command,
                             int width, int height, std::uint32_t dpi) noexcept {
  if (!loaded()) return err(status::internal);
  if (!parent) return err(status::invalid_arg);
  if (attached_) detach();

  chrome_attach_args args{};
  args.parent_hwnd = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
  args.context = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
  args.on_command = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(on_command));
  args.client_width = width;
  args.client_height = height;
  args.dpi = static_cast<std::int32_t>(dpi);

  const int rc = attach_(&args, static_cast<std::int32_t>(sizeof(args)));
  if (rc != 0) {
    MV_LOG_WARN("chrome: Attach failed (%d)", rc);
    return err(status::internal);
  }
  attached_ = true;

  if (HMODULE xaml = ::GetModuleHandleW(L"Microsoft.ui.xaml.dll")) {
    pre_translate_ =
        reinterpret_cast<pre_translate_fn>(::GetProcAddress(xaml, "ContentPreTranslateMessage"));
  }
  MV_LOG_INFO("chrome: island attached %dx%d @ %u dpi", width, height, dpi);
  return {};
}

void chrome_host::resize(int width, int height, std::uint32_t dpi) noexcept {
  if (!attached_ || !resize_) return;
  chrome_resize_args args{};
  args.width = width;
  args.height = height;
  args.dpi = static_cast<std::int32_t>(dpi);
  args.y = 0;
  (void)resize_(&args, static_cast<std::int32_t>(sizeof(args)));
}

expected chrome_host::attach_filmstrip(HWND parent, void* context, chrome_command_fn on_command,
                                       void* session, int width, int height,
                                       std::uint32_t dpi) noexcept {
  if (!loaded() || !attach_filmstrip_) return err(status::internal);
  if (!parent) return err(status::invalid_arg);
  if (filmstrip_attached_) {
    if (detach_filmstrip_) (void)detach_filmstrip_(nullptr, 0);
    filmstrip_attached_ = false;
  }

  chrome_filmstrip_args args{};
  args.parent_hwnd = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
  args.context = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
  args.on_command = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(on_command));
  args.session = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(session));
  args.client_width = width;
  args.client_height = height;
  args.dpi = static_cast<std::int32_t>(dpi);

  const int rc = attach_filmstrip_(&args, static_cast<std::int32_t>(sizeof(args)));
  if (rc != 0) {
    MV_LOG_WARN("chrome: AttachFilmstrip failed (%d)", rc);
    return err(status::internal);
  }
  filmstrip_attached_ = true;
  filmstrip_visible_ = true;  // AttachFilmstrip builds it on screen
  return {};
}

void chrome_host::resize_filmstrip(int width, int client_height, std::uint32_t dpi) noexcept {
  if (!filmstrip_attached_ || !resize_filmstrip_) return;
  if (!filmstrip_visible_) {
    // Hidden: keep it parked below the client area so a resize does not walk
    // it back on screen.
    show_filmstrip(false, width, client_height, dpi);
    return;
  }
  const int strip = chrome_filmstrip_height_px(dpi);
  chrome_resize_args args{};
  args.width = width;
  args.height = strip;
  args.dpi = static_cast<std::int32_t>(dpi);
  args.y = client_height > strip ? client_height - strip : 0;
  (void)resize_filmstrip_(&args, static_cast<std::int32_t>(sizeof(args)));
}

void chrome_host::show_filmstrip(bool visible, int width, int client_height,
                                 std::uint32_t dpi) noexcept {
  if (!filmstrip_attached_ || !show_filmstrip_) return;
  const int strip = chrome_filmstrip_height_px(dpi);
  chrome_show_args args{};
  args.visible = visible ? 1 : 0;
  args.width = visible ? width : 1;
  args.height = visible ? strip : 1;
  args.y = visible ? (client_height > strip ? client_height - strip : 0) : client_height;
  (void)show_filmstrip_(&args, static_cast<std::int32_t>(sizeof(args)));
  filmstrip_visible_ = visible;
}

expected chrome_host::attach_gallery(HWND parent, void* context, chrome_command_fn on_command,
                                     void* session, int width, int height,
                                     std::uint32_t dpi) noexcept {
  if (!loaded() || !attach_gallery_) return err(status::internal);
  if (!parent) return err(status::invalid_arg);
  if (gallery_attached_) {
    if (detach_gallery_) (void)detach_gallery_(nullptr, 0);
    gallery_attached_ = false;
    gallery_visible_ = false;
  }

  chrome_filmstrip_args args{};
  args.parent_hwnd = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
  args.context = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
  args.on_command = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(on_command));
  args.session = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(session));
  args.client_width = width;
  args.client_height = height;
  args.dpi = static_cast<std::int32_t>(dpi);

  const int rc = attach_gallery_(&args, static_cast<std::int32_t>(sizeof(args)));
  if (rc != 0) {
    MV_LOG_WARN("chrome: AttachGallery failed (%d)", rc);
    return err(status::internal);
  }
  gallery_attached_ = true;
  gallery_visible_ = false;  // AttachGallery builds it parked offscreen
  return {};
}

void chrome_host::resize_gallery(int width, int client_height, std::uint32_t dpi) noexcept {
  if (!gallery_attached_ || !resize_gallery_) return;
  if (!gallery_visible_) {
    show_gallery(false, width, client_height, dpi);
    return;
  }
  const int bar = chrome_bar_height_px(dpi);
  chrome_resize_args args{};
  args.width = width;
  args.height = client_height > bar ? client_height - bar : 1;
  args.dpi = static_cast<std::int32_t>(dpi);
  args.y = bar;
  (void)resize_gallery_(&args, static_cast<std::int32_t>(sizeof(args)));
}

void chrome_host::show_gallery(bool visible, int width, int client_height,
                               std::uint32_t dpi) noexcept {
  if (!gallery_attached_ || !show_gallery_) return;
  const int bar = chrome_bar_height_px(dpi);
  chrome_show_args args{};
  args.visible = visible ? 1 : 0;
  args.width = visible ? width : 1;
  args.height = visible ? (client_height > bar ? client_height - bar : 1) : 1;
  args.y = visible ? bar : client_height;
  (void)show_gallery_(&args, static_cast<std::int32_t>(sizeof(args)));
  gallery_visible_ = visible;
}

// The transport stacks ON TOP of the filmstrip, not over it: `filmstrip_px` is
// the bottom chrome already spoken for, and the strip's top edge is measured up
// from there. Native owns this maths for the same reason it owns the
// filmstrip's — the island must never have to guess where "offscreen" is.
namespace {
int transport_top(int client_height, int filmstrip_px, int strip) noexcept {
  const int bottom = client_height - filmstrip_px;
  return bottom > strip ? bottom - strip : 0;
}
}  // namespace

expected chrome_host::attach_transport(HWND parent, void* context, chrome_command_fn on_command,
                                       void* session, int width, int height,
                                       std::uint32_t dpi) noexcept {
  if (!loaded() || !attach_transport_) return err(status::internal);
  if (!parent) return err(status::invalid_arg);
  if (transport_attached_) {
    if (detach_transport_) (void)detach_transport_(nullptr, 0);
    transport_attached_ = false;
    transport_visible_ = false;
  }

  chrome_filmstrip_args args{};
  args.parent_hwnd = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
  args.context = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
  args.on_command = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(on_command));
  args.session = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(session));
  args.client_width = width;
  args.client_height = height;
  args.dpi = static_cast<std::int32_t>(dpi);

  const int rc = attach_transport_(&args, static_cast<std::int32_t>(sizeof(args)));
  if (rc != 0) {
    MV_LOG_WARN("chrome: AttachTransport failed (%d)", rc);
    return err(status::internal);
  }
  transport_attached_ = true;
  transport_visible_ = false;  // AttachTransport parks it: no clip is open yet
  return {};
}

void chrome_host::resize_transport(int width, int client_height, int filmstrip_px,
                                   std::uint32_t dpi) noexcept {
  if (!transport_attached_ || !resize_transport_) return;
  if (!transport_visible_) {
    show_transport(false, width, client_height, filmstrip_px, dpi);
    return;
  }
  const int strip = chrome_transport_height_px(dpi);
  chrome_resize_args args{};
  args.width = width;
  args.height = strip;
  args.dpi = static_cast<std::int32_t>(dpi);
  args.y = transport_top(client_height, filmstrip_px, strip);
  (void)resize_transport_(&args, static_cast<std::int32_t>(sizeof(args)));
}

void chrome_host::show_transport(bool visible, int width, int client_height, int filmstrip_px,
                                 std::uint32_t dpi) noexcept {
  if (!transport_attached_ || !show_transport_) return;
  const int strip = chrome_transport_height_px(dpi);
  chrome_show_args args{};
  args.visible = visible ? 1 : 0;
  args.width = visible ? width : 1;
  args.height = visible ? strip : 1;
  args.y = visible ? transport_top(client_height, filmstrip_px, strip) : client_height;
  (void)show_transport_(&args, static_cast<std::int32_t>(sizeof(args)));
  transport_visible_ = visible;
}

void chrome_host::apply_settings(std::int32_t flags) noexcept {
  if (!attached_ || !apply_settings_) return;
  chrome_flags_args args{};
  args.flags = flags;
  (void)apply_settings_(&args, static_cast<std::int32_t>(sizeof(args)));
}

void chrome_host::apply_rate(float rate) noexcept {
  if (!attached_ || !apply_rate_) return;
  chrome_rate_args args{};
  args.rate = rate;
  (void)apply_rate_(&args, static_cast<std::int32_t>(sizeof(args)));
}

bool chrome_host::pre_translate(MSG* msg) noexcept {
  if (!attached_ || !pre_translate_ || !msg) return false;
  return pre_translate_(msg) != FALSE;
}

bool chrome_host::navigate_focus(bool reverse) noexcept {
  if (!attached_ || !navigate_) return false;
  chrome_navigate_args args{};
  args.reverse = reverse ? 1 : 0;
  return navigate_(&args, static_cast<std::int32_t>(sizeof(args))) == 0;
}

void chrome_host::detach() noexcept {
  if (transport_attached_ && detach_transport_) {
    (void)detach_transport_(nullptr, 0);
    transport_attached_ = false;
    transport_visible_ = false;
  }
  if (gallery_attached_ && detach_gallery_) {
    (void)detach_gallery_(nullptr, 0);
    gallery_attached_ = false;
    gallery_visible_ = false;
  }
  if (filmstrip_attached_ && detach_filmstrip_) {
    (void)detach_filmstrip_(nullptr, 0);
    filmstrip_attached_ = false;
    filmstrip_visible_ = false;
  }
  if (attached_ && detach_) {
    (void)detach_(nullptr, 0);
    attached_ = false;
  }
  pre_translate_ = nullptr;
}

}  // namespace mv::shell
