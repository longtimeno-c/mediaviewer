// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/host_ports.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace mv::driftwire::host_ports {

namespace {

// The mapper's notes that mean nobody answered (mapping.cpp's words); any
// other note is a router that answered.
bool silent(const std::string& note) {
  return note == "no router found to ask for a port" || note == "PCP: no answer" || note == "NAT-PMP: no answer" ||
         note == "UPnP: no gateway answered";
}

std::vector<std::string> notes_since(const mapping::mapper& m, std::size_t from) {
  const auto& n = m.notes();
  return {n.begin() + static_cast<std::ptrdiff_t>(std::min(from, n.size())), n.end()};
}

bool anyone_answered(const std::vector<std::string>& notes) {
  return std::any_of(notes.begin(), notes.end(), [](const std::string& s) { return !silent(s); });
}

// The ledger as it stands, written as each forward is made, so a crash in the
// middle of a start leaves nothing unlisted (mapping::keeper rewrites it).
void write_ledger(const std::string& path, const net::address& gateway, const std::vector<mapping::mapped>& held) {
  if (path.empty()) return;
  const std::string tmp = path + ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << mapping::ledger_text(gateway, held);
  }
  std::remove(path.c_str());
  (void)std::rename(tmp.c_str(), path.c_str());
}

std::string label(const port_wanted& w) { return w.what.empty() ? "a port" : "the " + w.what; }

}  // namespace

const char* port_state_name(port_state s) noexcept {
  switch (s) {
    case port_state::not_offered:
      return "not offered";
    case port_state::refused:
      return "refused";
    case port_state::mapped:
      return "mapped";
  }
  return "?";
}

const char* outside_kind_name(outside_kind k) noexcept {
  switch (k) {
    case outside_kind::unknown:
      return "unknown";
    case outside_kind::public_address:
      return "public";
    case outside_kind::double_nat:
      return "double NAT";
    case outside_kind::carrier_nat:
      return "carrier-grade NAT";
    case outside_kind::not_public:
      return "not public";
  }
  return "?";
}

outside_kind classify(const net::address& a) noexcept {
  if (net::is_public(a)) return outside_kind::public_address;
  // 0.0.0.0/8: what a NAT-PMP router answers while its WAN is down.
  if (!a.v6 && a.bytes[0] == 0) return outside_kind::unknown;
  // 100.64.0.0/10 (RFC 6598): the provider's shared address space.
  if (!a.v6 && a.bytes[0] == 100 && (a.bytes[1] & 0xc0) == 64) return outside_kind::carrier_nat;
  // 192.0.0.0/29 (RFC 6333): DS-Lite, the provider's NAT over IPv6.
  if (!a.v6 && a.bytes[0] == 192 && a.bytes[1] == 0 && a.bytes[2] == 0 && a.bytes[3] < 8) return outside_kind::carrier_nat;
  if (a.v6) return outside_kind::not_public;
  return outside_kind::double_nat;
}

bool forwardable(std::uint16_t port) noexcept {
  return port != 137 && port != 138 && port != 139 && port != 445 && port != 3389;
}

keeper::keeper(std::vector<port_wanted> ports, options o) : opts_(std::move(o)) {
  const auto cancelled = [&] { return opts_.router.cancelled && opts_.router.cancelled(); };
  const int lease = std::max(2, opts_.lease_s);
  // What a run that ended without removing its forwards left (a crash).
  if (!opts_.ledger.empty()) mapping::clean_leftovers(opts_.ledger, opts_.router);

  auto mapper = std::make_unique<mapping::mapper>(opts_.router);
  report r;
  r.router_found = mapper->gateway().has_value();
  if (r.router_found) r.this_computer = net::local_address_toward(*mapper->gateway());

  std::optional<net::address> v6 = opts_.local_v6;
  if (!v6) {
    for (const auto& a : net::local_addresses()) {
      if (a.v6 && net::is_public(a)) {
        v6 = a;
        break;
      }
    }
  }
  if (v6 && (!v6->v6 || !net::is_public(*v6))) v6.reset();

  std::vector<mapping::mapped> held;
  for (auto& w : ports) {
    port_report p;
    p.wanted = w;
    if (w.internal_port == 0) {
      p.v4 = port_state::refused;
      p.why.emplace_back("no port to forward");
      r.ports.push_back(std::move(p));
      continue;
    }
    // SMB, NetBIOS and RDP never leave the home network: refused before any router is asked,
    // inside or outside, a fallback included.
    if (!forwardable(w.internal_port) || !forwardable(w.external_port) || !forwardable(w.fallback_external)) {
      p.v4 = port_state::refused;
      p.why.emplace_back(kNeverForwarded);
      r.ports.push_back(std::move(p));
      continue;
    }
    const std::size_t before = mapper->notes().size();
    const std::uint16_t ext = w.external_port ? w.external_port : w.internal_port;
    std::optional<mapping::mapped> got;
    if (!cancelled()) got = mapper->map_tcp(w.internal_port, ext, lease);
    if (got && got->external.port != ext && w.fallback_external) {
      mapper->remove(*got);  // another port the router chose: never left behind (#499)
      got.reset();
    }
    // The fallback only where a router answered and said no: not behind
    // another NAT (no forward helps), not where nobody answers (twice the wait).
    if (!got && w.fallback_external && w.fallback_external != ext && !mapper->behind_another_nat() &&
        anyone_answered(notes_since(*mapper, before)) && !cancelled()) {
      got = mapper->map_tcp(w.internal_port, w.fallback_external, lease);
    }
    if (got) {
      p.v4 = port_state::mapped;
      p.how = got->how;
      p.external_port = got->external.port;
      held.push_back(*got);
      write_ledger(opts_.ledger, *mapper->gateway(), held);
    } else {
      p.v4 = anyone_answered(notes_since(*mapper, before)) ? port_state::refused : port_state::not_offered;
    }
    // PCP answered: UPnP is not searched for a pinhole besides (#499).
    if (w.v6 && v6 && r.router_found && !(got && got->how == mapping::method::pcp) && !cancelled()) {
      const std::size_t mark = mapper->notes().size();
      if (auto hole = mapper->pinhole_v6(*v6, w.internal_port, lease)) {
        p.v6 = port_state::mapped;
        held.push_back(*hole);
        write_ledger(opts_.ledger, *mapper->gateway(), held);
      } else {
        const auto said = notes_since(*mapper, mark);
        const bool offered = std::any_of(said.begin(), said.end(), [](const std::string& s) {
          return s.rfind("IPv6:", 0) == 0 && s != "IPv6: no router firewall to open (IGDv2)";
        });
        p.v6 = offered ? port_state::refused : port_state::not_offered;
      }
    }
    p.why = notes_since(*mapper, before);
    r.ports.push_back(std::move(p));
  }

  if (r.router_found && !mapper->last_outside() && !cancelled()) (void)mapper->outside_v4();
  r.cancelled = cancelled();
  r.outside = mapper->last_outside();
  if (r.outside) {
    r.kind = classify(*r.outside);
  } else if (mapper->behind_another_nat()) {
    r.kind = outside_kind::not_public;  // defensive: the mapper notes the address before it says so
  }
  // The report's outside is IPv4, and 0.0.0.0 is no address (a WAN that is down).
  if (r.outside && (r.outside->v6 || r.kind == outside_kind::unknown)) r.outside.reset();

  report_ = std::move(r);
  held_ = std::make_unique<mapping::keeper>(std::move(mapper), std::move(held), lease, opts_.ledger);

  if (report_.router_found) {
    mapping::options ask = opts_.router;
    auto theirs = opts_.router.cancelled;
    ask.cancelled = [this, theirs] { return stop_.load() || (theirs && theirs()); };
    ask_ = std::move(ask);
    asker_ = std::make_unique<mapping::mapper>(ask_);
    watch_ = std::thread([this] { watch(); });
  }
}

keeper::~keeper() {
  {
    std::lock_guard lock(m_);
    stop_.store(true);
  }
  cv_.notify_all();
  if (watch_.joinable()) watch_.join();
  held_.reset();  // every forward and pinhole removed, the ledger gone
}

report keeper::status() const {
  std::lock_guard lock(m_);
  return report_;
}

void keeper::watch() {
  const auto every = std::chrono::milliseconds(std::max(50, opts_.check_every_ms));
  for (;;) {
    {
      std::unique_lock lock(m_);
      if (cv_.wait_for(lock, every, [&] { return stop_.load(); })) return;
    }
    const auto now = asker_->outside_v4();
    if (stop_.load()) return;
    if (!now) {
      // No answer this time: the last address stands, and the next ask
      // starts afresh (a UPnP search the router missed is made again).
      asker_ = std::make_unique<mapping::mapper>(ask_);
      continue;
    }
    const outside_kind kind = classify(*now);
    if (kind == outside_kind::unknown) continue;  // 0.0.0.0: the WAN is down; the last address stands
    bool changed = false;
    {
      std::lock_guard lock(m_);
      changed = report_.outside != now;
      report_.outside = now;
      report_.kind = kind;
    }
    // Only a public address is told: a name must never point at the
    // provider's NAT or a private network.
    if (changed && kind == outside_kind::public_address && opts_.on_outside_changed) opts_.on_outside_changed(*now);
  }
}

std::vector<std::string> advice(const report& r) {
  std::vector<std::string> out;
  if (r.cancelled) return out;  // stopped while asking: nothing known to advise on
  const bool any_v6 = std::any_of(r.ports.begin(), r.ports.end(),
                                  [](const port_report& p) { return p.v6 == port_state::mapped; });
  switch (r.kind) {
    case outside_kind::carrier_nat:
      out.emplace_back(
          "Your internet provider shares one outside address among many homes (carrier-grade NAT). No router "
          "forward can reach this computer from outside, and a relay on this computer cannot either.");
      out.emplace_back(
          "Phones will reach it at home only, unless you run mv-driftwire-relay on a small rented server "
          "(a VPS) and set that as your relay.");
      if (any_v6) {
        out.emplace_back("IPv6 is open to this computer, so a phone on an IPv6 network may still reach it.");
      } else {
        out.emplace_back("Asking your provider for a public IPv4 address, or for IPv6, also gets around it.");
      }
      return out;  // a forward by hand cannot help either
    case outside_kind::double_nat:
      out.emplace_back(
          "Your router is behind another router (double NAT), so its forwards do not reach the internet. Set "
          "the outer one (often the provider's modem) to bridge mode, or forward the same ports on both.");
      break;
    case outside_kind::not_public:
      out.emplace_back(
          "Your router faces the internet from an address that is not public. If another router is beyond it "
          "(double NAT), set that one to bridge mode or forward the same ports on both; if your provider "
          "shares one address among many homes (carrier-grade NAT), no forward helps, and a relay on a "
          "rented server is needed.");
      break;
    case outside_kind::unknown:
    case outside_kind::public_address:
      break;
  }
  bool missing = false;
  const bool all_silent = std::all_of(r.ports.begin(), r.ports.end(),
                                      [](const port_report& p) { return p.v4 == port_state::not_offered; });
  for (const auto& p : r.ports) {
    if (p.v4 == port_state::mapped || p.wanted.internal_port == 0) continue;
    // Never told to forward one by hand: it stays on the home network.
    if (!forwardable(p.wanted.internal_port) || !forwardable(p.wanted.external_port) ||
        !forwardable(p.wanted.fallback_external)) {
      continue;
    }
    missing = true;
    const std::uint16_t internal = p.wanted.internal_port;
    // The fallback is what a hand forward asks for: the preferred port (443)
    // may be someone else's forward.
    const std::uint16_t outside = p.wanted.fallback_external
                                      ? p.wanted.fallback_external
                                      : (p.wanted.external_port ? p.wanted.external_port : internal);
    std::string line = outside == internal
                           ? "Forward TCP port " + std::to_string(internal) + " on your router to this computer"
                           : "Forward outside TCP port " + std::to_string(outside) + " on your router to this computer's port " +
                                 std::to_string(internal);
    line += " (" + label(p.wanted) + ")";
    if (p.wanted.fallback_external && p.wanted.external_port && p.wanted.external_port != outside) {
      line += "; outside port " + std::to_string(p.wanted.external_port) + " instead if nothing else uses it";
    }
    line += ".";
    out.push_back(std::move(line));
  }
  if (missing) {
    if (!r.router_found) {
      out.insert(out.begin(), "No router was found on this network to ask for a port.");
    } else if (all_silent) {
      out.insert(out.begin(), "Your router did not answer a request to open a port (UPnP or NAT-PMP may be off in its settings).");
    }
  }
  return out;
}

}  // namespace mv::driftwire::host_ports
