// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone access in the app (link.h). Ported from Local search's
// engine_phone_link.cpp (2026-11-12) with the same requests, states and
// failure rules; what changed is who owns what. The library's folder, id and
// epoch are this feature's (state.json), the desk is called directly, and
// Local search, when loaded, is a source of generations followed here.
#include "phone/link.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "catalogue/catalog.h"
#include "catalogue/catalog_chain.h"
#include "catalogue/publisher.h"
#include "core/json.h"

namespace mv::phone {

namespace catalog = mv::ai::catalog;

namespace {

constexpr int kPairPollMs = 2000;

std::filesystem::path fs_path(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string last_part(std::string path) {
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
  const auto at = path.find_last_of("/\\");
  return at == std::string::npos ? path : path.substr(at + 1);
}

std::string slurp(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// Read whole: its size, then exactly that many bytes. nullopt: not read.
std::optional<std::string> read_whole(const std::string& file) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(fs_path(file), ec);
  if (ec || size > 64 * 1024) return std::nullopt;
  std::ifstream in(fs_path(file), std::ios::binary);
  if (!in) return std::nullopt;
  std::string text(static_cast<std::size_t>(size), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (in.gcount() != static_cast<std::streamsize>(text.size())) return std::nullopt;
  return text;
}

// Whole, on the disk, then renamed in: a crash leaves the old file or this one.
bool write_durable(const std::string& dir, const std::string& file, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(fs_path(dir), ec);
  const std::string part = file + ".part";
  bool ok = false;
#if defined(_WIN32)
  FILE* f = _wfopen(fs_path(part).c_str(), L"wb");
#else
  FILE* f = std::fopen(part.c_str(), "wb");
#endif
  if (f) {
    ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#if defined(_WIN32)
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(::fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
  }
  if (ok) {
    std::filesystem::rename(fs_path(part), fs_path(file), ec);
    ok = !ec;
  }
  return ok;
}

// A key read from the share: anything that is not 32 bytes of hex is no key.
std::vector<std::uint8_t> key_bytes(std::string_view hex) {
  const auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  if (hex.size() != 2 * catalog::kPublicKeyBytes) return {};
  std::vector<std::uint8_t> out(catalog::kPublicKeyBytes);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return {};
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return out;
}

// What a failure from the desk is called on the card.
const char* error_name(status s) noexcept {
  switch (s) {
    case status::not_found: return "no_driftwire";
    case status::permission_denied: return "refused";
    case status::invalid_arg: return "bad_address";
    case status::busy: return "too_many";
    case status::unsupported_format: return "nas_too_old";
    default: return "unreachable";
  }
}

// The NAS's own reason for a refusal (desk last_refusal), as the card names it;
// nullptr when it gave none the card has words for.
const char* refusal_name(std::string_view why) noexcept {
  if (why == "needs-unlock") return "needs_unlock";
  if (why == "passphrase" || why == "passphrase-changed") return "passphrase";
  if (why == "locked-out") return "locked_out";
  if (why == "no-passphrase") return "no_passphrase";
  if (why == "slow-down") return "slow_down";
  if (why == "wrong-service") return "wrong_service";
  return nullptr;
}

}  // namespace

link::link(options o) : o_(std::move(o)) {}

link::~link() { stop(); }

void link::start_thread_locked() {
  if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
  cv_.notify_all();
}

expected link::request(const std::string& request_json) {
  if (stopping_.load()) return err(status::cancelled);
  const auto doc = json::parse(request_json, 4);
  if (!doc || !doc->str("do")) return err(status::invalid_arg);
  std::lock_guard lock(m_);
  if (stopping_.load()) return err(status::cancelled);
  v_.busy = *doc->str("do");  // from now, so a poll right after this sees it
  requests_.push_back(request_json);
  start_thread_locked();
  return {};
}

void link::resume() {
  std::lock_guard lock(m_);
  if (!stopping_.load()) start_thread_locked();
}

void link::set_source(catalogue_source s) {
  {
    std::lock_guard lock(source_m_);
    source_ = std::move(s);
  }
  // Loaded: one look now (the card says so, a library it published before the
  // move is taken). Gone: the card says so. Nothing is queued while stopping,
  // and no thread is started for a source alone with nothing set up.
  bool any = false;
  {
    std::lock_guard source_lock(source_m_);
    any = static_cast<bool>(source_);
  }
  std::lock_guard lock(m_);
  if (stopping_.load()) return;
  if (any || !pub_.destination.empty()) {
    requests_.push_back("{\"do\":\"source\"}");
    start_thread_locked();
  }
}

void link::stop() noexcept {
  std::thread t;
  {
    std::lock_guard lock(m_);
    stopping_ = true;
    t = std::move(thread_);
    cv_.notify_all();
  }
  if (!t.joinable()) return;  // never used this run: nothing to tidy, no secret store woken
  // The thread tidies up itself (quit_tidy) and ends: one writer of link.json,
  // and the desk is never called from the stopping thread, which is the app's
  // main thread at quit.
  if (o_.detach_on_stop) {
    t.detach();  // this link is never destroyed (phone/app.cpp)
  } else {
    t.join();
  }
}

// The app going, on the link thread: a code showing is not withdrawn (a round
// trip to a NAS that may not answer); it is written down and withdrawn at the
// next start, and the NAS lets it lapse within five minutes anyway. Only what
// is on this computer goes now: the nearby service and a held session.
void link::quit_tidy() noexcept {
  try {
    bool showing = false, nearby = false;
    {
      std::lock_guard lock(m_);
      showing = v_.qr_expires != 0;
      nearby = v_.nearby;
      if (showing) {
        v_.code_left = true;
        v_.code_left_until = static_cast<std::int64_t>(std::time(nullptr)) + static_cast<std::int64_t>(o_.code_ttl_s) + 1;
      }
    }
    if (showing) save_flags();
    if (nearby) (void)route("nearby_stop", "{}");
    if (nearby || showing) (void)route("release", "{}");
  } catch (...) {
  }
}

// ---- what survives a restart ------------------------------------------------------------

// link.json: a hand-over not yet confirmed by a share that reads back, and a
// code left on the NAS.
void link::load_flags() {
  const std::string file = o_.data_dir + "/link.json";
  std::error_code ec;
  const bool there = std::filesystem::exists(fs_path(file), ec);
  if (!ec && !there) {
    // Never written: nothing handed over, nothing left. The one case that is open.
    std::lock_guard lock(m_);
    v_.flags_read = true;
    return;
  }
  auto text = read_whole(file);
  if (!text) {
    // A file being replaced, a disk that hiccuped: once more after a moment.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    text = read_whole(file);
  }
  const auto doc = text ? json::parse(*text, 4) : std::nullopt;
  std::lock_guard lock(m_);
  if (!doc || doc->k != json::kind::object) {
    // There and not read, or read and not ours to understand: fail closed.
    // Signing nearby copies with a key that may have been handed away is worse
    // than not serving nearby. Nothing is written over it until a read
    // succeeds (a later run), so a passing failure cannot erase a hand-over.
    v_.handed_over = true;
    v_.other_publisher = true;
    v_.flags_read = false;
    return;
  }
  v_.flags_read = true;
  v_.handed_over = doc->boolean("handed_over").value_or(false);
  v_.code_left = doc->boolean("code_left").value_or(false);
  v_.code_left_until = doc->integer("code_left_until").value_or(0);
  if (v_.handed_over) v_.other_publisher = true;
}

void link::save_flags() {
  bool handed = false, left = false;
  std::int64_t until = 0;
  {
    std::lock_guard lock(m_);
    // The file there could not be read: never overwritten blind, except to
    // record a hand-over, which is the side a failed read assumes anyway.
    if (!v_.flags_read && !v_.handed_over) {
      v_.flags_unsaved = true;
      return;
    }
    handed = v_.handed_over;
    left = v_.code_left;
    until = v_.code_left_until;
  }
  json::writer w;
  w.begin_object()
      .key("handed_over").boolean(handed)
      .key("code_left").boolean(left)
      .key("code_left_until").integer(until)
      .end_object();
  const bool ok = write_durable(o_.data_dir, o_.data_dir + "/link.json", w.str());
  std::lock_guard lock(m_);
  v_.flags_unsaved = !ok;
  if (ok) v_.flags_read = true;  // what is there now is what this wrote
}

// folder.json: the folder, the library's id and its epoch.
void link::load_state() {
  const auto text = read_whole(o_.data_dir + "/folder.json");
  const auto doc = text ? json::parse(*text, 4) : std::nullopt;
  std::lock_guard lock(m_);
  state_read_ = true;
  if (!doc || doc->k != json::kind::object) return;
  if (const std::string* d = doc->str("destination")) pub_.destination = *d;
  if (const std::string* id = doc->str("library_id")) pub_.library_id = *id;
  pub_.epoch = static_cast<std::uint64_t>(std::max<std::int64_t>(1, doc->integer("epoch").value_or(1)));
}

void link::save_state() {
  json::writer w;
  {
    std::lock_guard lock(m_);
    w.begin_object()
        .key("destination").string(pub_.destination)
        .key("library_id").string(pub_.library_id)
        .key("epoch").integer(static_cast<std::int64_t>(pub_.epoch))
        .end_object();
  }
  if (!write_durable(o_.data_dir, o_.data_dir + "/folder.json", w.str())) {
    std::lock_guard lock(m_);
    v_.flags_unsaved = true;
  }
}

// ---- Local search ------------------------------------------------------------------------

void link::read_source() {
  std::string text;
  bool any = false;
  {
    std::lock_guard lock(source_m_);
    any = static_cast<bool>(source_);
    if (any) text = source_.json();
  }
  const auto doc = any ? json::parse(text, 4) : std::nullopt;
  std::lock_guard lock(m_);
  pub_.source = any;
  pub_.source_json = any && doc && doc->k == json::kind::object ? text : std::string();
  if (!doc || doc->k != json::kind::object) {
    pub_.generation = pub_.waiting = 0;
    pub_.gen_dir.clear();
    return;
  }
  pub_.generation = static_cast<std::uint64_t>(std::max<std::int64_t>(0, doc->integer("generation").value_or(0)));
  pub_.waiting = static_cast<std::uint64_t>(std::max<std::int64_t>(0, doc->integer("waiting").value_or(0)));
  const std::string* g = doc->str("gen_dir");
  pub_.gen_dir = g ? *g : std::string();
  const std::string* id = doc->str("library_id");
  pub_.source_library_id = id ? *id : std::string();
  pub_.source_epoch = static_cast<std::uint64_t>(std::max<std::int64_t>(0, doc->integer("epoch").value_or(0)));
}

// Before the move, Local search held the folder, the id and the epoch. A
// library it published keeps them: taken once, when this has none.
void link::adopt_from_source() {
  std::string text;
  {
    std::lock_guard lock(m_);
    if (!pub_.destination.empty()) return;
    text = pub_.source_json;
  }
  const auto doc = json::parse(text, 4);
  const std::string* d = doc ? doc->str("destination") : nullptr;
  if (!d || d->empty()) return;
  {
    std::lock_guard lock(m_);
    pub_.destination = *d;
    if (const std::string* id = doc->str("library_id"); id && !id->empty()) pub_.library_id = *id;
    if (const auto e = doc->integer("epoch"); e && *e >= 1) pub_.epoch = static_cast<std::uint64_t>(*e);
  }
  save_state();
}

// ---- the thread --------------------------------------------------------------------------

result<std::string> link::route(const char* method, const std::string& json) {
  // Stopping: no new wait on the NAS. What only tidies this computer (or
  // withdraws a code) still goes.
  const std::string_view m(method);
  if (stopping_.load() && m != "nearby_stop" && m != "release" && m != "pair_stop") return err(status::cancelled);
  if (!o_.route) return err(status::not_found);
  auto r = o_.route(method, json);
  if (!r && r.error() == status::not_found && m == "status") {
    std::lock_guard lock(m_);
    v_.driftwire = false;
  }
  return r;
}

result<std::vector<std::uint8_t>> link::signing_key(bool make) {
  auto store = o_.key_store ? o_.key_store() : nullptr;
  auto legacy = o_.legacy_key_store ? o_.legacy_key_store() : nullptr;
  return catalog::publisher_key(store.get(), legacy.get(), make);
}

void link::loop() {
  load_flags();  // before the first request: small files, read once per run
  load_state();
  read_source();
  adopt_from_source();
  for (;;) {
    std::string request;
    bool poll = false, ticked = false;
    {
      std::unique_lock lock(m_);
      const auto ready = [this] { return stopping_.load() || !requests_.empty(); };
      if (v_.qr_expires != 0) {
        poll = !cv_.wait_for(lock, std::chrono::milliseconds(kPairPollMs), ready);
      } else if (!pub_.destination.empty()) {
        ticked = !cv_.wait_for(lock, std::chrono::milliseconds(static_cast<std::int64_t>(o_.tick_s * 1000)), ready);
      } else {
        cv_.wait(lock, ready);
      }
      if (stopping_.load()) {
        lock.unlock();
        quit_tidy();
        return;
      }
      if (!poll && !ticked) {
        request = std::move(requests_.front());
        requests_.pop_front();
        const auto doc = json::parse(request, 4);
        v_.busy = *doc->str("do");
        v_.error.clear();
      }
    }
    try {
      if (ticked) {
        read_source();
        adopt_from_source();  // a library Local search published before the move
        tick();
        continue;
      }
      if (poll) {
        run("{\"do\":\"poll\"}");
      } else {
        const auto doc = json::parse(request, 4);
        const bool pairing = doc && doc->str("do") && *doc->str("do") == "pair";
        {
          // Set by this thread alone, around the one request that makes a
          // code: `busy` follows whatever is queued next and cannot say so.
          std::lock_guard lock(m_);
          v_.pairing = pairing;
        }
        run(request);
        {
          std::lock_guard lock(m_);
          v_.pairing = false;
        }
        // The owner's session is kept only while a code shows: one forwarded
        // service, a few session slots, and a desktop that may sit here for days.
        // With no folder there is no session, and no desk to ask.
        bool showing = false, set_up = false;
        {
          std::lock_guard lock(m_);
          showing = v_.qr_expires != 0;
          set_up = !pub_.destination.empty();
        }
        if (!showing && set_up) (void)route("release", "{}");
      }
      bool dirty = false;
      {
        std::lock_guard lock(m_);
        dirty = v_.flags_dirty;
        v_.flags_dirty = false;
      }
      if (dirty) save_flags();
    } catch (...) {
      std::lock_guard lock(m_);
      v_.error = "failed";
    }
    std::lock_guard lock(m_);
    if (requests_.empty()) v_.busy.clear();
  }
}

void link::tick() {
  std::uint64_t newest = 0;
  bool used = false;
  {
    std::lock_guard lock(m_);
    used = !pub_.library_id.empty();
    newest = std::max(pub_.waiting, pub_.generation);
  }
  if (!used) return;
  std::string next;
  {
    std::lock_guard lock(m_);
    // A phone revoked here while the NAS was away keeps the NAS's door open
    // until the NAS hears it: ask again, backing off, until it has.
    const auto now = std::chrono::steady_clock::now();
    if (tick_seen_ && v_.pending > 0 && now >= v_.retry_at && requests_.empty()) {
      v_.retry_s = v_.retry_s <= 0 ? o_.retry_s : std::min(v_.retry_s * 2, 3600.0);
      v_.retry_at = now + std::chrono::milliseconds(static_cast<std::int64_t>(v_.retry_s * 1000));
      next = "{\"do\":\"refresh\"}";
    } else {
      const bool first = !tick_seen_;
      // One try per generation: no paired phone, or a copy that failed, is not
      // asked about again on every tick. A phone added later asks by itself.
      const bool stale = v_.claimed && newest != 0 && newest != v_.tried;
      if (!first && (!stale || !requests_.empty())) return;
      tick_seen_ = true;
      if (!first) v_.tried = newest;
      next = first ? "{\"do\":\"refresh\"}" : "{\"do\":\"nearby\"}";
    }
    v_.busy = "refresh";
    v_.error.clear();
  }
  run(next);
  (void)route("release", "{}");
  std::lock_guard lock(m_);
  if (requests_.empty()) v_.busy.clear();
}

void link::refresh(bool phones_only) {
  bool left = false, lapsed = false;
  {
    std::lock_guard lock(m_);
    // Never while a code is showing or being made: the withdraw would take
    // the new one down (the service holds one code at a time).
    left = v_.code_left && v_.qr_expires == 0 && !v_.pairing;
    // The NAS drops a code by itself once its lifetime is over.
    lapsed = v_.code_left && static_cast<std::int64_t>(std::time(nullptr)) >= v_.code_left_until;
  }
  // A code was showing when the app last stopped: it goes now. A definite
  // answer (done, refused, not owned) settles it; a NAS out of reach or not
  // ready leaves it for the next refresh, and never past the code's lifetime.
  bool settled = lapsed;
  if (left && !lapsed) {
    const auto r = route("pair_stop", "{}");
    const mv::status e = r ? mv::status::ok : r.error();
    settled = r || (e != status::io && e != status::timeout && e != status::cancelled && e != status::busy &&
                    e != status::not_found);
  }
  if (left && settled) {
    {
      std::lock_guard lock(m_);
      v_.code_left = false;
    }
    save_flags();
  }
  auto status_json = route("status", "{}");
  if (!status_json) {
    std::lock_guard lock(m_);
    v_.claimed = false;
    return;
  }
  const auto s = json::parse(*status_json, 4);
  const bool claimed = s && s->boolean("claimed").value_or(false);
  {
    std::lock_guard lock(m_);
    v_.driftwire = true;
    v_.claimed = claimed;
    const std::int64_t pending = s ? s->integer("pending_revokes").value_or(0) : 0;
    if (pending > 0 && v_.pending == 0) {
      v_.retry_s = 0;  // a new one: the first retry comes soon
      v_.retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<std::int64_t>(o_.retry_s * 1000));
    }
    v_.pending = pending;
    if (s) {
      if (const std::string* v = s->str("local")) v_.local = *v;
      if (const std::string* v = s->str("public")) v_.public_name = *v;
    }
  }
  if (claimed) {
    if (auto d = route("devices", "{}")) {
      const auto doc = json::parse(*d, 6);
      std::vector<row> rows;
      if (const json::value* list = doc ? doc->find("devices") : nullptr; list && list->k == json::kind::array) {
        for (const json::value& v : list->a) {
          const std::string* name = v.str("name");
          const std::string* key = v.str("key");
          if (!name || !key) continue;
          row r;
          r.name = *name;
          r.key = *key;
          r.limited = v.boolean("limited").value_or(false);
          r.expires = v.integer("expires").value_or(0);
          r.expired = v.boolean("expired").value_or(false);
          r.guest = v.boolean("guest").value_or(false);
          r.scoped = v.boolean("scoped").value_or(false);
          rows.push_back(std::move(r));
        }
      }
      std::lock_guard lock(m_);
      v_.devices = std::move(rows);
      v_.reachable = doc && doc->boolean("reachable").value_or(false);
      v_.code_waiting = doc && doc->boolean("code_waiting").value_or(false);
    }
    if (phones_only) return;  // the two-second poll under a showing code
    if (auto o = route("owner_status", "{}")) {
      const auto doc = json::parse(*o, 3);
      std::lock_guard lock(m_);
      if (doc) {
        const std::string* p = doc->str("passphrase");
        v_.nas_passphrase = p ? *p : std::string();
        v_.nas_features.clear();
        if (const json::value* f = doc->find("features"); f && f->k == json::kind::array) {
          for (const auto& e : f->a) {
            if (e.k == json::kind::string) v_.nas_features.push_back(e.s);
          }
        }
        v_.unlocked_s = doc->integer("unlocked_s").value_or(0);
        v_.locked_until = doc->integer("locked_until").value_or(0);
      }
    }
    if (auto r = route("roots", "{}")) {
      const auto doc = json::parse(*r, 6);
      std::vector<folder_row> rows;
      if (const json::value* list = doc ? doc->find("roots") : nullptr; list && list->k == json::kind::array) {
        for (const json::value& v : list->a) {
          const std::string* id = v.str("id");
          const std::string* label = v.str("label");
          const std::string* path = v.str("path");
          if (id && label && path) rows.push_back({*id, *label, *path});
        }
      }
      std::lock_guard lock(m_);
      v_.folders = std::move(rows);
    }
  }

  // Who signs this library now, as the share says.
  std::string destination;
  {
    std::lock_guard lock(m_);
    destination = pub_.destination;
  }
  if (destination.empty()) return;
  auto lib = catalog::read_library(destination);
  auto secret = signing_key(false);
  std::string mine;
  if (secret) mine = catalog::to_hex(catalog::public_of(*secret));
  bool other = false, mismatch = false, handed_over = false;
  {
    std::lock_guard lock(m_);
    handed_over = v_.handed_over;
  }
  // Handed over: only a library.json that reads and names this key again
  // makes this desktop the publisher; a share that cannot be read does not.
  if (handed_over && (!lib || lib->public_key != mine)) other = true;
  std::string other_fp;
  if (lib) {
    if (lib->public_key == mine) {
      // A takeover this desktop asked for has landed, or a key move raised the
      // epoch. Only ever forward: a restored or edited file that names another
      // library, or an older epoch, would make every phone refuse what this
      // desktop publishes, so it is shown instead of followed.
      bool adopted = false;
      {
        std::lock_guard lock(m_);
        const bool never_published = pub_.generation == 0 && pub_.waiting == 0;
        if (lib->library_id == pub_.library_id) {
          if (lib->epoch > pub_.epoch) {
            pub_.epoch = lib->epoch;
            adopted = true;
          } else if (lib->epoch < pub_.epoch) {
            mismatch = true;
          }
        } else if (never_published || pub_.library_id.empty()) {
          pub_.library_id = lib->library_id;
          pub_.epoch = lib->epoch;
          adopted = true;
        } else {
          mismatch = true;
        }
      }
      if (adopted) save_state();
    } else if (!mine.empty() || secret.error() == status::not_found) {
      other = true;
      if (const auto key = key_bytes(lib->public_key); !key.empty()) other_fp = catalog::fingerprint(key);
    }
  }
  std::string asking_name, asking_key, asking_fp;
  if (lib && !other) {
    if (auto t = catalog::read_takeover(destination)) {
      const auto key = key_bytes(t->public_key);
      if (!key.empty()) {
        asking_key = t->public_key;
        asking_name = t->name;
        asking_fp = catalog::fingerprint(key);
      }
    }
  }
  const bool cleared = handed_over && lib && lib->public_key == mine;
  if (cleared) {
    {
      std::lock_guard lock(m_);
      v_.handed_over = false;
    }
    save_flags();
  }
  std::lock_guard lock(m_);
  v_.mismatch = mismatch;
  if (v_.handed_over) other = true;
  if (!other) v_.other_fingerprint.clear();
  v_.other_publisher = other;
  if (!other_fp.empty()) v_.other_fingerprint = other_fp;
  v_.asking_name = asking_name;
  v_.asking_key = asking_key;
  v_.asking_fingerprint = asking_fp;
}

void link::prepare_nearby() {
  std::uint64_t generation = 0, epoch = 1;
  std::string library_id, destination, gen_dir;
  bool any = false;
  {
    std::lock_guard lock(m_);
    generation = std::max(pub_.waiting, pub_.generation);
    epoch = pub_.epoch;
    library_id = pub_.library_id;
    destination = pub_.destination;
    gen_dir = pub_.gen_dir;
    // Handed over: this desktop's key signs nothing a phone should take now.
    any = v_.claimed && !v_.devices.empty() && !v_.other_publisher;
    // A generation Local search built under another library's id would be
    // refused by every phone: not served, and said. (An older epoch is the
    // moment after a key move, before its next publish: signed here under the
    // library's epoch, as before the move.)
    if (!pub_.source_library_id.empty() && pub_.source_library_id != pub_.library_id) {
      any = false;
      v_.mismatch = true;
    }
    if (!any && v_.nearby) v_.nearby = false;
  }
  if (!any) {
    (void)route("nearby_stop", "{}");
    return;
  }
  // Nothing built here (no Local search, or nothing indexed): nothing to hand
  // a phone nearby; it lists the NAS's folders instead.
  if (generation == 0 || gen_dir.empty()) return;
  auto secret = signing_key(false);
  if (!secret) return;
  const std::string dir = o_.data_dir + "/nearby";
  catalog::nearby_options n;
  n.dir = dir;
  n.gen_dir = gen_dir;
  n.library_id = library_id;
  n.epoch = epoch;
  n.generation = generation;
  n.secret_key = *secret;
  n.public_key = catalog::public_of(*secret);
  n.shared = catalog::read_shared_list(gen_dir + ".shared.json");
  if (!destination.empty()) n.library_json = slurp(destination + "/.mediaviewer/library.json");
  std::error_code ec;
  if (!std::filesystem::is_directory(fs_path(gen_dir), ec)) return;
  if (const expected made = catalog::prepare_nearby(n); !made) return;
  json::writer w;
  w.begin_object().key("folder").string(dir).key("library_id").string(library_id).end_object();
  const bool up = route("nearby", w.str()).has_value();
  std::lock_guard lock(m_);
  v_.nearby = up;
  v_.prepared = generation;
}

void link::choose_folder(const std::string& path) {
  const auto fail = [this](const char* why) {
    std::lock_guard lock(m_);
    v_.error = why;
  };
  if (path.empty()) return fail("no_folder");
  auto secret = signing_key(true);
  if (!secret) return fail("no_key_store");
  const auto pub = catalog::public_of(*secret);
  std::string library_id;
  std::uint64_t epoch = 1;
  {
    std::lock_guard lock(m_);
    library_id = pub_.library_id;
    epoch = pub_.epoch;
  }
  // The share's own library.json decides: this key's is taken as it is
  // (another computer of the person's moved here, or a restore); another
  // key's is shown by refresh (hand-over or start over); none is started now,
  // so the library can be claimed and paired before anything is indexed.
  auto lib = catalog::read_library(path);
  if (lib && lib->public_key == catalog::to_hex(pub)) {
    library_id = lib->library_id;
    epoch = lib->epoch;
  } else if (!lib && lib.error() == status::not_found) {
    if (library_id.empty()) library_id = catalog::new_library_id();
    if (const expected started = catalog::start_library(path, library_id, pub); !started) {
      return fail(started.error() == status::permission_denied ? "other_library" : "unreachable");
    }
  } else if (!lib) {
    return fail("unreachable");  // there and unreadable: never written over
  }
  {
    std::lock_guard lock(m_);
    pub_.destination = path;
    if (!lib || lib->public_key == catalog::to_hex(pub)) {
      pub_.library_id = library_id;
      pub_.epoch = epoch;
    }
  }
  save_state();
  // Local search, when it is here, publishes there now (its first build).
  {
    std::lock_guard lock(source_m_);
    if (source_) (void)source_.publish(path);
  }
  read_source();
  refresh();
}

void link::run(const std::string& request) {
  const auto doc = json::parse(request, 4);
  const std::string what = *doc->str("do");
  const auto text = [&](const char* k) -> std::string {
    const std::string* v = doc->str(k);
    return v ? *v : std::string();
  };
  const auto fail = [this](const char* why) {
    std::lock_guard lock(m_);
    v_.error = why;
  };
  // A refusal, named by the NAS's reason when it gave one (a passphrase wanted,
  // wrong, or locked out) rather than the bare status.
  const auto fail_status = [this, &fail](status e) {
    if (e == status::permission_denied) {
      if (auto r = route("last_refusal", "{}")) {
        const auto d = json::parse(*r, 2);
        const std::string* why = d ? d->str("why") : nullptr;
        if (const char* named = why ? refusal_name(*why) : nullptr) {
          std::lock_guard lock(m_);
          v_.error = named;
          if (const auto until = d->integer("until")) v_.locked_until = *until;
          return;
        }
      }
    }
    fail(error_name(e));
  };
  const auto clear_qr = [this] {
    std::lock_guard lock(m_);
    v_.qr_url.clear();
    v_.qr_library.clear();
    v_.qr_fingerprint.clear();
    v_.qr_expires = 0;
    v_.qr_for_s = 0;
  };
  std::string destination;
  {
    std::lock_guard lock(m_);
    destination = pub_.destination;
  }
  if (destination.empty() && what != "folder" && what != "source") {
    // A folder Local search published to before this knew one (before the
    // move, or between ticks) is this library's folder too.
    read_source();
    adopt_from_source();
    std::lock_guard lock(m_);
    destination = pub_.destination;
  }

  if (what == "poll") {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    std::int64_t expires = 0;
    {
      std::lock_guard lock(m_);
      expires = v_.qr_expires;
    }
    if (expires != 0 && now >= expires) {
      clear_qr();
      (void)route("pair_stop", "{}");
      (void)route("release", "{}");
      return;
    }
    refresh(true);
    std::lock_guard lock(m_);
    if (!v_.code_waiting && v_.qr_expires != 0) {
      // Used by a phone already paired (a re-scan), or gone from the service:
      // a code it would not take again does not stay on the card.
      bool any_new = false;
      for (const auto& d : v_.devices) {
        any_new = any_new || std::find(before_.begin(), before_.end(), d.key) == before_.end();
      }
      if (!any_new) {
        v_.qr_url.clear();
        v_.qr_library.clear();
        v_.qr_fingerprint.clear();
        v_.qr_expires = 0;
        v_.qr_for_s = 0;
        return;
      }
    }
    bool landed = false;
    for (const auto& d : v_.devices) {
      if (std::find(before_.begin(), before_.end(), d.key) == before_.end()) {
        v_.added.push_back(d.name);
        before_.push_back(d.key);
        landed = true;
      }
    }
    if (landed) {
      // The code is spent; the next phone gets a new one.
      v_.qr_url.clear();
      v_.qr_library.clear();
      v_.qr_fingerprint.clear();
      v_.qr_expires = 0;
      v_.qr_for_s = 0;
      // The new phone can be served nearby from now on, and the session
      // kept open for this code is let go after it.
      requests_.push_back("{\"do\":\"nearby\"}");
    }
    return;
  }
  if (what == "source") {
    read_source();
    adopt_from_source();
    std::lock_guard lock(m_);
    // Nothing set up: nothing on the NAS or nearby to follow (no desk is woken).
    if (pub_.destination.empty()) return;
    requests_.push_back("{\"do\":\"nearby\"}");
    return;
  }
  if (what == "refresh") {
    read_source();
    adopt_from_source();
    refresh();
    prepare_nearby();
    return;
  }
  if (what == "nearby") {
    prepare_nearby();
    return;
  }
  if (what == "folder") {
    choose_folder(text("path"));
    return;
  }
  if (what == "publish") {
    if (destination.empty()) return fail("no_folder");
    expected started = err(status::not_found);
    {
      std::lock_guard lock(source_m_);
      if (source_) started = source_.publish(destination);
    }
    if (!started && started.error() == status::not_found) return fail("no_local_search");
    // Busy: one is running, and the card shows it. Anything else is said.
    if (!started && started.error() != status::busy) return fail("failed");
    read_source();
    return;
  }
  if (what == "cancel") {
    std::lock_guard lock(source_m_);
    if (source_) source_.cancel();
    return;
  }
  if (what == "claim") {
    if (destination.empty()) return fail("no_folder");
    json::writer w;
    w.begin_object().key("share").string(destination).key("local").string(text("local")).key("public").string(text("public"));
    if (const std::string pass = text("passphrase"); !pass.empty()) {
      w.key("passphrase").string(pass).key("new_passphrase").boolean(doc->boolean("new_passphrase").value_or(false));
    }
    w.end_object();
    auto r = route("claim", w.str());
    // Timed out: the service may have taken it and the answer been lost. A
    // second claim from this desktop is answered with the same keys.
    if (!r && r.error() == status::timeout) r = route("claim", w.str());
    if (!r && r.error() != status::timeout) return fail_status(r.error());
    // A slow claim may still have gone through: the service says which.
    refresh();
    if (!r) {
      std::lock_guard lock(m_);
      if (!v_.claimed) v_.error = "unreachable";
    }
    return;
  }
  if (what == "pair") {
    std::string library_id;
    {
      std::lock_guard lock(m_);
      library_id = pub_.library_id;
    }
    if (destination.empty()) return fail("no_folder");
    if (library_id.empty()) return fail("not_published");
    auto secret = signing_key(false);
    if (!secret) return fail("not_published");
    const auto publisher = catalog::public_of(*secret);
    const std::int64_t for_s = std::max<std::int64_t>(0, doc->integer("for_s").value_or(0));
    json::writer w;
    w.begin_object()
        .key("ttl_ms").integer(static_cast<std::int64_t>(o_.code_ttl_s * 1000))
        .key("library_id").string(library_id)
        .key("publisher_key").string(catalog::to_hex(publisher));
    if (for_s > 0) w.key("for_s").integer(for_s);
    w.end_object();
    auto answer = route("pair", w.str());
    if (!answer) return fail_status(answer.error());
    const auto a = json::parse(*answer, 4);
    if (!a) return fail("failed");
    catalog::pair_payload p;
    p.library_id = library_id;
    p.library_name = last_part(destination);
    p.publisher_key = publisher;
    const auto field = [&](const char* k) -> std::string {
      const std::string* v = a->str(k);
      return v ? *v : std::string();
    };
    p.service_local = field("local");
    p.service_public = field("public");
    p.service_identity_hex = field("identity");
    p.access_key_hex = field("access_key");
    p.code = field("code");
    p.expires = a->integer("expires").value_or(0);
    p.grant_for_s = for_s;
    const std::string url = catalog::pair_url(p);
    refresh();  // who is paired before this code
    std::lock_guard lock(m_);
    before_.clear();
    for (const auto& d : v_.devices) before_.push_back(d.key);
    v_.added.clear();
    v_.qr_url = url;
    v_.qr_library = p.library_name;
    v_.qr_fingerprint = field("check");  // pairing_check: the phone computes the same
    if (v_.qr_fingerprint.empty()) {
      // A desk too old to say it: no code to compare is no code to show.
      v_.qr_url.clear();
      v_.qr_expires = 0;
      v_.qr_for_s = 0;
      v_.error = "no_driftwire";
      return;
    }
    v_.qr_expires = p.expires;
    v_.qr_for_s = for_s;
    // A new code is the one on the service now: nothing left from before to withdraw.
    if (v_.code_left) {
      v_.code_left = false;
      v_.flags_dirty = true;  // saved outside the lock
    }
    v_.code_waiting = true;
    return;
  }
  if (what == "pair_stop") {
    clear_qr();
    (void)route("pair_stop", "{}");  // a photo of it pairs nothing now
    (void)route("release", "{}");
    return;
  }
  if (what == "revoke") {
    json::writer w;
    w.begin_object().key("key").string(text("key")).end_object();
    auto r = route("revoke", w.str());
    if (!r) return fail_status(r.error());
    const auto a = json::parse(*r, 4);
    if (a && !a->boolean("nas_told").value_or(true)) fail("revoke_waits");
    refresh();
    prepare_nearby();
    return;
  }
  if (what == "add_folder" || what == "remove_folder") {
    json::writer w;
    w.begin_object();
    if (what == "add_folder") {
      w.key("label").string(text("label")).key("path").string(text("path"));
    } else {
      w.key("id").string(text("id"));
    }
    w.end_object();
    if (auto r = route(what == "add_folder" ? "add_root" : "remove_root", w.str()); !r) return fail_status(r.error());
    refresh();
    return;
  }
  // ---- the owner's passphrase and timed phones ----
  if (what == "unlock" || what == "set_passphrase") {
    json::writer w;
    w.begin_object().key("passphrase").string(text("passphrase")).end_object();
    if (auto r = route(what.c_str(), w.str()); !r) return fail_status(r.error());
    {
      std::lock_guard lock(m_);
      v_.error.clear();
    }
    refresh();
    return;
  }
  if (what == "lock") {
    (void)route("lock", "{}");
    refresh();
    return;
  }
  if (what == "grant") {
    json::writer w;
    w.begin_object().key("key").string(text("key")).key("for_s").integer(std::max<std::int64_t>(0, doc->integer("for_s").value_or(0))).end_object();
    if (auto r = route("grant", w.str()); !r) return fail_status(r.error());
    refresh();
    prepare_nearby();
    return;
  }
  if (what == "set_addresses") {
    json::writer l, p, c;
    l.begin_object().key("local").string(text("local")).end_object();
    if (!text("local").empty()) {
      if (auto r = route("set_local", l.str()); !r) return fail(error_name(r.error()));
    }
    p.begin_object().key("public").string(text("public")).end_object();
    if (auto r = route("set_public", p.str()); !r) return fail(error_name(r.error()));
    std::string warning;
    if (!text("public").empty()) {
      c.begin_object().key("address").string(text("public")).end_object();
      if (auto r = route("check_address", c.str())) {
        const auto a = json::parse(*r, 4);
        if (const std::string* v = a ? a->str("warning") : nullptr) warning = *v;
      }
    }
    {
      std::lock_guard lock(m_);
      v_.warning = warning;
    }
    refresh();
    return;
  }
  if (what == "takeover_ask" || what == "takeover_restart") {
    if (destination.empty()) return fail("no_folder");
    auto secret = signing_key(true);
    if (!secret) return fail("no_key_store");
    const auto pub = catalog::public_of(*secret);
    if (what == "takeover_ask") {
      std::string machine = o_.machine_name;
      if (auto s = route("status", "{}")) {
        const auto d = json::parse(*s, 4);
        if (const std::string* v = d ? d->str("machine") : nullptr; v && !v->empty()) machine = *v;
      }
      if (const expected r = catalog::request_takeover(destination, pub, machine); !r) return fail("unreachable");
      return;
    }
    if (auto r = catalog::restart_publisher(destination, pub); !r) return fail("unreachable");
    refresh();
    return;
  }
  if (what == "takeover_allow") {
    if (destination.empty()) return fail("no_folder");
    auto request_seen = catalog::read_takeover(destination);
    if (!request_seen || request_seen->public_key != text("key")) return fail("failed");
    auto secret = signing_key(false);
    if (!secret) return fail("no_key_store");
    const auto to = key_bytes(request_seen->public_key);
    if (to.empty()) return fail("failed");
    auto moved = catalog::hand_over(destination, *secret, to);
    if (!moved) return fail(moved.error() == status::permission_denied ? "refused" : "unreachable");
    if (o_.after_hand_over) o_.after_hand_over();
    {
      // What hand_over wrote is the truth now, whether or not the share reads
      // back on the next try: this desktop is no longer the publisher.
      std::lock_guard lock(m_);
      v_.other_publisher = true;
      v_.handed_over = true;
      v_.other_fingerprint = catalog::fingerprint(to);
    }
    save_flags();
    prepare_nearby();  // its key is not the library's now: the nearby copy goes
    refresh();
    return;
  }
  fail("failed");
}

std::string link::state_json() const {
  json::writer w;
  std::lock_guard lock(m_);
  w.begin_object();
  w.key("driftwire").boolean(v_.driftwire);
  w.key("busy").string(v_.busy);
  w.key("error").string(v_.error);
  w.key("folder").string(pub_.destination);
  w.key("library_name").string(pub_.destination.empty() ? std::string() : last_part(pub_.destination));
  w.key("local_search").boolean(pub_.source);
  // Local search's own answer (phone_json), as it gave it: the catalogue rows.
  if (!pub_.source_json.empty()) w.key("catalogue").raw(pub_.source_json);
  w.key("claimed").boolean(v_.claimed);
  w.key("reachable").boolean(v_.reachable);
  w.key("local").string(v_.local);
  w.key("public").string(v_.public_name);
  w.key("warning").string(v_.warning);
  w.key("devices").begin_array();
  for (const auto& d : v_.devices) {
    w.begin_object().key("name").string(d.name).key("key").string(d.key);
    // A timed or scoped phone: its end (unix, the NAS's clock; 0 none) and whether it has passed.
    w.key("limited").boolean(d.limited).key("expires").integer(d.expires).key("expired").boolean(d.expired);
    w.key("guest").boolean(d.guest).key("scoped").boolean(d.scoped);
    w.end_object();
  }
  w.end_array();
  // The NAS owner's side: its passphrase ("set", "none", "broken"; "" from a NAS
  // too old to say), what it can do, this desktop's unlock and any lockout.
  w.key("nas_passphrase").string(v_.nas_passphrase);
  w.key("nas_features").begin_array();
  for (const auto& f : v_.nas_features) w.string(f);
  w.end_array();
  w.key("unlocked_s").integer(v_.unlocked_s);
  w.key("locked_until").integer(v_.locked_until);
  w.key("qr_for_s").integer(v_.qr_for_s);
  w.key("folders").begin_array();
  for (const auto& f : v_.folders) w.begin_object().key("id").string(f.id).key("label").string(f.label).key("path").string(f.path).end_object();
  w.end_array();
  w.key("qr").string(v_.qr_url);
  w.key("qr_library").string(v_.qr_library);
  w.key("qr_fingerprint").string(v_.qr_fingerprint);
  w.key("qr_expires").integer(v_.qr_expires);
  w.key("added").begin_array();
  for (const auto& n : v_.added) w.string(n);
  w.end_array();
  w.key("nearby").boolean(v_.nearby);
  w.key("other_publisher").boolean(v_.other_publisher);
  w.key("mismatch").boolean(v_.mismatch);
  w.key("flags_unsaved").boolean(v_.flags_unsaved);
  w.key("other_fingerprint").string(v_.other_fingerprint);
  w.key("asking_name").string(v_.asking_name);
  w.key("asking_key").string(v_.asking_key);
  w.key("asking_fingerprint").string(v_.asking_fingerprint);
  w.end_object();
  return w.take();
}

}  // namespace mv::phone
