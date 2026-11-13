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
// lowest interface metric.
//
// Block when the tunnel is down: WFP filters in a sublayer of Driftwire's
// own (kSublayer) let out only the tunnel's adapter, the relay's own
// address, loopback, the local networks and DHCP, and block everything
// else, IPv4 and IPv6. Not a dynamic session: the filters outlive a crash
// (a kill switch that vanished with the process would leak), and are taken
// away by stop(), by the next start, or by a reboot (they are not persistent).
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <fwpmu.h>

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

// Driftwire's WFP sublayer: every kill-switch filter is in it, so one
// sweep finds them all (a crash's leftovers too). {9e1f6b52-4c7a-4d6e-b0b3-2d5f1a8c7e41}
constexpr GUID kSublayer = {0x9e1f6b52, 0x4c7a, 0x4d6e, {0xb0, 0xb3, 0x2d, 0x5f, 0x1a, 0x8c, 0x7e, 0x41}};

struct wfp {
  HANDLE engine = nullptr;
  bool open() {
    if (engine) return true;
    FWPM_SESSION0 session{};  // not FWPM_SESSION_FLAG_DYNAMIC: the filters outlive the process
    return ::FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine) == ERROR_SUCCESS;
  }
  void close() {
    if (engine) ::FwpmEngineClose0(engine);
    engine = nullptr;
  }
  // Every filter in the sublayer, and the sublayer.
  void clear() {
    if (!open()) return;
    FWPM_FILTER_ENUM_TEMPLATE0 t{};
    t.layerKey = GUID{};
    t.enumType = FWP_FILTER_ENUM_OVERLAPPING;
    t.actionMask = 0xFFFFFFFF;
    for (const GUID& layer : {FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V6}) {
      t.layerKey = layer;
      HANDLE e = nullptr;
      if (::FwpmFilterCreateEnumHandle0(engine, &t, &e) != ERROR_SUCCESS) continue;
      for (;;) {
        FWPM_FILTER0** got = nullptr;
        UINT32 n = 0;
        if (::FwpmFilterEnum0(engine, e, 64, &got, &n) != ERROR_SUCCESS || n == 0) {
          if (got) ::FwpmFreeMemory0(reinterpret_cast<void**>(&got));
          break;
        }
        for (UINT32 i = 0; i < n; ++i) {
          if (::IsEqualGUID(got[i]->subLayerKey, kSublayer)) (void)::FwpmFilterDeleteById0(engine, got[i]->filterId);
        }
        ::FwpmFreeMemory0(reinterpret_cast<void**>(&got));
      }
      (void)::FwpmFilterDestroyEnumHandle0(engine, e);
    }
    (void)::FwpmSubLayerDeleteByKey0(engine, &kSublayer);
  }
  bool add(const GUID& layer, FWP_ACTION_TYPE action, UINT8 weight, FWPM_FILTER_CONDITION0* conds, UINT32 n) {
    FWPM_FILTER0 f{};
    f.displayData.name = const_cast<wchar_t*>(L"Driftwire VPN: block when the tunnel is down");
    f.layerKey = layer;
    f.subLayerKey = kSublayer;
    f.action.type = action;
    f.weight.type = FWP_UINT8;
    f.weight.uint8 = weight;
    f.filterCondition = conds;
    f.numFilterConditions = n;
    UINT64 id = 0;
    return ::FwpmFilterAdd0(engine, &f, nullptr, &id) == ERROR_SUCCESS;
  }
  // Only the tunnel, the relay, loopback, the local networks and DHCP out.
  bool block(const NET_LUID& tunnel, const settings& s) {
    clear();
    if (!open()) return false;
    if (::FwpmTransactionBegin0(engine, 0) != ERROR_SUCCESS) return false;
    FWPM_SUBLAYER0 sub{};
    sub.subLayerKey = kSublayer;
    sub.displayData.name = const_cast<wchar_t*>(L"Driftwire VPN");
    sub.weight = 0xFFFF;
    bool ok = ::FwpmSubLayerAdd0(engine, &sub, nullptr) == ERROR_SUCCESS;
    UINT64 luid = tunnel.Value;
    for (const GUID& layer : {FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V6}) {
      const bool v4 = ::IsEqualGUID(layer, FWPM_LAYER_ALE_AUTH_CONNECT_V4);
      FWPM_FILTER_CONDITION0 c{};
      // The tunnel's own adapter.
      c.fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
      c.matchType = FWP_MATCH_EQUAL;
      c.conditionValue.type = FWP_UINT64;
      c.conditionValue.uint64 = &luid;
      ok = ok && add(layer, FWP_ACTION_PERMIT, 15, &c, 1);
      // Loopback (the add-on's own proxy and the extension's pump).
      FWPM_FILTER_CONDITION0 lo{};
      lo.fieldKey = FWPM_CONDITION_FLAGS;
      lo.matchType = FWP_MATCH_FLAGS_ALL_SET;
      lo.conditionValue.type = FWP_UINT32;
      lo.conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;
      ok = ok && add(layer, FWP_ACTION_PERMIT, 14, &lo, 1);
      if (v4) {
        // The relay's own address, the local networks, DHCP's broadcast.
        for (const auto& a : s.exclude) {
          if (a.v6) continue;
          FWPM_FILTER_CONDITION0 r{};
          r.fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
          r.matchType = FWP_MATCH_EQUAL;
          r.conditionValue.type = FWP_UINT32;
          r.conditionValue.uint32 = static_cast<UINT32>(a.bytes[0]) << 24 | static_cast<UINT32>(a.bytes[1]) << 16 |
                                    static_cast<UINT32>(a.bytes[2]) << 8 | a.bytes[3];
          ok = ok && add(layer, FWP_ACTION_PERMIT, 13, &r, 1);
        }
        for (const auto& n : s.exclude_nets) {
          if (n.base.v6 || n.prefix <= 0 || n.prefix > 32) continue;
          FWP_V4_ADDR_AND_MASK m{};
          m.addr = static_cast<UINT32>(n.base.bytes[0]) << 24 | static_cast<UINT32>(n.base.bytes[1]) << 16 |
                   static_cast<UINT32>(n.base.bytes[2]) << 8 | n.base.bytes[3];
          m.mask = n.prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> n.prefix);
          m.addr &= m.mask;
          FWPM_FILTER_CONDITION0 r{};
          r.fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
          r.matchType = FWP_MATCH_EQUAL;
          r.conditionValue.type = FWP_V4_ADDR_MASK;
          r.conditionValue.v4AddrMask = &m;
          ok = ok && add(layer, FWP_ACTION_PERMIT, 13, &r, 1);
        }
        FWPM_FILTER_CONDITION0 dhcp{};
        dhcp.fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
        dhcp.matchType = FWP_MATCH_EQUAL;
        dhcp.conditionValue.type = FWP_UINT16;
        dhcp.conditionValue.uint16 = 67;
        ok = ok && add(layer, FWP_ACTION_PERMIT, 12, &dhcp, 1);
      }
      // Everything else.
      ok = ok && add(layer, FWP_ACTION_BLOCK, 0, nullptr, 0);
    }
    if (!ok) {
      (void)::FwpmTransactionAbort0(engine);
      return false;
    }
    return ::FwpmTransactionCommit0(engine) == ERROR_SUCCESS;
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
    // Block when the tunnel is down (a crash's leftovers are swept either way).
    if (s.kill_switch) {
      if (!wfp_.block(luid, s)) {
        stop_locked();
        return err(status::io);
      }
    } else {
      wfp_.clear();
    }
    return std::unique_ptr<device>(std::make_unique<wintun_device>(w_, session_));
  }

  void stop() override {
    std::lock_guard lock(m_);
    stop_locked();
  }

 private:
  void stop_locked() {
    wfp_.clear();  // the kill switch's filters go with the tunnel
    wfp_.close();
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
  wfp wfp_;                                   // under m_
};

}  // namespace

std::unique_ptr<os_port> make_os_port() { return std::make_unique<win_port>(); }

}  // namespace mv::driftwire::vpn
