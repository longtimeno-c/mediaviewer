// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN's interface on Windows (vpn.h): a Wintun adapter.
//
// wintun.dll is loaded at run time from beside the add-on, never linked, and
// is not shipped yet: its signed prebuilt DLL comes under WireGuard LLC's
// "Prebuilt Binaries License", which a GPL-3.0 program can carry only with
// an additional permission from MediaViewer's copyright holder (the owner's
// call, docs/design/11). Its API (wintun.h, GPL-2.0 OR MIT) is declared here
// under MIT. Without the DLL, unavailable() says so and the proxy carries on.
//
// Making an adapter, its address and its routes needs an administrator; the
// app runs as the person. Until an elevated helper exists (owed), start()
// works only when MediaViewer runs elevated, and says so otherwise.
//
// Routes: 0.0.0.0/1 and 128.0.0.0/1 through the adapter (they win over the
// default route without touching it), and the relay's own address through
// the route it had before. The local networks keep their own, more specific,
// routes. DNS: the adapter's resolver is the tunnel's (198.18.0.2), with the
// lowest interface metric. Block when the tunnel is down needs WFP filters:
// not built on Windows (block_unavailable()).
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <string>
#include <vector>

#include "addons/driftwire/net.h"
#include "addons/driftwire/vpn.h"

namespace mv::driftwire::vpn {

namespace {

// wintun.h, taken under MIT.
using WINTUN_ADAPTER_HANDLE = struct wintun_adapter*;
using WINTUN_SESSION_HANDLE = struct wintun_session*;
using create_adapter_fn = WINTUN_ADAPTER_HANDLE(WINAPI*)(LPCWSTR, LPCWSTR, const GUID*);
using close_adapter_fn = void(WINAPI*)(WINTUN_ADAPTER_HANDLE);
using get_luid_fn = void(WINAPI*)(WINTUN_ADAPTER_HANDLE, NET_LUID*);
using start_session_fn = WINTUN_SESSION_HANDLE(WINAPI*)(WINTUN_ADAPTER_HANDLE, DWORD);
using end_session_fn = void(WINAPI*)(WINTUN_SESSION_HANDLE);
using read_event_fn = HANDLE(WINAPI*)(WINTUN_SESSION_HANDLE);
using receive_fn = BYTE*(WINAPI*)(WINTUN_SESSION_HANDLE, DWORD*);
using release_fn = void(WINAPI*)(WINTUN_SESSION_HANDLE, const BYTE*);
using allocate_fn = BYTE*(WINAPI*)(WINTUN_SESSION_HANDLE, DWORD);
using send_fn = void(WINAPI*)(WINTUN_SESSION_HANDLE, const BYTE*);

struct wintun {
  HMODULE dll = nullptr;
  create_adapter_fn create = nullptr;
  close_adapter_fn close = nullptr;
  get_luid_fn luid = nullptr;
  start_session_fn start = nullptr;
  end_session_fn end = nullptr;
  read_event_fn event = nullptr;
  receive_fn receive = nullptr;
  release_fn release = nullptr;
  allocate_fn allocate = nullptr;
  send_fn send = nullptr;

  bool load() {
    if (dll) return true;
    // Beside the add-on (this module), never the search path.
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&make_os_port), &self)) {
      return false;
    }
    wchar_t path[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    dll = ::LoadLibraryExW((dir + L"wintun.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return false;
    const auto get = [&](auto& f, const char* name) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(::GetProcAddress(dll, name));
      return f != nullptr;
    };
    if (!get(create, "WintunCreateAdapter") || !get(close, "WintunCloseAdapter") ||
        !get(luid, "WintunGetAdapterLUID") || !get(start, "WintunStartSession") || !get(end, "WintunEndSession") ||
        !get(event, "WintunGetReadWaitEvent") || !get(receive, "WintunReceivePacket") ||
        !get(release, "WintunReleaseReceivePacket") || !get(allocate, "WintunAllocateSendPacket") ||
        !get(send, "WintunSendPacket")) {
      ::FreeLibrary(dll);
      dll = nullptr;
      return false;
    }
    return true;
  }
};

bool elevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION e{};
  DWORD size = 0;
  const bool ok = ::GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
  ::CloseHandle(token);
  return ok;
}

bool ipv4(const std::string& text, IN_ADDR& out) {
  const auto a = net::parse_address(text);
  if (!a || a->v6) return false;
  std::memcpy(&out, a->bytes.data(), 4);
  return true;
}

class wintun_device final : public device {
 public:
  wintun_device(wintun& w, WINTUN_SESSION_HANDLE s) : w_(w), s_(s), event_(w.event(s)) {}
  ~wintun_device() override { shutdown(); }
  result<std::size_t> read(std::span<std::uint8_t> into, int timeout_ms) override {
    for (;;) {
      if (gone_.load()) return err(status::io);
      DWORD size = 0;
      BYTE* p = w_.receive(s_, &size);
      if (p) {
        const std::size_t n = std::min<std::size_t>(size, into.size());
        std::memcpy(into.data(), p, n);
        w_.release(s_, p);
        return n;
      }
      if (::GetLastError() != ERROR_NO_MORE_ITEMS) return err(status::io);
      if (::WaitForSingleObject(event_, static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) return err(status::timeout);
    }
  }
  expected write(std::span<const std::uint8_t> packet) override {
    if (gone_.load()) return err(status::io);
    BYTE* p = w_.allocate(s_, static_cast<DWORD>(packet.size()));  // thread-safe in Wintun
    if (!p) return err(status::io);
    std::memcpy(p, packet.data(), packet.size());
    w_.send(s_, p);
    return {};
  }
  void shutdown() noexcept override { gone_.store(true); }

 private:
  wintun& w_;
  WINTUN_SESSION_HANDLE s_;
  HANDLE event_;
  std::atomic<bool> gone_{false};
};

class win_port final : public os_port {
 public:
  ~win_port() override { stop(); }

  std::string unavailable() override {
    std::lock_guard lock(m_);
    if (!w_.load()) {
      return "All traffic (VPN) needs Wintun beside Driftwire, which this build does not carry yet (its licence "
             "waits for a decision). The proxy works without it.";
    }
    return {};
  }
  std::string block_unavailable() override {
    return "Block when the tunnel is down is not on Windows yet.";
  }

  result<std::unique_ptr<device>> start(const settings& s) override {
    std::lock_guard lock(m_);
    if (!w_.load()) return err(status::unsupported_format);
    if (!elevated()) return err(status::permission_denied);  // owed: an elevated helper
    stop_locked();
    // The route to the relay before ours exist: kept for it alone.
    for (const auto& a : s.exclude) {
      if (a.v6) continue;
      SOCKADDR_INET dst{};
      dst.Ipv4.sin_family = AF_INET;
      std::memcpy(&dst.Ipv4.sin_addr, a.bytes.data(), 4);
      MIB_IPFORWARD_ROW2 best{};
      SOCKADDR_INET src{};
      if (::GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &best, &src) == NO_ERROR) {
        MIB_IPFORWARD_ROW2 r;
        ::InitializeIpForwardEntry(&r);
        r.InterfaceLuid = best.InterfaceLuid;
        r.DestinationPrefix.Prefix = dst;
        r.DestinationPrefix.PrefixLength = 32;
        r.NextHop = best.NextHop;
        r.Protocol = MIB_IPPROTO_NETMGMT;
        r.Metric = 0;
        if (::CreateIpForwardEntry2(&r) == NO_ERROR) kept_routes_.push_back(r);
      }
    }
    adapter_ = w_.create(L"Driftwire", L"Driftwire", nullptr);
    if (!adapter_) {
      stop_locked();
      return err(::GetLastError() == ERROR_ACCESS_DENIED ? status::permission_denied : status::io);
    }
    NET_LUID luid{};
    w_.luid(adapter_, &luid);
    // The adapter's address.
    MIB_UNICASTIPADDRESS_ROW addr;
    ::InitializeUnicastIpAddressEntry(&addr);
    addr.InterfaceLuid = luid;
    addr.Address.Ipv4.sin_family = AF_INET;
    if (!ipv4(s.address4, addr.Address.Ipv4.sin_addr)) {
      stop_locked();
      return err(status::invalid_arg);
    }
    addr.OnLinkPrefixLength = static_cast<UINT8>(s.prefix4);
    addr.DadState = IpDadStatePreferred;
    if (::CreateUnicastIpAddressEntry(&addr) != NO_ERROR) {
      stop_locked();
      return err(status::io);
    }
    // Its metric, so its DNS server is asked first; its MTU.
    MIB_IPINTERFACE_ROW iface;
    ::InitializeIpInterfaceEntry(&iface);
    iface.InterfaceLuid = luid;
    iface.Family = AF_INET;
    if (::GetIpInterfaceEntry(&iface) == NO_ERROR) {
      iface.UseAutomaticMetric = FALSE;
      iface.Metric = 1;
      iface.NlMtu = s.mtu;
      iface.SitePrefixLength = 0;
      (void)::SetIpInterfaceEntry(&iface);
    }
    // 0.0.0.0/1 and 128.0.0.0/1 through it.
    for (const char* half : {"0.0.0.0", "128.0.0.0"}) {
      MIB_IPFORWARD_ROW2 r;
      ::InitializeIpForwardEntry(&r);
      r.InterfaceLuid = luid;
      r.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
      ipv4(half, r.DestinationPrefix.Prefix.Ipv4.sin_addr);
      r.DestinationPrefix.PrefixLength = 1;
      r.NextHop.Ipv4.sin_family = AF_INET;  // on the link
      r.Protocol = MIB_IPPROTO_NETMGMT;
      r.Metric = 0;
      if (::CreateIpForwardEntry2(&r) != NO_ERROR) {
        stop_locked();
        return err(status::io);
      }
    }
    // The tunnel's resolver on the adapter.
    GUID guid{};
    if (::ConvertInterfaceLuidToGuid(&luid, &guid) == NO_ERROR) {
      std::wstring dns(s.dns4.begin(), s.dns4.end());
      DNS_INTERFACE_SETTINGS d{};
      d.Version = DNS_INTERFACE_SETTINGS_VERSION1;
      d.Flags = DNS_SETTING_NAMESERVER;
      d.NameServer = dns.data();
      (void)::SetInterfaceDnsSettings(guid, &d);
    }
    session_ = w_.start(adapter_, 0x400000);  // 4 MiB ring
    if (!session_) {
      stop_locked();
      return err(status::io);
    }
    return std::unique_ptr<device>(std::make_unique<wintun_device>(w_, session_));
  }

  void stop() override {
    std::lock_guard lock(m_);
    stop_locked();
  }

 private:
  void stop_locked() {
    if (session_) w_.end(session_);
    session_ = nullptr;
    if (adapter_) w_.close(adapter_);  // its address, routes and DNS go with it
    adapter_ = nullptr;
    for (auto& r : kept_routes_) (void)::DeleteIpForwardEntry2(&r);
    kept_routes_.clear();
  }

  std::mutex m_;
  wintun w_;                                  // under m_
  WINTUN_ADAPTER_HANDLE adapter_ = nullptr;   // under m_
  WINTUN_SESSION_HANDLE session_ = nullptr;   // under m_
  std::vector<MIB_IPFORWARD_ROW2> kept_routes_;  // under m_
};

}  // namespace

std::unique_ptr<os_port> make_os_port() { return std::make_unique<win_port>(); }

}  // namespace mv::driftwire::vpn
