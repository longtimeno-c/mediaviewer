// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/library.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/json.h"
#include "io/file.h"
#include "io/media_extensions.h"
#include "io/replace.h"
#include "io/verified_copy.h"
#include "mobile/link_payload.h"
#include "wire/serve.h"
#include "wire/serve_wire.h"

namespace mv::remote {

namespace fs = std::filesystem;
namespace serve = driftwire::serve;
using key32 = std::array<std::uint8_t, 32>;

namespace {

constexpr const char* kServersFile = "servers.json";
constexpr const char* kSecretPrefix = "remote_";
constexpr int kFetchTries = 4;  // a link that drops mid-fetch: go on from where it got to

fs::path fs_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string hex_of(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 15];
  }
  return out;
}

std::optional<key32> key_of_hex(std::string_view hex) {
  if (hex.size() != 64) return std::nullopt;
  key32 k{};
  std::size_t bin_len = 0;
  if (sodium_hex2bin(k.data(), k.size(), hex.data(), hex.size(), nullptr, &bin_len, nullptr) != 0 || bin_len != 32) {
    return std::nullopt;
  }
  return k;
}

// The server's id: 64 bits of a hash of its pinned identity, so pairing the
// same service again finds the same row.
std::string id_of(const key32& identity) {
  unsigned char h[8];
  crypto_generichash(h, sizeof h, identity.data(), identity.size(), nullptr, 0);
  return hex_of(std::span<const std::uint8_t>(h, sizeof h));
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') s.remove_prefix(1);
  while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') s.remove_suffix(1);
  return s;
}

std::string utf8_of(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// io::replace_atomic replaces a file that is there; a first write is write_new_atomic.
expected write_atomic(const std::string& path, std::span<const std::uint8_t> bytes) {
  if (io::file_exists(path)) return io::replace_atomic(path, bytes);
  auto made = io::write_new_atomic(path, bytes);
  if (!made && io::file_exists(path)) return io::replace_atomic(path, bytes);  // another writer won the race
  return made;
}

bool is_hidden(std::string_view name) { return name.empty() || name.front() == '.'; }

std::string join_rel(std::string_view rel, std::string_view name) {
  if (rel.empty()) return std::string(name);
  std::string out(rel);
  out += '/';
  out += name;
  return out;
}

bool less_name(const std::string& a, const std::string& b) {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const auto ca = static_cast<unsigned char>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] - 'A' + 'a' : a[i]);
    const auto cb = static_cast<unsigned char>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] - 'A' + 'a' : b[i]);
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

io::media_kind kind_of(std::string_view name, bool& known) {
  const io::media_extension* e = io::find_media_extension(io::extension_of(name));
  known = e != nullptr;
  return e ? e->kind : io::media_kind::still;
}

std::mutex g_shared_m;
std::unique_ptr<library> g_shared;
std::atomic<library*> g_shared_ptr{nullptr};

}  // namespace

const char* reach_name(reach r) noexcept {
  switch (r) {
    case reach::unknown: return "unknown";
    case reach::online: return "online";
    case reach::unreachable: return "unreachable";
    case reach::access_ended: return "access_ended";
    case reach::wrong_service: return "wrong_service";
    case reach::too_old: return "too_old";
  }
  return "unknown";
}

std::string open_problem_words(status st, reach r, std::string_view server) {
  const std::string name(server.empty() ? std::string_view("the server") : server);
  if (r == reach::access_ended || st == status::permission_denied) {
    return "This computer's access to " + name + " has ended. Connect again with a new code from " + name + ".";
  }
  if (r == reach::wrong_service) return "Something answered as " + name + " but is not it. Nothing was read from it.";
  if (r == reach::too_old || st == status::unsupported_format) {
    return name + " runs an older driftwire than this app needs. Update it there.";
  }
  if (st == status::not_found) return "This folder's drive isn't connected to " + name + " right now.";
  if (r == reach::unreachable || st == status::io || st == status::timeout) {
    return "Can't reach " + name + " right now. Its folders show again when it answers.";
  }
  return "This folder on " + name + " could not be opened.";
}

std::string pair_problem_words(status st, std::string_view server) {
  const std::string name(server.empty() ? std::string_view("the server") : server);
  switch (st) {
    case status::invalid_arg:
      return "That is not a pairing code. Copy the whole code or link from the other computer.";
    case status::timeout:
      return "That code has expired. Make a new one on " + name + ".";
    case status::permission_denied:
      return name + " did not take the code: it was used already, has expired, or is another server's. Make a new one there.";
    case status::unsupported_format:
      return name + " runs an older driftwire than this app needs. Update it there.";
    case status::internal:
      return "This computer could not keep the keys to open " + name + " in its keychain. If " + name +
             " lists this computer, remove it there before connecting again.";
    case status::cancelled:
      return {};
    default:
      return "Can't reach " + name + ". Check it is on, and on a network this computer can reach.";
  }
}

std::string stale_words(std::string_view server) {
  const std::string name(server.empty() ? std::string_view("the server") : server);
  return "Can't reach " + name + " right now. This folder is shown as it was; what was opened here before still opens.";
}

struct library::server_rt {
  std::string id, name, local, public_name, relay, library_id;
  key32 identity{};
  key32 device_key{};
  key32 access_key{};
  std::int64_t paired_unix = 0;
  bool keys_missing = false;
  std::unique_ptr<mobile::link_session> link;
  std::atomic<reach> state{reach::unknown};
  std::atomic<std::int64_t> failed_at_ms{0};
  std::atomic<int> role{0};  // role_code: 0 not known this run
  // The service has mkdir / move / delete (serve::client::changes_files), and whether the
  // writable roots were read since the last connect that changed the role.
  std::atomic<bool> changes{false};
  std::atomic<bool> rights_known{false};

  ~server_rt() {
    sodium_memzero(device_key.data(), device_key.size());
    sodium_memzero(access_key.data(), access_key.size());
  }
};

struct library::fetch_state {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  result<std::string> outcome = err(status::internal);
};

// A clip's bytes through range reads, for its poster: FFmpeg's AVIO asks for
// 64 KiB at a time, mostly in order, so each miss reads a 256 KiB block.
class link_reader final : public range_reader {
 public:
  link_reader(mobile::link_session& link, std::string root, std::string rel, std::uint64_t size, const job_context* ctx,
              std::atomic<std::uint64_t>& counted)
      : link_(link), root_(std::move(root)), rel_(std::move(rel)), size_(size), ctx_(ctx), counted_(counted) {}

  result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> out) override {
    if (offset >= size_ || out.empty()) return std::size_t{0};
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size_ - offset));
    if (!(offset >= block_at_ && offset + want <= block_at_ + block_.size())) {
      const std::uint64_t len = std::min<std::uint64_t>(std::max<std::uint64_t>(want, kBlock), size_ - offset);
      block_.clear();
      block_.reserve(static_cast<std::size_t>(len));
      block_at_ = offset;
      auto done = link_.read_range(
          root_, rel_, offset, len,
          [&](std::uint64_t, std::span<const std::uint8_t> bytes) {
            if (ctx_ && ctx_->cancelled()) return false;
            block_.insert(block_.end(), bytes.begin(), bytes.end());
            return true;
          },
          nullptr, false);
      counted_.fetch_add(block_.size(), std::memory_order_relaxed);
      if (!done) {
        block_.clear();
        return err(ctx_ && ctx_->cancelled() ? status::cancelled : done.error());
      }
    }
    const std::size_t from = static_cast<std::size_t>(offset - block_at_);
    const std::size_t n = std::min(want, block_.size() - from);
    std::memcpy(out.data(), block_.data() + from, n);
    return n;
  }
  std::uint64_t size() const noexcept override { return size_; }

 private:
  static constexpr std::uint64_t kBlock = 256 * 1024;
  mobile::link_session& link_;
  std::string root_, rel_;
  std::uint64_t size_;
  const job_context* ctx_;
  std::atomic<std::uint64_t>& counted_;
  std::vector<std::uint8_t> block_;
  std::uint64_t block_at_ = 0;
};

library::library(config c) : cfg_(std::move(c)), cache_(cfg_.cache_dir, cfg_.cache_cap) {
  (void)sodium_init();
  // Made here, once: pairing, forgetting and loading run on different threads.
  if (cfg_.store) return;
  if (!cfg_.dev_keys_dir.empty()) {
    // Never the OS store as a fallback: a dev run must not touch the installed app's keys.
    if (driftwire::secrets::owner_only_folder(cfg_.dev_keys_dir))
      own_store_ = driftwire::secrets::file_store(cfg_.dev_keys_dir);
    return;
  }
  own_store_ = driftwire::secrets::os_store(driftwire::secrets::kAppService);
}

library::~library() {
  std::lock_guard lock(m_);
  for (auto& [id, s] : servers_) {
    (void)id;
    if (s->link) s->link->shut_down();
  }
}

void library::install(std::unique_ptr<library> lib) {
  std::lock_guard lock(g_shared_m);
  g_shared_ptr.store(lib.get(), std::memory_order_release);
  g_shared = std::move(lib);
}

library* library::shared() noexcept { return g_shared_ptr.load(std::memory_order_acquire); }

driftwire::secrets::store* library::store() {
  return cfg_.store ? cfg_.store : own_store_.get();
}

namespace {

std::unique_ptr<mobile::link_session> make_link(const config& cfg, const std::string& local,
                                                const std::string& public_name, const std::string& relay,
                                                const key32& identity, const key32& access, const key32& device) {
  mobile::service_config c;
  c.local = local;
  c.public_name = public_name;
  c.relay = relay;
  c.identity = identity;
  c.access_key = access;
  c.device_key = device;
  c.device_name = cfg.device_name;
  c.upgrade = true;  // a desktop is never on mobile data: the direct path is always wanted
  c.loopback_only = cfg.loopback_only;
  return std::make_unique<mobile::link_session>(std::move(c));
}

}  // namespace

std::string library::servers_file() const { return utf8_of(fs_path(cfg_.data_dir) / kServersFile); }

expected library::load() {
  if (!io::file_exists(servers_file())) return {};  // nothing paired yet
  auto bytes = io::read_all(servers_file());
  if (!bytes) return err(bytes.error());
  auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!doc) return err(status::corrupt);
  const json::value* list = doc->find("servers");
  if (!list || list->k != json::kind::array) return err(status::corrupt);
  std::map<std::string, std::shared_ptr<server_rt>> next;
  for (const json::value& v : list->a) {
    const std::string* id = v.str("id");
    const std::string* identity = v.str("identity");
    if (!id || !identity || !is_hex16(*id)) continue;
    auto ident = key_of_hex(*identity);
    if (!ident || id_of(*ident) != *id) continue;
    auto s = std::make_shared<server_rt>();
    s->id = *id;
    s->identity = *ident;
    if (const auto* x = v.str("name")) s->name = *x;
    if (const auto* x = v.str("local")) s->local = *x;
    if (const auto* x = v.str("public")) s->public_name = *x;
    if (const auto* x = v.str("relay")) s->relay = *x;
    if (const auto* x = v.str("library_id")) s->library_id = *x;
    s->paired_unix = v.integer("paired").value_or(0);
    // No OS store: its keys cannot be read, so it is listed to pair again.
    std::optional<std::vector<std::uint8_t>> secret;
    if (auto* keys = store()) secret = keys->get(kSecretPrefix + s->id);
    if (secret && secret->size() == 64) {
      std::memcpy(s->device_key.data(), secret->data(), 32);
      std::memcpy(s->access_key.data(), secret->data() + 32, 32);
      s->link = make_link(cfg_, s->local, s->public_name, s->relay, s->identity, s->access_key, s->device_key);
    } else {
      s->keys_missing = true;  // the OS store lost them: pair again
      s->state = reach::access_ended;
    }
    if (secret) sodium_memzero(secret->data(), secret->size());
    next[s->id] = std::move(s);
  }
  {
    std::lock_guard lock(m_);
    servers_ = std::move(next);
  }
  gen_.fetch_add(1, std::memory_order_acq_rel);
  return {};
}

std::string library::servers_json_locked() const {
  json::writer w;
  w.begin_object().key("format").integer(1).key("servers").begin_array();
  for (const auto& [id, s] : servers_) {
    w.begin_object()
        .key("id").string(id)
        .key("name").string(s->name)
        .key("local").string(s->local)
        .key("public").string(s->public_name)
        .key("relay").string(s->relay)
        .key("identity").string(hex_of(s->identity))
        .key("library_id").string(s->library_id)
        .key("paired").integer(s->paired_unix)
        .end_object();
  }
  w.end_array().end_object();
  return w.str();
}

expected library::write_servers(const std::string& text) const {
  std::error_code ec;
  fs::create_directories(fs_path(cfg_.data_dir), ec);
  return write_atomic(servers_file(),
                            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

result<code_preview> library::preview(std::string_view code_text, std::int64_t now_unix) const {
  auto p = mobile::parse_pair_payload(trim(code_text));
  if (!p) return err(status::invalid_arg);
  code_preview out;
  out.name = p->library_name.empty() ? std::string("Server") : p->library_name;
  out.fingerprint = p->fingerprint;
  out.expires = p->expires;
  {
    std::lock_guard lock(m_);
    out.already_paired = servers_.count(id_of(p->identity)) != 0;
  }
  if (p->expires <= now_unix) return err(status::timeout);
  return out;
}

result<std::string> library::pair(std::string_view code_text, std::int64_t now_unix, const mobile::cancel_flag* cancel) {
  auto p = mobile::parse_pair_payload(trim(code_text));
  if (!p) return err(status::invalid_arg);
  if (p->expires <= now_unix) return err(status::timeout);
  // No OS store (a build signed without a keychain group, Linux without a
  // keyring): nothing could be kept, so the code is not spent.
  if (!store()) return err(status::internal);
  auto s = std::make_shared<server_rt>();
  s->id = id_of(p->identity);
  s->name = p->library_name.empty() ? std::string("Server") : p->library_name;
  s->local = p->local;
  s->public_name = p->public_name;
  s->relay = p->relay;
  s->library_id = p->library_id;
  s->identity = p->identity;
  s->access_key = p->access_key;
  s->paired_unix = now_unix;
  randombytes_buf(s->device_key.data(), s->device_key.size());  // this desktop's own key, for this server
  mobile::service_config c;
  c.local = s->local;
  c.public_name = s->public_name;
  c.relay = s->relay;
  c.identity = s->identity;
  c.access_key = s->access_key;
  c.device_key = s->device_key;
  c.device_name = cfg_.device_name;
  // Paired as a desktop, so the service starts it as "can view files" (owner call 1), never as a
  // phone that may back up (docs/plans/self-host-users-smb.md §9).
  c.device_kind = driftwire::serve::kDeviceDesktop;
  c.loopback_only = cfg_.loopback_only;
  mobile::route_report report;
  if (auto done = mobile::pair_device(c, p->code, report, cancel, cfg_.connect_timeout_ms); !done) {
    sodium_memzero(c.device_key.data(), c.device_key.size());
    sodium_memzero(c.access_key.data(), c.access_key.size());
    return err(done.error());
  }
  sodium_memzero(c.device_key.data(), c.device_key.size());
  sodium_memzero(c.access_key.data(), c.access_key.size());
  std::array<std::uint8_t, 64> secret{};
  std::memcpy(secret.data(), s->device_key.data(), 32);
  std::memcpy(secret.data() + 32, s->access_key.data(), 32);
  auto* keys = store();
  const bool kept = keys && keys->put(kSecretPrefix + s->id, secret);
  sodium_memzero(secret.data(), secret.size());
  // The service has taken the code and lists this desktop by now: not "can't
  // reach" (the Mac's keychain refused a release signed without the
  // keychain group, 2026-11-15).
  if (!kept) return err(status::internal);
  s->link = make_link(cfg_, s->local, s->public_name, s->relay, s->identity, s->access_key, s->device_key);
  s->state = reach::online;  // it just answered
  std::shared_ptr<server_rt> old;
  {
    std::lock_guard saving(save_m_);
    std::string text;
    {
      std::lock_guard lock(m_);
      if (auto it = servers_.find(s->id); it != servers_.end()) old = it->second;
      servers_[s->id] = s;
      text = servers_json_locked();
    }
    if (auto saved = write_servers(text); !saved) return err(saved.error());
  }
  if (old && old->link) old->link->shut_down();
  gen_.fetch_add(1, std::memory_order_acq_rel);
  return s->id;
}

expected library::forget(std::string_view server_id) {
  std::shared_ptr<server_rt> s;
  {
    std::lock_guard saving(save_m_);
    std::string text;
    {
      std::lock_guard lock(m_);
      auto it = servers_.find(std::string(server_id));
      if (it == servers_.end()) return err(status::not_found);
      s = it->second;
      servers_.erase(it);
      text = servers_json_locked();
    }
    if (auto saved = write_servers(text); !saved) return err(saved.error());
  }
  if (s->link) s->link->shut_down();
  if (auto* keys = store()) (void)keys->erase(kSecretPrefix + s->id);
  cache_.remove_server(s->id);
  {
    std::lock_guard lock(stamps_m_);
    const std::string prefix = server_key(s->id);
    std::erase_if(stamps_, [&](const auto& kv) { return kv.first.rfind(prefix, 0) == 0; });
    std::erase_if(labels_, [&](const auto& kv) { return kv.first.rfind(prefix, 0) == 0; });
  }
  gen_.fetch_add(1, std::memory_order_acq_rel);
  return {};
}

namespace {

// The caps' role word as a small number, so a server keeps it in an atomic.
int role_code(std::string_view w) noexcept {
  if (w == "view") return 1;
  if (w == "change") return 2;
  if (w == "admin") return 3;
  return 0;
}
const char* role_word(int c) noexcept {
  switch (c) {
    case 1: return "view";
    case 2: return "change";
    case 3: return "admin";
    default: return "";
  }
}

}  // namespace

std::vector<server_info> library::servers() const {
  std::vector<server_info> out;
  std::lock_guard lock(m_);
  out.reserve(servers_.size());
  for (const auto& [id, s] : servers_) out.push_back({id, s->name, s->state.load(), s->paired_unix, role_word(s->role.load())});
  std::sort(out.begin(), out.end(), [](const server_info& a, const server_info& b) { return less_name(a.name, b.name); });
  return out;
}

std::optional<server_info> library::server(std::string_view id) const {
  auto s = find(id);
  if (!s) return std::nullopt;
  return server_info{s->id, s->name, s->state.load(), s->paired_unix, role_word(s->role.load())};
}

std::shared_ptr<library::server_rt> library::find(std::string_view id) const {
  std::lock_guard lock(m_);
  auto it = servers_.find(std::string(id));
  return it == servers_.end() ? nullptr : it->second;
}

std::string library::display_name(std::string_view key) const {
  auto k = parse_key(key);
  if (!k) return {};
  if (k->root.empty()) {
    auto s = find(k->server);
    return s ? s->name : std::string{};
  }
  if (k->rel.empty()) {
    std::lock_guard lock(stamps_m_);
    auto it = labels_.find(key);
    return it == labels_.end() ? std::string{} : it->second;
  }
  return leaf_name(key);
}

void library::set_state(server_rt& s, reach r) {
  if (s.state.exchange(r) != r) gen_.fetch_add(1, std::memory_order_acq_rel);
}

void library::keep_addresses(server_rt& s) {
  // Where the service now says it is (a DuckDNS name or a relay added after pairing, a new LAN
  // address): kept in servers.json, so the next run dials it too. Never a change of server: the
  // link pinned its key. Saved only when something changed; a failed save tries at the next connect.
  std::string local, public_name, relay;
  s.link->addresses(local, public_name, relay);
  std::lock_guard saving(save_m_);
  std::string text;
  {
    std::lock_guard lock(m_);
    if (local == s.local && public_name == s.public_name && relay == s.relay) return;
    std::swap(s.local, local);
    std::swap(s.public_name, public_name);
    std::swap(s.relay, relay);
    text = servers_json_locked();
  }
  if (!write_servers(text)) {
    // Not kept: as it was, so the next connect sees the change again and retries the save.
    // save_m_ is still held, so no other save has changed these since.
    std::lock_guard lock(m_);
    std::swap(s.local, local);
    std::swap(s.public_name, public_name);
    std::swap(s.relay, relay);
  }
}

expected library::connect(server_rt& s, bool person, const mobile::cancel_flag* cancel) {
  if (!s.link) return err(status::permission_denied);  // its keys are gone: pair again
  if (!person && s.state.load() == reach::unreachable &&
      now_ms() - s.failed_at_ms.load() < cfg_.retry_after_ms) {
    return err(status::io);
  }
  mobile::route_report report;
  auto done = s.link->connect(report, cancel, cfg_.connect_timeout_ms);
  if (done) {
    std::string word;
    if (s.link->role(word) >= 0 && s.role.exchange(role_code(word)) != role_code(word)) {
      // The role is the caps' (sent once, at hello): a new one means what it may change is
      // read again now, so can_write never answers from the old role meanwhile.
      s.rights_known.store(false);
      if (auto roots = s.link->roots()) {
        note_rights(s, roots.value());
      } else {
        forget_rights(s);
      }
      gen_.fetch_add(1, std::memory_order_acq_rel);  // the rows say what it may do
    }
    keep_addresses(s);
    set_state(s, reach::online);
    return {};
  }
  switch (done.error()) {
    case status::cancelled:
      break;
    case status::permission_denied: {
      reach r = reach::access_ended;
      for (const auto& f : report.failures) {
        if (f.problem == mobile::route_problem::wrong_service) r = reach::wrong_service;
      }
      s.role.store(0);  // what it may do there is no longer known (set_state bumps)
      s.rights_known.store(false);
      s.changes.store(false);
      set_state(s, r);
      break;
    }
    case status::unsupported_format:
      set_state(s, reach::too_old);
      break;
    default:
      s.failed_at_ms.store(now_ms());
      set_state(s, reach::unreachable);
      break;
  }
  return err(done.error());
}

result<listing> library::list(std::string_view folder_key, bool person, const mobile::cancel_flag* cancel) {
  auto k = parse_key(folder_key);
  if (!k || !k->folder) return err(status::invalid_arg);
  auto s = find(k->server);
  if (!s) return err(status::not_found);
  auto live = list_live(*s, *k, folder_key, person, cancel);
  // The service cut the link as this asked (its limit changed: a role or folders the admin
  // changed): a person's listing connects again once rather than failing or going stale.
  if (!live && person && (live.error() == status::io || live.error() == status::timeout) &&
      !(cancel && cancel->stopped())) {
    live = list_live(*s, *k, folder_key, person, cancel);
  }
  if (live) {
    keep_listing(s->id, folder_key, live.value());
    return live;
  }
  // Nothing answered: the folder as it was, so what is cached still opens.
  if (live.error() == status::io || live.error() == status::timeout) {
    if (auto kept = kept_listing(s->id, folder_key)) return std::move(*kept);
  }
  return live;
}

std::string library::listing_file(std::string_view server, std::string_view folder_key) const {
  unsigned char h[16];
  crypto_generichash(h, sizeof h, reinterpret_cast<const unsigned char*>(folder_key.data()), folder_key.size(), nullptr, 0);
  return utf8_of(fs_path(cfg_.cache_dir) / fs_path(server) / "listings" /
                 fs_path(hex_of(std::span<const std::uint8_t>(h, sizeof h)) + ".json"));
}

void library::keep_listing(std::string_view server, std::string_view folder_key, const listing& l) const {
  json::writer w;
  w.begin_object().key("key").string(folder_key).key("title").string(l.title).key("folders").begin_array();
  for (const auto& f : l.folders) {
    w.begin_object().key("key").string(f.key).key("name").string(f.name).key("offline").boolean(f.offline).end_object();
  }
  w.end_array().key("files").begin_array();
  for (const auto& f : l.files) {
    w.begin_object()
        .key("key").string(f.key)
        .key("name").string(f.name)
        .key("size").integer(static_cast<std::int64_t>(f.size))
        .key("mtime").integer(f.mtime)
        .end_object();
  }
  w.end_array().end_object();
  const std::string path = listing_file(server, folder_key);
  std::error_code ec;
  fs::create_directories(fs_path(path).parent_path(), ec);
  const std::string& text = w.str();
  (void)write_atomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<listing> library::kept_listing(std::string_view server, std::string_view folder_key) {
  auto bytes = io::read_all(listing_file(server, folder_key));
  if (!bytes) return std::nullopt;
  auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!doc) return std::nullopt;
  const std::string* key = doc->str("key");
  if (!key || *key != folder_key) return std::nullopt;  // a hash collision, or not ours
  listing out;
  out.stale = true;
  if (const auto* t = doc->str("title")) out.title = *t;
  const std::string prefix = server_key(server);
  if (const json::value* folders = doc->find("folders"); folders && folders->k == json::kind::array) {
    for (const auto& v : folders->a) {
      const std::string* k = v.str("key");
      const std::string* n = v.str("name");
      if (!k || !n || k->rfind(prefix, 0) != 0 || !parse_key(*k)) continue;
      out.folders.push_back({*k, *n, v.boolean("offline").value_or(false)});
    }
  }
  std::lock_guard lock(stamps_m_);
  if (const json::value* files = doc->find("files"); files && files->k == json::kind::array) {
    for (const auto& v : files->a) {
      const std::string* k = v.str("key");
      const std::string* n = v.str("name");
      const auto size = v.integer("size");
      const auto mtime = v.integer("mtime");
      if (!k || !n || !size || !mtime || *size < 0 || k->rfind(prefix, 0) != 0) continue;
      auto parts = parse_key(*k);
      if (!parts || parts->folder) continue;
      file f{*k, *n, static_cast<std::uint64_t>(*size), *mtime};
      stamps_.emplace(f.key, std::make_pair(f.size, f.mtime));
      out.files.push_back(std::move(f));
    }
  }
  return out;
}

result<listing> library::list_live(server_rt& srv, const key_parts& kp, std::string_view folder_key, bool person,
                                   const mobile::cancel_flag* cancel) {
  auto s = &srv;
  const key_parts* k = &kp;
  MV_TRY_VOID(connect(*s, person, cancel));
  stats_.listings.fetch_add(1, std::memory_order_relaxed);
  listing out;
  if (k->root.empty()) {
    auto roots = s->link->roots();
    if (!roots) return err(roots.error());
    note_rights(*s, roots.value());
    out.title = s->name;
    std::lock_guard lock(stamps_m_);
    for (const serve::root_info& r : roots.value()) {
      if (r.label == serve::kCatalogueLabel) continue;  // the catalogue is MediaViewer's, not a folder
      key_parts p;
      p.server = s->id;
      p.root = r.id;
      if (!is_hex16(p.root)) continue;
      folder f{make_key(p), r.label, r.offline};
      labels_[f.key] = r.label;
      out.folders.push_back(std::move(f));
    }
    return out;
  }
  auto entries = s->link->list(k->root, k->rel);
  if (!entries) return err(entries.error());
  // A folder opened before its server's top this run: what this device may change there.
  if (!s->rights_known.load()) {
    if (auto roots = s->link->roots()) note_rights(*s, roots.value());
  }
  out.title = k->rel.empty() ? display_name(folder_key) : leaf_name(folder_key);
  if (out.title.empty() && k->rel.empty()) {
    // Opened before its server's top was listed this session (a pinned tree,
    // a restored folder): its label from the service.
    if (auto roots = s->link->roots()) {
      std::lock_guard lock(stamps_m_);
      for (const auto& r : roots.value()) {
        key_parts p{s->id, r.id, {}, true};
        if (is_hex16(r.id)) labels_[make_key(p)] = r.label;
        if (r.id == k->root) out.title = r.label;
      }
    }
  }
  std::lock_guard lock(stamps_m_);
  for (const serve::list_entry& e : entries.value()) {
    if (is_hidden(e.name)) continue;
    key_parts p;
    p.server = s->id;
    p.root = k->root;
    p.rel = join_rel(k->rel, e.name);
    if (e.dir) {
      p.folder = true;
      out.folders.push_back({make_key(p), e.name, false});
      continue;
    }
    if (!io::is_listed_extension(io::extension_of(e.name))) continue;
    p.folder = false;
    file f{make_key(p), e.name, e.size, e.mtime};
    stamps_[f.key] = {e.size, e.mtime};
    out.files.push_back(std::move(f));
  }
  std::sort(out.folders.begin(), out.folders.end(), [](const folder& a, const folder& b) { return less_name(a.name, b.name); });
  std::sort(out.files.begin(), out.files.end(), [](const file& a, const file& b) { return less_name(a.name, b.name); });
  return out;
}

std::optional<std::pair<std::uint64_t, std::int64_t>> library::stamp(std::string_view file_key) const {
  std::lock_guard lock(stamps_m_);
  auto it = stamps_.find(file_key);
  if (it == stamps_.end()) return std::nullopt;
  return it->second;
}

std::optional<identity> library::identity_of(std::string_view file_key) {
  auto k = parse_key(file_key);
  if (!k || k->folder || k->root.empty()) return std::nullopt;
  identity id;
  id.server = k->server;
  id.root = k->root;
  id.rel = k->rel;
  if (auto st = stamp(file_key)) {
    id.size = st->first;
    id.mtime = st->second;
  } else {
    // Not listed this session (a key restored from before): one stat.
    auto s = find(k->server);
    if (!s || !connect(*s, false, nullptr)) return std::nullopt;
    auto info = s->link->stat(k->root, k->rel);
    if (!info || info->dir) return std::nullopt;
    id.size = info->size;
    id.mtime = info->mtime;
    std::lock_guard lock(stamps_m_);
    stamps_[std::string(file_key)] = {id.size, id.mtime};
  }
  return id;
}

std::optional<std::string> library::cached(std::string_view file_key) {
  auto k = parse_key(file_key);
  auto st = stamp(file_key);
  if (!k || k->folder || !st) return std::nullopt;
  return cache_.find(identity{k->server, k->root, k->rel, st->first, st->second});
}

std::optional<std::string> library::cached_tile(std::string_view file_key) {
  auto k = parse_key(file_key);
  auto st = stamp(file_key);
  if (!k || k->folder || !st) return std::nullopt;
  const std::string path = cache_.tile_path(identity{k->server, k->root, k->rel, st->first, st->second});
  if (!io::file_exists(path)) return std::nullopt;
  return path;
}

expected library::read_range(std::string_view file_key, std::uint64_t at, std::uint64_t length,
                             const mobile::byte_sink& sink, const mobile::cancel_flag* cancel) {
  auto k = parse_key(file_key);
  if (!k || k->folder || k->root.empty()) return err(status::invalid_arg);
  auto s = find(k->server);
  if (!s) return err(status::not_found);
  MV_TRY_VOID(connect(*s, true, cancel));
  return s->link->read_range(
      k->root, k->rel, at, length,
      [&](std::uint64_t off, std::span<const std::uint8_t> bytes) {
        stats_.bytes_read.fetch_add(bytes.size(), std::memory_order_relaxed);
        return sink(off, bytes);
      },
      cancel, false);
}

expected library::fetch_into(server_rt& s, const key_parts& k, const identity& id, const mobile::cancel_flag* cancel,
                             const progress_fn& progress) {
  const std::string final_path = cache_.original_path(id);
  const std::string part = final_path + ".part";
  std::error_code ec;
  fs::create_directories(fs_path(final_path).parent_path(), ec);
  std::ofstream out(fs_path(part), std::ios::binary | std::ios::trunc);
  if (!out) return err(status::io);
  std::uint64_t written = 0;
  int tries_left = kFetchTries;
  while (written < id.size || id.size == 0) {
    MV_TRY_VOID(connect(s, true, cancel));
    const std::uint64_t before = written;
    auto done = s.link->read_range(
        k.root, k.rel, written, id.size - written,
        [&](std::uint64_t off, std::span<const std::uint8_t> bytes) {
          if (cancel && cancel->stopped()) return false;
          if (off != written) return false;  // never a hole
          out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
          if (!out) return false;
          written += bytes.size();
          stats_.bytes_read.fetch_add(bytes.size(), std::memory_order_relaxed);
          if (progress) progress(written, id.size);
          return true;
        },
        cancel, /*bulk=*/true);
    if (done) break;
    if (done.error() != status::io || (cancel && cancel->stopped())) {
      out.close();
      fs::remove(fs_path(part), ec);
      return err(cancel && cancel->stopped() ? status::cancelled : done.error());
    }
    // The link went: go on from where it got to, a few times without progress at most.
    if (written == before && --tries_left <= 0) {
      out.close();
      fs::remove(fs_path(part), ec);
      return err(status::io);
    }
  }
  out.close();
  if (!out || written != id.size) {
    fs::remove(fs_path(part), ec);
    return err(written != id.size ? status::corrupt : status::io);  // the file changed under us
  }
  if (!cache_.admit(id, part)) return err(status::io);
  stats_.whole_fetches.fetch_add(1, std::memory_order_relaxed);  // whole files that landed
  return {};
}

result<std::string> library::fetch(std::string_view file_key, const mobile::cancel_flag* cancel,
                                   const progress_fn& progress) {
  auto k = parse_key(file_key);
  if (!k || k->folder || k->root.empty()) return err(status::invalid_arg);
  auto id = identity_of(file_key);
  if (!id) return err(status::not_found);
  if (auto hit = cache_.find(*id)) return *hit;
  auto s = find(k->server);
  if (!s) return err(status::not_found);
  for (;;) {
    std::shared_ptr<fetch_state> st;
    bool lead = false;
    {
      std::lock_guard lock(fetch_m_);
      auto it = fetching_.find(file_key);
      if (it == fetching_.end()) {
        st = std::make_shared<fetch_state>();
        fetching_.emplace(std::string(file_key), st);
        lead = true;
      } else {
        st = it->second;
      }
    }
    if (lead) {
      // A fetch that finished between our cache look and taking the lead.
      expected done = cache_.find(*id) ? expected{} : fetch_into(*s, *k, *id, cancel, progress);
      {
        std::lock_guard lock(st->m);
        st->done = true;
        st->outcome = done ? result<std::string>(cache_.original_path(*id)) : result<std::string>(err(done.error()));
      }
      st->cv.notify_all();
      {
        std::lock_guard lock(fetch_m_);
        fetching_.erase(std::string(file_key));
      }
      if (!done) return err(done.error());
      return cache_.original_path(*id);
    }
    // Someone else's fetch of the same file: wait for it (polling our own cancel).
    std::unique_lock lock(st->m);
    while (!st->done) {
      if (cancel && cancel->stopped()) return err(status::cancelled);
      st->cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (st->outcome) return std::string(st->outcome.value());
    if (st->outcome.error() != status::cancelled) return err(st->outcome.error());
    // The leader was cancelled (its viewer moved on) and this caller was not: lead now.
  }
}

result<std::vector<std::uint8_t>> library::tile(std::string_view file_key, const job_context* ctx,
                                                const mobile::cancel_flag* cancel) {
  auto k = parse_key(file_key);
  if (!k || k->folder || k->root.empty()) return err(status::invalid_arg);
  auto id = identity_of(file_key);
  if (!id) return err(status::not_found);
  const std::string tile_path = cache_.tile_path(*id);
  if (auto have = io::read_all(tile_path); have && !have->empty()) return std::move(have).value();
  auto s = find(k->server);
  if (!s) return err(status::not_found);
  bool known = false;
  const io::media_kind kind = kind_of(k->rel, known);
  if (!known || kind == io::media_kind::audio || kind == io::media_kind::document) {
    return err(status::unsupported_format);
  }
  const auto stopped = [ctx, cancel] { return (ctx && ctx->cancelled()) || (cancel && cancel->stopped()); };
  result<std::vector<std::uint8_t>> jpeg = err(status::unsupported_format);
  if (kind == io::media_kind::video) {
    if (!cfg_.clip_tile) return err(status::unsupported_format);
    MV_TRY_VOID(connect(*s, false, nullptr));
    link_reader reader(*s->link, k->root, k->rel, id->size, ctx, stats_.bytes_read);
    jpeg = cfg_.clip_tile(reader, ctx);
  } else {
    if (!cfg_.still_tile) return err(status::unsupported_format);
    // A file already here, or small enough to fetch whole (it is then here
    // for the viewer too): its bytes. A large one: its head first, where a
    // camera's embedded preview usually is.
    std::optional<std::string> local = cache_.find(*id);
    // Only a RAW's head: its embedded preview is a whole JPEG near the start. A
    // JPEG, HEIC or TIFF cut short would decode (or half-decode) to a wrong
    // picture, so those are read whole.
    if (!local && id->size > kWholeTileBytes && kind == io::media_kind::raw) {
      MV_TRY_VOID(connect(*s, false, nullptr));
      std::vector<std::uint8_t> head;
      head.reserve(static_cast<std::size_t>(kHeadTileBytes));
      stats_.head_reads.fetch_add(1, std::memory_order_relaxed);
      auto read = s->link->read_range(
          k->root, k->rel, 0, kHeadTileBytes,
          [&](std::uint64_t, std::span<const std::uint8_t> bytes) {
            if (stopped()) return false;
            head.insert(head.end(), bytes.begin(), bytes.end());
            stats_.bytes_read.fetch_add(bytes.size(), std::memory_order_relaxed);
            return true;
          },
          cancel, false);
      if (stopped()) return err(status::cancelled);
      if (read) jpeg = cfg_.still_tile(head, ctx);
    }
    if (!jpeg) {
      if (!local) {
        if (stopped()) return err(status::cancelled);
        if (!connect(*s, false, nullptr)) return err(status::io);
        mobile::cancel_flag flag;  // the job's cancel reaches the fetch through its sink
        if (stopped()) flag.stop.store(true);
        auto fetched = fetch(file_key, cancel ? cancel : &flag, [&](std::uint64_t, std::uint64_t) {
          if (stopped()) flag.stop.store(true);
        });
        if (!fetched) return err(fetched.error());
        local = std::move(fetched).value();
      }
      auto bytes = io::read_all(*local);
      if (!bytes) return err(bytes.error());
      jpeg = cfg_.still_tile(bytes.value(), ctx);
    }
  }
  if (!jpeg) return err(jpeg.error());
  std::error_code ec;
  fs::create_directories(fs_path(tile_path).parent_path(), ec);
  (void)write_atomic(tile_path, jpeg.value());
  return jpeg;
}

void library::forget_rights(server_rt& s) {
  s.changes.store(false);
  s.rights_known.store(false);
  std::lock_guard lock(stamps_m_);
  const std::string prefix = s.id + "/";
  for (auto it = writable_.lower_bound(prefix); it != writable_.end() && it->first.rfind(prefix, 0) == 0; ++it) {
    it->second = false;
  }
}

// A change refused "not-allowed": the role or a folder's grant changed since this link's hello
// (the service cuts a link whose limit changed, but a change in flight can still meet it). Ask
// again now -- a fresh hello for the role, the roots for each folder -- so the next answer is
// the server's, not the one this link started with.
void library::recheck_rights(server_rt& s) {
  mobile::route_report report;
  if (s.link->reconnect(report, nullptr, cfg_.connect_timeout_ms)) {
    std::string word;
    if (s.link->role(word) >= 0) s.role.store(role_code(word));
    if (auto roots = s.link->roots()) {
      note_rights(s, roots.value());
    } else {
      forget_rights(s);
    }
  } else {
    forget_rights(s);
  }
  gen_.fetch_add(1, std::memory_order_acq_rel);
}

void library::note_rights(server_rt& s, const std::vector<serve::root_info>& roots) {
  auto changes = s.link->changes_files();
  const bool can = changes && changes.value();
  bool moved = s.changes.exchange(can) != can;
  {
    std::lock_guard lock(stamps_m_);
    // A folder the server no longer lists for this device (a narrower grant) is not writable:
    // every entry of this server is cleared first, then the listed ones set.
    const std::string prefix = s.id + "/";
    for (auto it = writable_.lower_bound(prefix); it != writable_.end() && it->first.rfind(prefix, 0) == 0; ++it) {
      if (it->second) moved = true;
      it->second = false;
    }
    for (const auto& r : roots) {
      if (!is_hex16(r.id)) continue;
      // Its label too: a subfolder opened first (Back, a pin) still names its folder in the trail.
      labels_[make_key(key_parts{s.id, r.id, {}, true})] = r.label;
      const bool w = can && r.writable && r.label != serve::kCatalogueLabel;
      bool& slot = writable_[s.id + "/" + r.id];
      if (w) moved = true;  // set again after the clear above; a bump is cheap
      slot = w;
    }
  }
  s.rights_known.store(true);
  if (moved) gen_.fetch_add(1, std::memory_order_acq_rel);  // the commands say what it may do
}

bool library::can_write(std::string_view key) const noexcept {
  auto k = parse_key(key);
  if (!k || k->root.empty()) return false;
  auto s = find(k->server);
  if (!s || !s->changes.load() || role_word(s->role.load()) == std::string_view("view")) return false;
  std::lock_guard lock(stamps_m_);
  auto it = writable_.find(k->server + "/" + k->root);
  return it != writable_.end() && it->second;
}

std::string library::write_problem(std::string_view key) const {
  if (can_write(key)) return {};
  auto k = parse_key(key);
  auto s = k ? find(k->server) : nullptr;
  if (!s) return "On another computer: read-only from here";
  const std::string name = s->name.empty() ? std::string("the server") : s->name;
  if (k->root.empty()) return "Pick one of " + name + "'s folders first";
  if (s->state.load() != reach::online) return name + " isn't answering: nothing can be changed there now";
  if (!s->rights_known.load()) return "Still asking " + name + " what this computer may change";
  if (role_word(s->role.load()) == std::string_view("view")) {
    return name + " lets this computer view only. Its owner can allow changes in driftwire.";
  }
  if (!s->changes.load()) return name + " needs an update before files can be changed from here";
  return "Read-only: " + name + " doesn't let this computer change this folder";
}

result<std::shared_ptr<library::server_rt>> library::live_server(const key_parts& k, const mobile::cancel_flag* cancel) {
  auto s = find(k.server);
  if (!s) return err(status::not_found);
  MV_TRY_VOID(connect(*s, true, cancel));
  if (!s->rights_known.load()) {
    if (auto roots = s->link->roots()) note_rights(*s, roots.value());
  }
  return s;
}

void library::forget_changed(std::string_view server, std::string_view folder_key, std::string_view item_key) {
  std::error_code ec;
  fs::remove(fs_path(listing_file(server, folder_key)), ec);
  if (item_key.empty()) return;
  // A folder that moved or went: the listings kept for the folders inside it too, or a server
  // that does not answer would show them again under keys that no longer exist. Each file
  // names its key; a walk of this server's few listings, on the worker.
  if (!item_key.empty() && item_key.back() == '/') {
    const fs::path dir = fs_path(listing_file(server, item_key)).parent_path();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      auto bytes = io::read_all(utf8_of(it->path()));
      if (!bytes) continue;
      auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
      const std::string* key = doc ? doc->str("key") : nullptr;
      if (key && key->rfind(item_key, 0) == 0) {
        std::error_code rm;
        fs::remove(it->path(), rm);
      }
    }
  }
  std::lock_guard lock(stamps_m_);
  // A file's stamp, or everything listed under a folder that moved or went.
  for (auto it = stamps_.lower_bound(item_key); it != stamps_.end() && it->first.rfind(item_key, 0) == 0;) {
    it = stamps_.erase(it);
  }
}

namespace {
// A change's refusal as the caller's status (serve_client.h's table), with `why` filled.
status change_status(status st, const serve::client::change_refused& w, serve::client::change_refused* why) {
  if (why) *why = w;
  // "busy": a write is open on that name, or beneath the folder, right now. Not a refusal
  // to say no to: the same change works in a moment, and the words say so.
  if (w.why == serve::client::change_why::busy) return status::busy;
  return st;
}
std::string key_in(const key_parts& folder, const std::string& rel, bool is_folder) {
  key_parts p{folder.server, folder.root, rel, is_folder};
  return make_key(p);
}
}  // namespace

result<std::string> library::add_file(std::string_view folder_key, const std::string& local_path,
                                      const std::string& name, const mobile::cancel_flag* cancel,
                                      const std::function<void(std::uint64_t)>& progress, change_refused* why) {
  auto k = parse_key(folder_key);
  if (!k || !k->folder || k->root.empty() || name.empty() || is_hidden(name) || name.find('/') != std::string::npos) {
    return err(status::invalid_arg);
  }
  std::error_code ec;
  const std::uint64_t size = fs::file_size(fs_path(local_path), ec);
  if (ec) return err(status::not_found);
  auto hashed = io::hash_file(local_path, io::read_mode::sequential, cancel ? &cancel->stop : nullptr);
  if (!hashed) return err(hashed.error());
  const std::string hash = hashed.value().hex();
  std::ifstream in(fs_path(local_path), std::ios::binary);
  if (!in) return err(status::io);
  const serve::client::read_fn read = [&in](std::uint64_t at, std::span<std::uint8_t> into) -> result<std::size_t> {
    in.clear();
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
    if (in.bad()) return err(status::io);
    return static_cast<std::size_t>(in.gcount());
  };
  const std::string rel = join_rel(k->rel, name);
  std::uint64_t at = 0;
  int tries = 6;
  for (;;) {
    auto s = live_server(*k, cancel);
    if (!s) return err(s.error());
    change_refused w;
    auto r = s.value()->link->add(k->root, rel, size, hash, at, read, progress, cancel, &w);
    if (r) {
      forget_changed(k->server, folder_key, {});
      return key_in(*k, r->rel, false);
    }
    if (cancel && cancel->stopped()) return err(change_status(status::cancelled, w, why));
    // Where the unfinished copy on the server ends: go on from there.
    if (w.why == serve::client::change_why::offset && w.have != at && --tries > 0) {
      at = w.have;
      continue;
    }
    // The link went: connect again and ask; an unfinished copy answers with an offset.
    if ((r.error() == status::io || r.error() == status::timeout) && --tries > 0) continue;
    if (w.why == serve::client::change_why::not_allowed) recheck_rights(*s.value());
    return err(change_status(r.error(), w, why));
  }
}

result<std::string> library::make_folder(std::string_view parent_key, const std::string& name, change_refused* why) {
  auto k = parse_key(parent_key);
  if (!k || !k->folder || k->root.empty() || name.empty() || is_hidden(name) || name.find('/') != std::string::npos) {
    return err(status::invalid_arg);
  }
  auto s = live_server(*k, nullptr);
  if (!s) return err(s.error());
  change_refused w;
  auto r = s.value()->link->make_folder(k->root, join_rel(k->rel, name), &w);
  if (!r) {
    if (w.why == serve::client::change_why::not_allowed) recheck_rights(*s.value());
    return err(change_status(r.error(), w, why));
  }
  forget_changed(k->server, parent_key, {});
  if (r->existing) return err(change_status(status::permission_denied, {serve::client::change_why::exists, "exists", 0}, why));
  return key_in(*k, r->rel, true);
}

result<std::string> library::move(std::string_view key, std::string_view to_folder_key, const std::string& name,
                                  change_refused* why) {
  auto from = parse_key(key);
  auto to = parse_key(to_folder_key);
  if (!from || !to || from->root.empty() || from->rel.empty() || !to->folder || to->root.empty() ||
      from->server != to->server || name.empty() || is_hidden(name) || name.find('/') != std::string::npos) {
    return err(status::invalid_arg);
  }
  auto s = live_server(*from, nullptr);
  if (!s) return err(s.error());
  change_refused w;
  auto r = s.value()->link->move(from->root, from->rel, to->root, join_rel(to->rel, name), &w);
  if (!r) {
    if (w.why == serve::client::change_why::not_allowed) recheck_rights(*s.value());
    return err(change_status(r.error(), w, why));
  }
  forget_changed(from->server, parent_key(key), key);
  forget_changed(to->server, to_folder_key, {});
  return key_in(*to, r->rel, from->folder);
}

result<std::string> library::remove(std::string_view key, change_refused* why) {
  auto k = parse_key(key);
  if (!k || k->root.empty() || k->rel.empty()) return err(status::invalid_arg);
  auto s = live_server(*k, nullptr);
  if (!s) return err(s.error());
  change_refused w;
  auto r = s.value()->link->remove(k->root, k->rel, &w);
  if (!r) {
    if (w.why == serve::client::change_why::not_allowed) recheck_rights(*s.value());
    return err(change_status(r.error(), w, why));
  }
  forget_changed(k->server, parent_key(key), key);
  return r->rel;
}

result<bool> library::exists(std::string_view key) {
  auto k = parse_key(key);
  if (!k || k->root.empty()) return err(status::invalid_arg);
  if (k->rel.empty()) return true;
  auto s = live_server(*k, nullptr);
  if (!s) return err(s.error());
  auto info = s.value()->link->stat(k->root, k->rel);
  if (info) return true;
  if (info.error() == status::not_found) return false;
  return err(info.error());
}

}  // namespace mv::remote
