// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// OneDrive (and any Cloud Files API provider) for cloud_files.h. In-box since
// Windows 10 1709: cldapi for hydrate / dehydrate, the Network List Manager for
// whether the connection is metered.
#include <windows.h>
// cfapi.h uses NTSTATUS, which windows.h leaves out.
#include <winternl.h>

#include <cfapi.h>
#include <netlistmgr.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "addons/ai/cloud_files.h"

#pragma comment(lib, "cldapi.lib")
#pragma comment(lib, "ole32.lib")

namespace mv::ai {
namespace {

std::wstring wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

struct handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~handle() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};

// Opening a placeholder never hydrates it; reading would.
HANDLE open_placeholder(const std::wstring& path, DWORD access) {
  return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                     OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

class onedrive final : public cloud_files {
 public:
  expected hydrate(const std::string& path, const std::function<bool(double)>& progress) override {
    handle f{open_placeholder(wide(path), GENERIC_READ)};
    if (f.h == INVALID_HANDLE_VALUE) return err(status::not_found);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f.h, &size)) return err(status::io);
    // In slices, so a large clip reports progress and a stop is heard between
    // them (CfHydratePlaceholder itself cannot be interrupted).
    constexpr std::int64_t kSlice = 16 << 20;
    for (std::int64_t at = 0; at < size.QuadPart; at += kSlice) {
      if (progress && !progress(static_cast<double>(at) / static_cast<double>(size.QuadPart))) {
        return err(status::cancelled);
      }
      LARGE_INTEGER off{}, len{};
      off.QuadPart = at;
      len.QuadPart = std::min<std::int64_t>(kSlice, size.QuadPart - at);
      const HRESULT hr = CfHydratePlaceholder(f.h, off, len, CF_HYDRATE_FLAG_NONE, nullptr);
      if (FAILED(hr)) {
        // Not a Cloud Files placeholder (an old-style offline file): leave it.
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_A_CLOUD_FILE)) return err(status::unsupported_format);
        return err(status::io);
      }
    }
    if (progress) (void)progress(1.0);
    return {};
  }

  expected dehydrate(const std::string& path) override {
    handle f{open_placeholder(wide(path), GENERIC_READ | FILE_WRITE_ATTRIBUTES)};
    if (f.h == INVALID_HANDLE_VALUE) return err(status::not_found);
    LARGE_INTEGER off{}, len{};
    len.QuadPart = -1;  // to the end
    const HRESULT hr = CfDehydratePlaceholder(f.h, off, len, CF_DEHYDRATE_FLAG_NONE, nullptr);
    return SUCCEEDED(hr) ? expected{} : err(status::io);
  }

  bool network_unmetered() override {
    // Asked about once a second while the option is on; the answer is kept
    // for ten seconds (a COM round trip each time is not worth it).
    std::lock_guard lock(m_);
    const auto now = std::chrono::steady_clock::now();
    if (now - checked_ < std::chrono::seconds(10) && checked_.time_since_epoch().count() != 0) return last_;
    checked_ = now;
    last_ = ask();
    return last_;
  }

 private:
  static bool ask() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    INetworkListManager* nlm = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager,
                                   reinterpret_cast<void**>(&nlm)))) {
      VARIANT_BOOL online = VARIANT_FALSE;
      if (SUCCEEDED(nlm->get_IsConnectedToInternet(&online)) && online == VARIANT_TRUE) {
        INetworkCostManager* cost = nullptr;
        if (SUCCEEDED(nlm->QueryInterface(IID_INetworkCostManager, reinterpret_cast<void**>(&cost)))) {
          DWORD c = 0;
          if (SUCCEEDED(cost->GetCost(&c, nullptr))) {
            ok = (c & NLM_CONNECTION_COST_UNRESTRICTED) != 0 &&
                 (c & (NLM_CONNECTION_COST_OVERDATALIMIT | NLM_CONNECTION_COST_CONGESTED)) == 0;
          }
          cost->Release();
        } else {
          ok = true;  // no cost information: an ordinary wired / Wi-Fi network
        }
      }
      nlm->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
  }

  std::mutex m_;
  std::chrono::steady_clock::time_point checked_{};
  bool last_ = false;
};

}  // namespace

std::unique_ptr<cloud_files> make_cloud_files() { return std::make_unique<onedrive>(); }

}  // namespace mv::ai
