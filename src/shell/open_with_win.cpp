// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/open_with_win.h"

#include <windows.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <iterator>

namespace mv::shell {
namespace {

// COM for this call only, on whatever worker runs it. The association
// handlers want an apartment; S_FALSE / RPC_E_CHANGED_MODE mean the thread
// already has one, which is just as good (and not ours to uninitialise).
struct com_scope {
  HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  ~com_scope() {
    if (SUCCEEDED(hr)) ::CoUninitialize();
  }
};

bool same_path(const std::wstring& a, const std::wstring& b) {
  return !a.empty() && ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring own_exe() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
}

std::wstring default_exe(const std::wstring& extension) {
  wchar_t buf[MAX_PATH * 2] = {};
  DWORD n = static_cast<DWORD>(std::size(buf));
  if (FAILED(::AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, extension.c_str(),
                                 L"open", buf, &n))) {
    return {};
  }
  return buf;
}

// Calls `fn(handler, exe, ui_name)` for each recommended handler that is not
// MediaViewer; stops when it returns true.
template <typename Fn>
void each_handler(const std::wstring& extension, Fn&& fn) {
  IEnumAssocHandlers* handlers = nullptr;
  if (FAILED(::SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_RECOMMENDED, &handlers)) || !handlers) {
    return;
  }
  const std::wstring me = own_exe();
  IAssocHandler* h = nullptr;
  ULONG got = 0;
  bool done = false;
  while (!done && handlers->Next(1, &h, &got) == S_OK && got == 1) {
    LPWSTR exe = nullptr;
    LPWSTR ui = nullptr;
    if (SUCCEEDED(h->GetName(&exe)) && SUCCEEDED(h->GetUIName(&ui)) && exe && ui &&
        !same_path(exe, me)) {
      done = fn(h, std::wstring(exe), std::wstring(ui));
    }
    ::CoTaskMemFree(exe);
    ::CoTaskMemFree(ui);
    h->Release();
  }
  handlers->Release();
}

}  // namespace

std::vector<open_with_app> list_open_with(const std::wstring& extension) {
  std::vector<open_with_app> out;
  if (extension.empty()) return out;
  const com_scope com;
  const std::wstring def = default_exe(extension);
  each_handler(extension, [&](IAssocHandler*, const std::wstring& exe, const std::wstring& ui) {
    for (const auto& seen : out) {
      if (same_path(seen.exe, exe)) return false;
    }
    open_with_app app{ui, exe};
    if (same_path(exe, def)) {
      out.insert(out.begin(), std::move(app));
    } else {
      out.push_back(std::move(app));
    }
    return false;
  });
  return out;
}

bool open_with(const std::wstring& file, const std::wstring& exe) {
  const std::wstring extension = ::PathFindExtensionW(file.c_str());
  if (extension.empty()) return false;
  const com_scope com;
  std::wstring want = exe;
  if (want.empty()) {
    const std::wstring def = default_exe(extension);
    // The default, unless that is MediaViewer: then the first other app.
    each_handler(extension, [&](IAssocHandler*, const std::wstring& e, const std::wstring&) {
      if (want.empty()) want = e;
      if (same_path(e, def)) {
        want = e;
        return true;
      }
      return false;
    });
    if (want.empty()) return false;
  }
  IShellItem* item = nullptr;
  if (FAILED(::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) return false;
  IDataObject* data = nullptr;
  const HRESULT bound = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
  item->Release();
  if (FAILED(bound) || !data) return false;
  bool opened = false;
  each_handler(extension, [&](IAssocHandler* h, const std::wstring& e, const std::wstring&) {
    if (!same_path(e, want)) return false;
    opened = SUCCEEDED(h->Invoke(data));
    return true;
  });
  data->Release();
  return opened;
}

}  // namespace mv::shell
