// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/desk.h"

#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

#include "addons/driftwire/advert.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/pair_check.h"
#include "addons/driftwire/serve_client.h"
#include "addons/driftwire/transfer.h"
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
  // One request; a held session that died is replaced once.
  [[nodiscard]] result<std::string> ask(const std::string& request) {
    for (int attempt = 0;; ++attempt) {
      MV_TRY(serve::client * c, session());
      auto r = c->admin(request);
      if (r || attempt == 1 || !admin->closed()) return r;
      admin.reset();
    }
  }
  [[nodiscard]] std::vector<device_row> local_devices() {
    std::vector<device_row> out;
    for (const auto& d : known::load_named(*o.store, kDevicesName)) out.push_back({d.name, d.key});
    return out;
  }
  [[nodiscard]] bool save_local(const std::vector<device_row>& rows) {
    std::vector<known::device> list;
    for (const auto& r : rows) {
      known::device d;
      d.name = r.name;
      d.key = r.key;
      list.push_back(std::move(d));
    }
    const auto bytes = known::encode(list);
    const bool saved = o.store->put(kDevicesName, bytes);
    std::lock_guard lock(nearby_m);
    if (nearby_store) {
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
  (void)sodium_init();
  st_->o = std::move(o);
}

desk::~desk() { stop_nearby(); }

std::optional<service_info> desk::service() {
  std::lock_guard lock(st_->m);
  st_->load();
  return st_->info;
}

expected desk::claim(const std::string& share_folder, const std::string& local, const std::string& public_name) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (!split_host_port(local)) return err(status::invalid_arg);
  if (!public_name.empty() && !split_host_port(public_name)) return err(status::invalid_arg);
  MV_TRY(const key32 key, st_->device_key());

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
  const std::string token = random_hex(32);
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  if (ec || !write(file, token)) return err(status::io);

  auto l = st_->connect_to(local);
  if (!l) return err(status::io);
  auto answer = serve::claim(std::shared_ptr<link>(std::move(*l)), serve::claim_key(token), key, st_->o.device_name,
                             [&](std::string_view nonce) { return write(proof, nonce); });
  // Silence and a refusal are the same thing from here: it has an owner, or
  // this is not its own network, and the service never says which.
  if (!answer) return err(status::permission_denied);
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

result<pairing> desk::start_pairing(int ttl_ms) {
  std::lock_guard lock(st_->m);
  st_->load();
  if (ttl_ms <= 0 || ttl_ms > serve::kMaxPairTtlMs) return err(status::invalid_arg);
  const std::string code = random_hex(serve::kPairCodeChars / 2);
  json::writer w;
  w.begin_object().key("do").string("pair_code").key("code").string(code).key("ttl_ms").integer(ttl_ms).end_object();
  MV_TRY(const std::string done, st_->ask(w.str()));
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
    rows.push_back({*name, *parsed});
  }
  if (!st_->save_local(rows)) return err(status::io);
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
    known::device d;
    d.name = r.name;
    d.key = r.key;
    list.push_back(std::move(d));
  }
  (void)store->put(known::kDevicesName, known::encode(list));
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
  for (const auto& r : rows) w.begin_object().key("name").string(r.name).key("key").string(key_hex(r.key)).end_object();
  w.end_array().end_object();
  return w.take();
}

}  // namespace

result<std::string> route_request(desk& d, std::string_view caller, std::string_view method, std::string_view request_json) {
  if (caller != "ai") return err(status::permission_denied);
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
    MV_TRY_VOID(d.claim(text("share"), text("local"), text("public")));
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
    MV_TRY(const pairing p, d.start_pairing(static_cast<int>(doc->integer("ttl_ms").value_or(kDefaultPairTtlMs))));
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
