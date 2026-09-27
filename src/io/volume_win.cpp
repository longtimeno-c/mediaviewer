// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Windows half of the volume port (io/volume.h).
//
// Arrival: WM_DEVICECHANGE (DBT_DEVTYP_VOLUME) on a hidden top-level window
// owned by the watcher's own thread. Volume broadcasts are not delivered to
// message-only windows, so the window is a real, never-shown one.
// Eject: lock + dismount + IOCTL_STORAGE_EJECT_MEDIA for a card in a reader
// (what Explorer's Eject does for removable media), falling back to
// CM_Request_Device_Eject on the disk's parent device for a hot-plug drive.
#include "io/volume.h"

#include <windows.h>
// initguid.h before winioctl.h, so GUID_DEVINTERFACE_DISK is defined here and
// not merely declared.
#include <initguid.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <setupapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace mv::io {
namespace {

std::wstring wide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), n);
  return out;
}

std::string narrow(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

struct handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit handle(HANDLE v) : h(v) {}
  handle(const handle&) = delete;
  handle& operator=(const handle&) = delete;
  ~handle() {
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }
  explicit operator bool() const noexcept { return h != INVALID_HANDLE_VALUE; }
};

// "\\.\E:" for the root "E:\". Empty for a UNC or mounted-folder root.
std::wstring device_path_for(const std::wstring& root) {
  if (root.size() < 2 || root[1] != L':') return {};
  return std::wstring(L"\\\\.\\") + root.substr(0, 2);
}

bool device_number(const std::wstring& dev, STORAGE_DEVICE_NUMBER& out) {
  handle h(::CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr));
  if (!h) return false;
  DWORD got = 0;
  return ::DeviceIoControl(h.h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &out, sizeof out,
                           &got, nullptr) != 0;
}

bool bus_is_removable(const std::wstring& dev) {
  handle h(::CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr));
  if (!h) return false;
  STORAGE_PROPERTY_QUERY q{};
  q.PropertyId = StorageDeviceProperty;
  q.QueryType = PropertyStandardQuery;
  alignas(8) BYTE buf[1024]{};
  DWORD got = 0;
  if (!::DeviceIoControl(h.h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got,
                         nullptr)) {
    return false;
  }
  const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf);
  return d->RemovableMedia || d->BusType == BusTypeUsb || d->BusType == BusTypeSd ||
         d->BusType == BusTypeMmc || d->BusType == BusType1394;
}

// The device instance of the disk with `number`, through the disk interface list.
DEVINST disk_devinst(DWORD number) {
  HDEVINFO set = ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return 0;
  DEVINST found = 0;
  SP_DEVICE_INTERFACE_DATA iface{};
  iface.cbSize = sizeof iface;
  for (DWORD i = 0; !found && ::SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK,
                                                            i, &iface);
       ++i) {
    DWORD need = 0;
    ::SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
    if (need == 0) continue;
    std::vector<BYTE> storage(need);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof info;
    if (!::SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, &info)) continue;
    STORAGE_DEVICE_NUMBER n{};
    if (device_number(detail->DevicePath, n) && n.DeviceNumber == number) found = info.DevInst;
  }
  ::SetupDiDestroyDeviceInfoList(set);
  return found;
}

}  // namespace

result<volume_info> volume_of(std::string_view utf8_path) {
  const std::wstring path = wide(utf8_path);
  if (path.empty()) return err(status::invalid_arg);
  wchar_t root[MAX_PATH + 1]{};
  if (!::GetVolumePathNameW(path.c_str(), root, MAX_PATH)) return err(status::io);
  const std::wstring root_w(root);

  volume_info out;
  out.root_utf8 = narrow(root_w);
  const UINT type = ::GetDriveTypeW(root);
  out.network = type == DRIVE_REMOTE;
  out.removable = type == DRIVE_REMOVABLE;

  wchar_t label[MAX_PATH + 1]{};
  DWORD serial = 0;
  DWORD flags = 0;
  if (::GetVolumeInformationW(root, label, MAX_PATH, &serial, nullptr, &flags, nullptr, 0)) {
    out.label_utf8 = narrow(label);
    out.read_only = (flags & FILE_READ_ONLY_VOLUME) != 0;
  }
  ULARGE_INTEGER avail{}, total{}, free_total{};
  if (::GetDiskFreeSpaceExW(root, &avail, &total, &free_total)) {
    out.total_bytes = total.QuadPart;
    out.free_bytes = avail.QuadPart;
  }
  // The serial number is written at format time and survives replugging;
  // the capacity separates two cards that happen to share one.
  char id[64];
  std::snprintf(id, sizeof id, "vsn:%08lX:%llu", static_cast<unsigned long>(serial),
                static_cast<unsigned long long>(out.total_bytes));
  out.volume_id = id;

  const std::wstring dev = device_path_for(root_w);
  STORAGE_DEVICE_NUMBER n{};
  if (!out.network && !dev.empty() && device_number(dev, n)) {
    char key[48];
    std::snprintf(key, sizeof key, "disk:%lu", static_cast<unsigned long>(n.DeviceNumber));
    out.device_key = key;
    if (!out.removable) out.removable = bus_is_removable(dev);
  } else {
    out.device_key = (out.network ? "net:" : "vol:") + out.root_utf8;
  }
  return out;
}

result<std::vector<volume_info>> list_volumes() {
  wchar_t buf[512]{};
  const DWORD n = ::GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buf) - 1), buf);
  if (n == 0 || n >= std::size(buf)) return err(status::io);
  std::vector<volume_info> out;
  for (const wchar_t* p = buf; *p; p += wcslen(p) + 1) {
    const UINT type = ::GetDriveTypeW(p);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
    if (auto v = volume_of(narrow(p))) out.push_back(std::move(*v));
  }
  std::stable_sort(out.begin(), out.end(), [](const volume_info& a, const volume_info& b) {
    return a.removable && !b.removable;
  });
  return out;
}

expected eject_volume(std::string_view root_utf8) {
  const std::wstring root = wide(root_utf8);
  const std::wstring dev = device_path_for(root);
  if (dev.empty()) return err(status::invalid_arg);
  STORAGE_DEVICE_NUMBER number{};
  const bool have_number = device_number(dev, number);

  {
    handle h(::CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (h) {
      DWORD got = 0;
      bool locked = false;
      for (int i = 0; i < 10 && !locked; ++i) {
        locked = ::DeviceIoControl(h.h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        if (!locked) ::Sleep(100);  // the eject worker, never the UI thread
      }
      if (!locked) return err(status::io);  // something still has a file open
      if (::DeviceIoControl(h.h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) {
        PREVENT_MEDIA_REMOVAL allow{};
        allow.PreventMediaRemoval = FALSE;
        ::DeviceIoControl(h.h, IOCTL_STORAGE_MEDIA_REMOVAL, &allow, sizeof allow, nullptr, 0, &got,
                          nullptr);
        if (::DeviceIoControl(h.h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &got,
                              nullptr)) {
          return {};
        }
      }
    }
  }

  // A drive whose medium is not removable (a USB stick or SSD): ask PnP to
  // remove the device, parent first, the way "Safely Remove" does.
  if (!have_number) return err(status::io);
  const DEVINST disk = disk_devinst(number.DeviceNumber);
  if (!disk) return err(status::io);
  DEVINST parent = 0;
  for (DEVINST target : {DEVINST{0}, disk}) {
    if (target == 0) {
      if (::CM_Get_Parent(&parent, disk, 0) != CR_SUCCESS) continue;
      target = parent;
    }
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t veto_name[MAX_PATH]{};
    if (::CM_Request_Device_EjectW(target, &veto, veto_name, MAX_PATH, 0) == CR_SUCCESS &&
        veto == PNP_VetoTypeUnknown) {
      return {};
    }
  }
  return err(status::io);
}

// ---------------------------------------------------------------------------
struct volume_watcher::impl {
  std::thread thread;
  DWORD thread_id = 0;
  HANDLE ready = nullptr;
  callback cb = nullptr;
  void* user = nullptr;

  static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DEVICECHANGE && (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) {
      const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
      auto* self = reinterpret_cast<impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self && hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        const auto* vol = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp);
        for (int i = 0; i < 26; ++i) {
          if (!(vol->dbcv_unitmask & (1u << i))) continue;
          const char root[4] = {static_cast<char>('A' + i), ':', '\\', 0};
          self->cb(self->user,
                   wp == DBT_DEVICEARRIVAL ? volume_event::arrived : volume_event::removed, root);
        }
      }
      return TRUE;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }

  void run() {
    thread_id = ::GetCurrentThreadId();
    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &impl::wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MediaViewerVolumeWatcher";
    ::RegisterClassW(&wc);  // fails harmlessly if a previous watcher registered it
    HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0,
                                  nullptr, nullptr, inst, nullptr);
    if (hwnd) ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ::SetEvent(ready);
    if (!hwnd) return;
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    ::DestroyWindow(hwnd);
  }
};

volume_watcher::volume_watcher() = default;
volume_watcher::~volume_watcher() { stop(); }

expected volume_watcher::start(callback cb, void* user) {
  if (!cb) return err(status::invalid_arg);
  std::lock_guard lock(mutex_);
  if (impl_) return err(status::invalid_arg);
  auto p = std::make_unique<impl>();
  p->cb = cb;
  p->user = user;
  p->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!p->ready) return err(status::io);
  impl* raw = p.get();
  p->thread = std::thread([raw] { raw->run(); });
  ::WaitForSingleObject(p->ready, INFINITE);
  impl_ = std::move(p);
  return {};
}

void volume_watcher::stop() noexcept {
  std::lock_guard lock(mutex_);
  if (!impl_) return;
  if (impl_->thread_id) ::PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
  if (impl_->thread.joinable()) impl_->thread.join();
  ::CloseHandle(impl_->ready);
  impl_.reset();
}

}  // namespace mv::io
