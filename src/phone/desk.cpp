// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phone/desk.h"

#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

#include "wire/advert.h"
#include "wire/known.h"
#include "wire/pair_check.h"
#include "wire/serve_client.h"
#include "wire/keys.h"
#include "core/json.h"

namespace mv::driftwire::desk {

namespace {

namespace fs = std::filesystem;

// The nearby service's one named folder.
constexpr const char* kNearbyRoot = "c47a10601e000001";

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())); }

// What the nearby service reads: in memory, so its timer never wakes the Keychain.
class memory_store final : public secrets::store {
 public:
  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    std::lock_guard lock(m_);
    v_[name].assign(value.begin(), value.end());
    return true;
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    std::lock_guard lock(m_);
    const auto it = v_.find(name);
    if (it == v_.end()) return std::nullopt;
    return it->second;
  }
  bool erase(const std::string& name) override {
    std::lock_guard lock(m_);
    v_.erase(name);
    return true;
  }

 private:
  std::mutex m_;
  std::map<std::string, std::vector<std::uint8_t>> v_;
};

std::string service_json(const service_info& s) {
  json::writer w;
  w.begin_object();
  w.key("local").string(s.local);
  w.key("public").string(s.public_name);
  w.key("identity").string(key_hex(s.identity));
  w.key("access_key").string(key_hex(s.access_key));
  w.end_object();
  return w.take();
}

std::optional<service_info> parse_service(std::string_view text) {
  const auto doc = json::parse(text, 4);
  if (!doc) return std::nullopt;
  const std::string* local = doc->str("local");
  const std::string* pub = doc->str("public");
  const std::string* identity = doc->str("identity");
  const std::string* access = doc->str("access_key");
  if (!local || !pub || !identity || !access) return std::nullopt;
  const auto id = parse_key_hex(*identity);
  const auto key = parse_key_hex(*access);
  if (!id || !key) return std::nullopt;
  service_info s;
  s.local = *local;
  s.public_name = *pub;
  s.identity = *id;
  s.access_key = *key;
  return s;
}

std::string random_hex(std::size_t bytes) {
  std::vector<std::uint8_t> b(bytes);
  randombytes_buf(b.data(), b.size());
  std::string out(bytes * 2 + 1, '\0');
  sodium_bin2hex(out.data(), out.size(), b.data(), b.size());
  out.resize(bytes * 2);
  return out;
}

result<std::unique_ptr<link>> default_connect(const std::string& host, std::uint16_t port, int timeout_ms) {
  const auto addresses = net::resolve(host);
  status last = status::io;
  for (const net::address& a : addresses) {
    auto l = net::connect({net::address_text(a), port}, timeout_ms);
    if (l) return l;
    last = l.error();
  }
  return err(last);
}

}  // namespace

void forget(secrets::store& store) {
  (void)store.erase(kKeyName);
  (void)store.erase(kServiceName);
  (void)store.erase(kDevicesName);
  (void)store.erase(kPendingName);
}

void take_items(secrets::store& from, secrets::store& to) {
  for (const char* name : {kKeyName, kServiceName, kDevicesName, kPendingName}) {
    if (to.exists(name)) continue;
    auto value = from.get(name);
    if (!value) continue;
    if (!to.put(name, *value)) continue;
    auto back = to.get(name);
    if (back && *back == *value) (void)from.erase(name);
  }
}

std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text) {
  std::string_view host, port;
  if (text.starts_with('[')) {
    const auto close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
    host = text.substr(1, close - 1);
    port = text.substr(close + 2);
  } else {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || text.find(':', colon + 1) != std::string_view::npos) return std::nullopt;
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
  }
  if (host.empty() || port.empty() || host.find_first_of(" \t/") != std::string_view::npos) return std::nullopt;
  unsigned value = 0;
  const auto [end, why] = std::from_chars(port.data(), port.data() + port.size(), value);
  if (why != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535) return std::nullopt;
  return std::make_pair(std::string(host), static_cast<std::uint16_t>(value));
}

address_warning classify_addresses(std::span<const net::address> addresses) noexcept {
  if (addresses.empty()) return address_warning::unresolved;
  bool carrier = false;
  for (const net::address& a : addresses) {
    if (net::is_public(a)) return address_warning::none;
    if (!a.v6 && a.bytes[0] == 100 && (a.bytes[1] & 0xC0) == 64) carrier = true;
  }
  return carrier ? address_warning::carrier_nat : address_warning::private_range;
}

const char* warning_name(address_warning w) noexcept {
  switch (w) {
    case address_warning::none: return "";
    case address_warning::private_range: return "private";
    case address_warning::carrier_nat: return "carrier";
    case address_warning::unresolved: return "unresolved";
  }
  return "";
}

struct desk::state {
  options o;
  std::mutex m;  // one operation at a time: each may wait on the NAS
  std::optional<service_info> info;
  bool loaded = false;
  std::unique_ptr<serve::client> admin;  // kept through a pairing, so polling is one connection
  bool waiting = false;                   // the last code is still on the service
  // The owner's unlock (§3.4): the key the passphrase made, in memory only and
  // only this long (call C2), with the salt and cost it was made for.
  std::optional<serve::owner::signer> signer;
  serve::owner::salt_t signer_salt{};
  std::uint64_t signer_ops = 0, signer_mem = 0;
  std::chrono::steady_clock::time_point signer_until{};
  std::string refusal = "{}";  // the NAS's last refusal
  serve::end_code last_end = serve::end_code::ok;  // how the last request's stream ended
  bool claim_passphrase_set = false;              // the last claim took the passphrase step

  void forget_signer() {
    signer.reset();
    signer_until = {};
  }
  // Past its ten minutes the key goes, used or not.
  void expire_signer() {
    if (signer && std::chrono::steady_clock::now() >= signer_until) forget_signer();
  }
  void keep_signer(const serve::owner::signer& s, const serve::owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) {
    signer = s;
    signer_salt = salt;
    signer_ops = ops;
    signer_mem = mem;
    signer_until = std::chrono::steady_clock::now() + std::chrono::seconds(serve::owner::kUnlockSeconds);
  }
  // A derive for client::unlock that runs Argon2id over `passphrase` and keeps what it made.
  serve::client::derive_fn deriving(const std::string& passphrase) {
    return [this, passphrase](const serve::owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) {
      auto s = serve::owner::derive(passphrase, salt, ops, mem);
      if (s) keep_signer(*s, salt, ops, mem);
      return s;
    };
  }
  // The kept key, for the salt and cost it was made for, while it lasts.
  serve::client::derive_fn kept() {
    return [this](const serve::owner::salt_t& salt, std::uint64_t ops, std::uint64_t mem) -> std::optional<serve::owner::signer> {
      if (!signer || std::chrono::steady_clock::now() >= signer_until || salt != signer_salt || ops != signer_ops ||
          mem != signer_mem) {
        return std::nullopt;
      }
      return *signer;
    };
  }
  [[nodiscard]] std::string why_of_refusal() const {
    const auto doc = json::parse(refusal, 2);
    const std::string* why = doc ? doc->str("why") : nullptr;
    return why ? *why : std::string();
  }

  // The nearby service.
  mutable std::mutex nearby_m;
  std::unique_ptr<memory_store> nearby_store;
  std::unique_ptr<serve::server> nearby;
  std::unique_ptr<advert::registration> advert;
  std::string nearby_folder, nearby_library;

  void load() {
    if (loaded) return;
    loaded = true;
    if (auto v = o.store->get(kServiceName)) info = parse_service(std::string_view(reinterpret_cast<const char*>(v->data()), v->size()));
  }
  [[nodiscard]] bool save() {
    const std::string text = service_json(*info);
    return o.store->put(kServiceName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  }
  [[nodiscard]] result<key32> device_key() {
    key32 k;
    if (auto v = o.store->get(kKeyName); v && v->size() == k.size()) {
      std::memcpy(k.data(), v->data(), k.size());
      sodium_memzero(v->data(), v->size());
      return k;
    }
    if (o.store->exists(kKeyName)) return err(status::io);  // there and unreadable: never replaced
    randombytes_buf(k.data(), k.size());
    if (!o.store->put(kKeyName, k)) return err(status::io);
    return k;
  }
  [[nodiscard]] result<std::unique_ptr<link>> connect_to(const std::string& host_port) {
    const auto where = split_host_port(host_port);
    if (!where) return err(status::invalid_arg);
    if (o.connect) return o.connect(where->first, where->second, kConnectMs);
    return default_connect(where->first, where->second, kConnectMs);
  }
  // The owner's session: the one held, or the first address that answers.
  [[nodiscard]] result<serve::client*> session() {
    if (!info) return err(status::not_found);
    if (admin && !admin->closed()) return admin.get();
    admin.reset();
    MV_TRY(const key32 key, device_key());
    serve::client::options c;
    c.access_key = info->access_key;
    c.device_key = key;
    c.service_key = info->identity;
    c.id = "0000000000000000";
    c.name = o.device_name;
    c.ops = serve::kAllOps | serve::kAdminOp;
    status last = status::io;
    for (const std::string* where : {&info->local, &info->public_name}) {
      if (where->empty()) continue;
      auto l = connect_to(*where);
      if (!l) {
        last = l.error();
        continue;
      }
      auto opened = serve::client::open(std::shared_ptr<link>(std::move(*l)), c);
      if (opened) {
        admin = std::move(*opened);
        return admin.get();
      }
      last = opened.error();
    }
    return err(last == status::invalid_arg ? status::io : last);
  }
  // One request; a held session that died is replaced once. Refused for want
  // of an unlock, with the key still kept here: unlocked with it and asked again.
  [[nodiscard]] result<std::string> ask(const std::string& request) {
    bool unlocked_again = false;
    for (int attempt = 0;; ++attempt) {
      MV_TRY(serve::client * c, session());
      std::string why;
      auto r = c->admin(request, &last_end, &why);
      refusal = r ? std::string("{}") : (why.empty() ? std::string("{}") : why);
      if (!r && !unlocked_again && why_of_refusal() == "needs-unlock" && signer &&
          std::chrono::steady_clock::now() < signer_until) {
        unlocked_again = true;
        std::string unlock_why;
        auto u = c->unlock(kept(), &unlock_why, o.floor_ops, o.floor_mem);
        if (u) {
          r = c->admin(request, &last_end, &why);
          refusal = r ? std::string("{}") : (why.empty() ? std::string("{}") : why);
        } else {
          const auto d = json::parse(unlock_why, 2);
          const std::string* reason = d ? d->str("why") : nullptr;
          // Only a definite no makes the person type it again: the passphrase
          // changed, or the key no longer fits the NAS's salt and cost. A
          // slow-down, a lockout or a dropped link keeps it for the next try.
          if (u.error() == status::corrupt || (reason && (*reason == "passphrase" || *reason == "passphrase-changed"))) {
            forget_signer();
          }
          if (reason && !reason->empty()) refusal = unlock_why;  // say why, with its "until"
        }
      }
      if (r || attempt == 1 || !admin->closed()) return r;
      admin.reset();
    }
  }
  [[nodiscard]] std::vector<serve::grants::grant> local_grants() {
    std::vector<serve::grants::grant> out;
    if (auto v = o.store->get(kGrantsName)) {
      if (auto list = serve::grants::decode(std::string_view(reinterpret_cast<const char*>(v->data()), v->size()))) {
        out = std::move(*list);
      }
    }
    return out;
  }
  [[nodiscard]] std::vector<device_row> local_devices() {
    std::vector<device_row> out;
    for (const auto& d : known::load_named(*o.store, kDevicesName)) {
      device_row r;
      r.name = d.name;
      r.key = d.key;
      out.push_back(std::move(r));
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    for (const auto& g : local_grants()) {
      device_row r;
      r.name = g.name;
      r.key = g.key;
      r.limited = true;
      r.expires = g.expires;
      r.expired = g.expired(now);
      r.guest = g.guest;
      r.scoped = g.roots.has_value();
      out.push_back(std::move(r));
    }
    return out;
  }
  // The unlimited phones in the paired list and the limited ones in the
  // grants, never both, here and in the nearby service's store, so the Mac
  // serves a guest nearby only as the NAS would (§6.5). `roots` carries a
  // scoped grant's folders.
  [[nodiscard]] bool save_local(const std::vector<device_row>& rows,
                                const std::map<key32, std::vector<std::string>>& roots = {}) {
    std::vector<known::device> list;
    std::vector<serve::grants::grant> limited;
    // A scoped grant keeps the folders it had here unless the NAS's answer gave new ones.
    std::map<key32, std::vector<std::string>> known_roots;
    for (const auto& g : local_grants()) {
      if (g.roots) known_roots[g.key] = *g.roots;
    }
    for (const auto& [k, ids] : roots) known_roots[k] = ids;
    for (const auto& r : rows) {
      if (r.limited) {
        serve::grants::grant g;
        g.name = r.name;
        g.key = r.key;
        g.expires = r.expires;
        g.guest = r.guest;
        if (r.scoped) {
          if (const auto it = known_roots.find(r.key); it != known_roots.end()) g.roots = it->second;
        }
        if (r.scoped && !g.roots) g.roots = std::vector<std::string>{};  // its folders unknown here: none nearby
        if (g.expires == 0 && !g.roots) g.roots = std::vector<std::string>{};
        limited.push_back(std::move(g));
        continue;
      }
      known::device d;
      d.name = r.name;
      d.key = r.key;
      list.push_back(std::move(d));
    }
    const auto bytes = known::encode(list);
    const std::string grants_text = serve::grants::encode(limited);
    const std::span<const std::uint8_t> grants_bytes(reinterpret_cast<const std::uint8_t*>(grants_text.data()), grants_text.size());
    bool saved = o.store->put(kDevicesName, bytes);
    saved = (limited.empty() ? (o.store->erase(kGrantsName), true) : o.store->put(kGrantsName, grants_bytes)) && saved;
    std::lock_guard lock(nearby_m);
    if (nearby_store) {
      (void)nearby_store->put(serve::grants::kGrantsName, grants_bytes);
      (void)nearby_store->put(known::kDevicesName, bytes);
      if (nearby) nearby->recheck_devices();
    }
    return saved;
  }
  [[nodiscard]] std::vector<key32> pending() {
    std::vector<key32> out;
    if (auto v = o.store->get(kPendingName)) {
      for (std::size_t i = 0; i + 32 <= v->size(); i += 32) {
        key32 k;
        std::memcpy(k.data(), v->data() + i, 32);
        out.push_back(k);
      }
    }
    return out;
  }
  void save_pending(const std::vector<key32>& keys) {
    if (keys.empty()) {
      (void)o.store->erase(kPendingName);
      return;
    }
    std::vector<std::uint8_t> bytes;
    for (const auto& k : keys) bytes.insert(bytes.end(), k.begin(), k.end());
    (void)o.store->put(kPendingName, bytes);
  }
  [[nodiscard]] expected tell_revoke(const key32& key) {
    json::writer w;
    w.begin_object().key("do").string("revoke").key("key").string(key_hex(key)).end_object();
    auto r = ask(w.str());
    // Already off the list is what was wanted.
    if (!r && r.error() != status::not_found) return err(r.error());
    return {};
  }
  void start_advert(std::uint16_t port) {
    if (!o.advertise) return;
    advert = advert::publish(port, {{"v", "1"}, {"lib", library_tag(nearby_library)}});
  }
};

desk::desk(options o) : st_(std::make_unique<state>()) {
  // A failed init leaves every later sodium call failing its own checks; the
  // result is kept so -Werror=unused-result builds (GCC ignores a (void) cast).
  [[maybe_unused]] const int sodium_ready = sodium_init();
  st_->o = std::move(o);
}

desk::~desk() { stop_nearby(); }

std::optional<service_info> desk::service() {
  std::lock_guard lock(st_->m);
  st_->load();
  return st_->info;
}

expected desk::claim(const std::string& share_folder, const std::string& local, const std::string& public_name,
                     const std::string& passphrase, bool new_passphrase) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (!split_host_port(local)) return err(status::invalid_arg);
  if (!public_name.empty() && !split_host_port(public_name)) return err(status::invalid_arg);
  MV_TRY(const key32 key, st_->device_key());

  // The passphrase is stretched before anything touches the share or the
  // network: Argon2id takes about a second, which the claim's deadline
  // should not pay for.
  std::optional<serve::claim_passphrase> pass;
  if (!passphrase.empty()) {
    pass.emplace();
    pass->sign = st_->deriving(passphrase);
    if (new_passphrase) {
      serve::owner::signer made;
      pass->set = serve::owner::make(passphrase, static_cast<std::int64_t>(std::time(nullptr)), st_->o.pass_ops,
                                     st_->o.pass_mem, &made);
      if (!pass->set) {
        st_->refusal = serve::owner::acceptable(passphrase) ? "{}" : "{\"why\":\"passphrase-short\"}";
        return err(status::invalid_arg);  // too short, or no memory for it
      }
      st_->keep_signer(made, pass->set->salt, pass->set->ops, pass->set->mem);
    }
    // The NAS claimed before (a re-claim): only that identity is answered.
    if (st_->info) pass->service_key = st_->info->identity;
    pass->min_ops = st_->o.floor_ops;
    pass->min_mem = st_->o.floor_mem;
  }
  const fs::path file = fs_path(share_folder) / fs_path(serve::kClaimTokenFile);
  const fs::path proof = fs_path(share_folder) / fs_path(serve::kClaimProofFile);
  // Whatever happens from here, neither file stays on the share; this is in
  // place before either is written, so a write that fails halfway goes too.
  struct remove_files {
    fs::path a, b;
    ~remove_files() {
      std::error_code e;
      fs::remove(a, e);
      fs::remove(b, e);
    }
  } gone{file, proof};
  const auto write = [](const fs::path& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out.flush());
  };
  // One deadline for the whole claim, started before anything touches the
  // share. The SMB writes themselves (the token, the proof) cannot be cut
  // short; a claim they make overrun comes back status::timeout, and the
  // caller claims once more, which the service answers again for its owner.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kClaimTotalMs);
  const std::string token = random_hex(32);
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  if (ec || !write(file, token)) return err(status::io);

  auto l = st_->connect_to(local);
  if (!l) return err(status::io);
  std::string why;
  auto answer = serve::claim(std::shared_ptr<link>(std::move(*l)), serve::claim_key(token), key, st_->o.device_name,
                             [&](std::string_view nonce) { return write(proof, nonce); }, deadline,
                             pass ? &*pass : nullptr, &why);
  {
    json::writer r;
    r.begin_object();
    if (!answer && !why.empty()) r.key("why").string(why);
    r.end_object();
    st_->refusal = r.take();
  }
  // Silence and a refusal are the same thing from here: it has an owner, or
  // this is not its own network, and the service never says which. A timeout
  // after the proof was written is not a refusal: it is passed on.
  if (!answer) {
    st_->forget_signer();  // nothing it made is known to be right
    return err(answer.error() == status::timeout ? status::timeout : status::permission_denied);
  }
  st_->claim_passphrase_set = pass && answer->passphrase_step;
  if (pass && !answer->passphrase_step) {
    // A NAS from before the passphrase: claimed, and nothing was set.
    st_->forget_signer();
  }
  service_info s;
  s.local = local;
  s.public_name = public_name;
  s.identity = answer->identity;
  s.access_key = answer->access_key;
  st_->info = std::move(s);
  if (!st_->save()) return err(status::io);
  return {};
}

expected desk::set_public_name(const std::string& public_name) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (!st_->info) return err(status::not_found);
  if (!public_name.empty() && !split_host_port(public_name)) return err(status::invalid_arg);
  st_->info->public_name = public_name;
  if (!st_->save()) return err(status::io);
  return {};
}

expected desk::set_local(const std::string& local) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (!st_->info) return err(status::not_found);
  if (!split_host_port(local)) return err(status::invalid_arg);
  st_->info->local = local;
  st_->admin.reset();
  if (!st_->save()) return err(status::io);
  return {};
}

result<pairing> desk::start_pairing(int ttl_ms, std::int64_t for_s) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (ttl_ms <= 0 || ttl_ms > serve::kMaxPairTtlMs || for_s < 0 || for_s > serve::grants::kMaxSeconds) {
    return err(status::invalid_arg);
  }
  const std::string code = random_hex(serve::kPairCodeChars / 2);
  json::writer w;
  // A guest code is its own verb: a NAS from before grants answers
  // `unsupported`, never pairs the phone with no limit.
  w.begin_object().key("do").string(for_s > 0 ? "guest_code" : "pair_code").key("code").string(code).key("ttl_ms").integer(ttl_ms);
  if (for_s > 0) w.key("for_s").integer(for_s);
  w.end_object();
  auto asked = st_->ask(w.str());
  if (!asked && for_s > 0 && st_->last_end == serve::end_code::unsupported) return err(status::unsupported_format);
  MV_TRY(const std::string done, std::move(asked));
  (void)done;
  st_->waiting = true;
  pairing p;
  p.service = *st_->info;
  p.code = code;
  p.expires_unix = static_cast<std::int64_t>(std::time(nullptr)) + (ttl_ms + 999) / 1000;
  return p;
}

expected desk::stop_pairing() {
  std::lock_guard lock(st_->m);
  st_->load();
  MV_TRY(const std::string done, st_->ask("{\"do\":\"pair_cancel\"}"));
  (void)done;
  st_->waiting = false;
  return {};
}

bool desk::pairing_waiting() {
  std::lock_guard lock(st_->m);
  return st_->waiting;
}

std::size_t desk::revokes_pending() {
  std::lock_guard lock(st_->m);
  return st_->pending().size();
}

void desk::release() {
  std::lock_guard lock(st_->m);
  st_->admin.reset();
  st_->expire_signer();
}

std::vector<device_row> desk::devices() {
  std::lock_guard lock(st_->m);
  return st_->local_devices();
}

result<std::vector<device_row>> desk::refresh_devices() {
  std::lock_guard lock(st_->m);
  st_->load();
  auto waiting = st_->pending();
  for (auto it = waiting.begin(); it != waiting.end();) {
    if (auto told = st_->tell_revoke(*it); !told) return err(told.error());
    it = waiting.erase(it);
    st_->save_pending(waiting);
  }
  MV_TRY(const std::string text, st_->ask("{\"do\":\"devices\"}"));
  const auto doc = json::parse(text, 6);
  const json::value* list = doc ? doc->find("devices") : nullptr;
  if (!list || list->k != json::kind::array) return err(status::corrupt);
  st_->waiting = doc->boolean("code_waiting").value_or(false);
  std::vector<device_row> rows;
  for (const auto& v : list->a) {
    const std::string* name = v.str("name");
    const std::string* key = v.str("key");
    const auto parsed = key ? parse_key_hex(*key) : std::nullopt;
    if (!name || !parsed) return err(status::corrupt);
    device_row r;
    r.name = *name;
    r.key = *parsed;
    rows.push_back(std::move(r));
  }
  // The timed and scoped phones (§5), from a NAS that has them: `devices`
  // never lists them, so a desktop from before this never serves one nearby.
  std::map<key32, std::vector<std::string>> scopes;
  if (auto g = st_->ask("{\"do\":\"grants\"}")) {
    const auto gdoc = json::parse(*g, 6);
    const json::value* glist = gdoc ? gdoc->find("grants") : nullptr;
    if (!glist || glist->k != json::kind::array) return err(status::corrupt);
    for (const auto& v : glist->a) {
      const std::string* name = v.str("name");
      const std::string* key = v.str("key");
      const auto parsed = key ? parse_key_hex(*key) : std::nullopt;
      const auto expires = v.integer("expires");
      if (!name || !parsed || !expires) return err(status::corrupt);
      device_row r;
      r.name = *name;
      r.key = *parsed;
      r.limited = true;
      r.expires = *expires;
      r.expired = v.boolean("expired").value_or(false);
      r.guest = v.boolean("guest").value_or(false);
      if (const json::value* roots = v.find("roots"); roots && roots->k == json::kind::array) {
        r.scoped = true;
        std::vector<std::string> ids;
        for (const auto& id : roots->a) {
          // Only a folder id this side can keep: 16 lowercase hex digits.
          const bool ok = id.k == json::kind::string && id.s.size() == 16 &&
                          std::all_of(id.s.begin(), id.s.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
          if (ok) ids.push_back(id.s);
        }
        scopes[r.key] = std::move(ids);
      }
      // A key on both lists (a move half done on the NAS) is treated by its grant.
      std::erase_if(rows, [&](const device_row& x) { return x.key == r.key; });
      rows.push_back(std::move(r));
    }
  } else if (g.error() != status::permission_denied) {
    return err(g.error());  // permission_denied: `unsupported` from a NAS from before grants
  }
  st_->refusal = "{}";
  if (!st_->save_local(rows, scopes)) return err(status::io);
  return rows;
}

expected desk::add_device(const std::string& name, const key32& key) {
  std::lock_guard lock(st_->m);
  st_->load();
  json::writer w;
  w.begin_object().key("do").string("add_device").key("name").string(name).key("key").string(key_hex(key)).end_object();
  MV_TRY(const std::string done, st_->ask(w.str()));
  (void)done;
  auto rows = st_->local_devices();
  if (std::none_of(rows.begin(), rows.end(), [&](const device_row& r) { return r.key == key; })) rows.push_back({name, key});
  if (!st_->save_local(rows)) return err(status::io);
  return {};
}

expected desk::revoke(const key32& key) {
  std::lock_guard lock(st_->m);
  st_->load();
  auto rows = st_->local_devices();
  std::erase_if(rows, [&](const device_row& r) { return r.key == key; });
  if (!st_->save_local(rows)) return err(status::io);
  auto waiting = st_->pending();
  if (std::find(waiting.begin(), waiting.end(), key) == waiting.end()) waiting.push_back(key);
  st_->save_pending(waiting);
  if (auto told = st_->tell_revoke(key); !told) return err(status::io);
  std::erase(waiting, key);
  st_->save_pending(waiting);
  return {};
}

result<std::vector<root_row>> desk::roots() {
  std::lock_guard lock(st_->m);
  st_->load();
  MV_TRY(const std::string text, st_->ask("{\"do\":\"roots\"}"));
  const auto doc = json::parse(text, 6);
  const json::value* list = doc ? doc->find("roots") : nullptr;
  if (!list || list->k != json::kind::array) return err(status::corrupt);
  std::vector<root_row> out;
  for (const auto& v : list->a) {
    const std::string* id = v.str("id");
    const std::string* label = v.str("label");
    const std::string* path = v.str("path");
    if (!id || !label || !path) return err(status::corrupt);
    out.push_back({*id, *label, *path});
  }
  return out;
}

result<std::string> desk::add_root(const std::string& label, const std::string& path) {
  std::lock_guard lock(st_->m);
  st_->load();
  json::writer w;
  w.begin_object().key("do").string("add_root").key("label").string(label).key("path").string(path).end_object();
  MV_TRY(const std::string text, st_->ask(w.str()));
  const auto doc = json::parse(text, 4);
  const std::string* id = doc ? doc->str("id") : nullptr;
  if (!id) return err(status::corrupt);
  return *id;
}

expected desk::remove_root(const std::string& id) {
  std::lock_guard lock(st_->m);
  st_->load();
  json::writer w;
  w.begin_object().key("do").string("remove_root").key("id").string(id).end_object();
  MV_TRY(const std::string done, st_->ask(w.str()));
  (void)done;
  return {};
}

expected desk::unlock(const std::string& passphrase) {
  std::lock_guard lock(st_->m);
  st_->load();
  st_->forget_signer();
  MV_TRY(serve::client * c, st_->session());
  std::string why;
  auto r = c->unlock(st_->deriving(passphrase), &why, st_->o.floor_ops, st_->o.floor_mem);
  st_->refusal = r ? std::string("{}") : (why.empty() ? std::string("{}") : why);
  if (!r) {
    st_->forget_signer();
    return err(r.error());
  }
  return {};
}

expected desk::lock() {
  std::lock_guard lock(st_->m);
  st_->load();
  st_->forget_signer();
  if (!st_->admin || st_->admin->closed()) return {};  // nothing unlocked over there either
  (void)st_->admin->admin("{\"do\":\"lock\"}");
  return {};
}

expected desk::set_passphrase(const std::string& next) {
  std::lock_guard lock(st_->m);
  st_->load();
  serve::owner::signer made;
  const auto v = serve::owner::make(next, static_cast<std::int64_t>(std::time(nullptr)), st_->o.pass_ops,
                                    st_->o.pass_mem, &made);
  if (!v) {
    st_->refusal = serve::owner::acceptable(next) ? "{}" : "{\"why\":\"passphrase-short\"}";
    return err(status::invalid_arg);
  }
  json::writer w;
  std::string salt(serve::owner::kSaltBytes * 2, '0');
  sodium_bin2hex(salt.data(), salt.size() + 1, v->salt.data(), v->salt.size());
  w.begin_object().key("do").string("set_passphrase").key("set").begin_object();
  w.key("salt").string(salt).key("ops").integer(static_cast<std::int64_t>(v->ops));
  w.key("mem").integer(static_cast<std::int64_t>(v->mem)).key("pk").string(key_hex(v->pk));
  w.end_object().end_object();
  MV_TRY(const std::string done, st_->ask(w.str()));
  (void)done;
  // The NAS ended this session's unlock; the new key unlocks it at the next change.
  st_->keep_signer(made, v->salt, v->ops, v->mem);
  return {};
}

result<owner_state> desk::owner_status() {
  std::lock_guard lock(st_->m);
  st_->load();
  st_->expire_signer();
  MV_TRY(serve::client * c, st_->session());
  owner_state out;
  out.features = c->caps().features;
  out.passphrase = c->caps().passphrase;
  if (out.features.empty()) return out;  // a NAS from before this: nothing more to ask
  MV_TRY(const std::string text, st_->ask("{\"do\":\"owner_status\"}"));
  const auto doc = json::parse(text, 2);
  if (!doc) return err(status::corrupt);
  if (const std::string* p = doc->str("passphrase")) out.passphrase = *p;
  out.unlocked_s = doc->integer("unlocked_s").value_or(0);
  out.locked_until = doc->integer("locked_until").value_or(0);
  if (const std::string* r = doc->str("relay")) out.relay = *r;
  // This desktop's own kept key decides whether it can act without asking.
  if (!st_->signer || std::chrono::steady_clock::now() >= st_->signer_until) {
    out.unlocked_s = 0;
  } else {
    out.unlocked_s = std::chrono::duration_cast<std::chrono::seconds>(st_->signer_until - std::chrono::steady_clock::now()).count();
  }
  return out;
}

expected desk::set_relay(const std::string& address, const std::string& key) {
  std::lock_guard lock(st_->m);
  st_->load();
  json::writer w;
  w.begin_object().key("do").string("set_relay").key("address").string(address).key("key").string(key).end_object();
  MV_TRY(const std::string text, st_->ask(w.str()));
  (void)text;
  return {};
}

expected desk::grant(const key32& key, std::int64_t for_s) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (for_s < 0 || for_s > serve::grants::kMaxSeconds) return err(status::invalid_arg);
  json::writer w;
  w.begin_object().key("do").string("grant").key("key").string(key_hex(key));
  if (for_s > 0) {
    w.key("for_s").integer(for_s);
  } else {
    w.key("forever").boolean(true);
  }
  w.end_object();
  MV_TRY(const std::string done, st_->ask(w.str()));
  (void)done;
  return {};
}

std::string desk::last_refusal() {
  std::lock_guard lock(st_->m);
  return st_->refusal;
}

void desk::clear_refusal() {
  std::lock_guard lock(st_->m);
  st_->refusal = "{}";
  st_->last_end = serve::end_code::ok;
}

bool desk::claim_took_passphrase() {
  std::lock_guard lock(st_->m);
  return st_->claim_passphrase_set;
}

expected desk::start_nearby(const std::string& folder, const std::string& library_id) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (!st_->info) return err(status::not_found);
  const auto devices = st_->local_devices();
  std::lock_guard near(st_->nearby_m);
  if (st_->nearby && st_->nearby_folder == folder && st_->nearby_library == library_id) return {};
  st_->advert.reset();
  st_->nearby.reset();
  auto store = std::make_unique<memory_store>();
  key32 identity;
  randombytes_buf(identity.data(), identity.size());  // nothing pins it: trust is the catalogue's signature
  (void)store->put(known::kIdentityName, identity);
  sodium_memzero(identity.data(), identity.size());
  (void)store->put(serve::kAccessKeyName, st_->info->access_key);
  std::vector<known::device> list;
  for (const auto& r : devices) {
    if (r.limited) continue;  // in the grants below
    known::device d;
    d.name = r.name;
    d.key = r.key;
    list.push_back(std::move(d));
  }
  (void)store->put(known::kDevicesName, known::encode(list));
  {
    // The timed phones nearby too, so the Mac ends a guest when the NAS would.
    const std::string text = serve::grants::encode(st_->local_grants());
    (void)store->put(serve::grants::kGrantsName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  }
  serve::server::options o;
  o.cfg.port = 0;
  o.cfg.name = "MediaViewer";
  o.cfg.roots = {{kNearbyRoot, serve::kCatalogueLabel, folder}};
  o.store = store.get();
  o.loopback_only = st_->o.nearby_loopback;
  o.lan_only = true;
  o.own_network = st_->o.own_network;
  o.claimable = false;
  o.max_sessions = 4;
  auto started = serve::server::start(o);
  if (!started) return err(started.error());
  st_->nearby_store = std::move(store);
  st_->nearby = std::move(*started);
  st_->nearby_folder = folder;
  st_->nearby_library = library_id;
  st_->start_advert(st_->nearby->port());
  return {};
}

void desk::stop_nearby() {
  std::lock_guard near(st_->nearby_m);
  st_->advert.reset();
  st_->nearby.reset();
  st_->nearby_store.reset();
  st_->nearby_folder.clear();
  st_->nearby_library.clear();
}

std::uint16_t desk::nearby_port() const {
  std::lock_guard near(st_->nearby_m);
  return st_->nearby ? st_->nearby->port() : 0;
}

std::optional<serve::counters> desk::nearby_stats() const {
  std::lock_guard near(st_->nearby_m);
  if (!st_->nearby) return std::nullopt;
  return st_->nearby->stats();
}

std::string desk::library_tag(std::string_view library_id) {
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(library_id.data()), library_id.size());
  char out[17];
  sodium_bin2hex(out, sizeof out, h, 8);
  return std::string(out, 16);
}

// ---- the route ------------------------------------------------------------------------------

namespace {

std::string devices_json(const std::vector<device_row>& rows, bool reachable, bool waiting) {
  json::writer w;
  w.begin_object().key("reachable").boolean(reachable).key("code_waiting").boolean(waiting).key("devices").begin_array();
  for (const auto& r : rows) {
    w.begin_object().key("name").string(r.name).key("key").string(key_hex(r.key));
    if (r.limited) {
      w.key("limited").boolean(true).key("expires").integer(r.expires).key("expired").boolean(r.expired);
      w.key("guest").boolean(r.guest).key("scoped").boolean(r.scoped);
    }
    w.end_object();
  }
  w.end_array().end_object();
  return w.take();
}

}  // namespace

result<std::string> route_request(desk& d, std::string_view caller, std::string_view method, std::string_view request_json) {
  if (caller != "phone") return err(status::permission_denied);
  if (method != "last_refusal") d.clear_refusal();
  const auto doc = json::parse(request_json.empty() ? std::string_view("{}") : request_json, 4);
  if (!doc) return err(status::invalid_arg);
  const auto text = [&](const char* k) -> std::string {
    const std::string* v = doc->str(k);
    return v ? *v : std::string();
  };
  if (method == "status") {
    const auto s = d.service();
    json::writer w;
    w.begin_object();
    w.key("claimed").boolean(s.has_value());
    w.key("local").string(s ? s->local : std::string());
    w.key("public").string(s ? s->public_name : std::string());
    w.key("machine").string(net::machine_name());
    w.key("pending_revokes").integer(static_cast<std::int64_t>(d.revokes_pending()));
    w.end_object();
    return w.take();
  }
  if (method == "claim") {
    MV_TRY_VOID(d.claim(text("share"), text("local"), text("public"), text("passphrase"),
                        doc->boolean("new_passphrase").value_or(false)));
    if (!text("passphrase").empty()) {
      json::writer w;
      w.begin_object().key("passphrase_set").boolean(d.claim_took_passphrase()).end_object();
      return w.take();
    }
    return std::string("{}");
  }
  if (method == "set_public") {
    MV_TRY_VOID(d.set_public_name(text("public")));
    return std::string("{}");
  }
  if (method == "set_local") {
    MV_TRY_VOID(d.set_local(text("local")));
    return std::string("{}");
  }
  if (method == "pair") {
    MV_TRY(const pairing p, d.start_pairing(static_cast<int>(doc->integer("ttl_ms").value_or(kDefaultPairTtlMs)),
                                            doc->integer("for_s").value_or(0)));
    json::writer w;
    w.begin_object();
    w.key("local").string(p.service.local);
    w.key("public").string(p.service.public_name);
    w.key("identity").string(key_hex(p.service.identity));
    w.key("access_key").string(key_hex(p.service.access_key));
    w.key("code").string(p.code);
    w.key("expires").integer(p.expires_unix);
    // What both screens show (pair_check.h): only when the caller says whose library this is.
    if (const auto publisher = parse_key_hex(text("publisher_key")); publisher && !text("library_id").empty()) {
      w.key("check").string(pairing_check(text("library_id"), *publisher, p.service.identity, p.service.access_key));
    }
    w.end_object();
    return w.take();
  }
  if (method == "devices") {
    auto rows = d.refresh_devices();
    if (rows) return devices_json(*rows, true, d.pairing_waiting());
    if (rows.error() != status::io && rows.error() != status::timeout) return err(rows.error());
    return devices_json(d.devices(), false, d.pairing_waiting());
  }
  if (method == "revoke") {
    const auto key = parse_key_hex(text("key"));
    if (!key) return err(status::invalid_arg);
    const auto r = d.revoke(*key);
    if (!r && r.error() != status::io) return err(r.error());
    json::writer w;
    w.begin_object().key("nas_told").boolean(r.has_value()).end_object();
    return w.take();
  }
  if (method == "roots") {
    MV_TRY(const auto rows, d.roots());
    json::writer w;
    w.begin_object().key("roots").begin_array();
    for (const auto& r : rows) w.begin_object().key("id").string(r.id).key("label").string(r.label).key("path").string(r.path).end_object();
    w.end_array().end_object();
    return w.take();
  }
  if (method == "add_root") {
    MV_TRY(const std::string id, d.add_root(text("label"), text("path")));
    json::writer w;
    w.begin_object().key("id").string(id).end_object();
    return w.take();
  }
  if (method == "remove_root") {
    MV_TRY_VOID(d.remove_root(text("id")));
    return std::string("{}");
  }
  if (method == "check_address") {
    const auto where = split_host_port(text("address"));
    if (!where) return err(status::invalid_arg);
    const auto found = net::resolve(where->first);
    json::writer w;
    w.begin_object().key("warning").string(warning_name(classify_addresses(found))).end_object();
    return w.take();
  }
  if (method == "nearby") {
    MV_TRY_VOID(d.start_nearby(text("folder"), text("library_id")));
    json::writer w;
    w.begin_object().key("port").integer(d.nearby_port()).end_object();
    return w.take();
  }
  // ---- the owner's passphrase and timed phones (docs/plans/nas-owner-and-guests.md) ----
  if (method == "unlock") {
    MV_TRY_VOID(d.unlock(text("passphrase")));
    return std::string("{}");
  }
  if (method == "lock") {
    MV_TRY_VOID(d.lock());
    return std::string("{}");
  }
  if (method == "set_passphrase") {
    MV_TRY_VOID(d.set_passphrase(text("passphrase")));
    return std::string("{}");
  }
  if (method == "owner_status") {
    MV_TRY(const owner_state s, d.owner_status());
    json::writer w;
    w.begin_object().key("passphrase").string(s.passphrase).key("features").begin_array();
    for (const auto& f : s.features) w.string(f);
    w.end_array().key("unlocked_s").integer(s.unlocked_s).key("locked_until").integer(s.locked_until);
    w.key("relay").string(s.relay).end_object();
    return w.take();
  }
  if (method == "set_relay") {
    MV_TRY_VOID(d.set_relay(text("address"), text("key")));
    return std::string("{}");
  }
  if (method == "grant") {
    const auto key = parse_key_hex(text("key"));
    if (!key) return err(status::invalid_arg);
    MV_TRY_VOID(d.grant(*key, doc->integer("for_s").value_or(0)));
    return std::string("{}");
  }
  if (method == "last_refusal") return d.last_refusal();
  if (method == "pair_stop") {
    MV_TRY_VOID(d.stop_pairing());
    return std::string("{}");
  }
  if (method == "release") {
    d.release();
    return std::string("{}");
  }
  if (method == "nearby_stop") {
    d.stop_nearby();
    return std::string("{}");
  }
  return err(status::not_found);
}

}  // namespace mv::driftwire::desk
