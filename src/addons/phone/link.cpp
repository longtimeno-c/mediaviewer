// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone access in the app (link.h). Ported from Local search's
// engine_phone_link.cpp (2026-11-12) with the same requests, states and
// failure rules; what changed is who owns what. The library's folder, id and
// epoch are this feature's (state.json), the desk is called directly, and
// Local search, when loaded, is a source of generations followed here.
#include "addons/phone/link.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
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
#include "wire/serve_wire.h"

namespace mv::phone {

namespace catalog = mv::ai::catalog;
namespace serve = mv::driftwire::serve;

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

// A secret's text zeroed before it is let go (the relay line holds a key).
void wipe_string(std::string& s) {
  std::fill(s.begin(), s.end(), '\0');
  s.clear();
}

// The program's first line, as it said it (the card shows why a step failed).
std::string first_line(const std::string& out) {
  std::string l = out.substr(0, out.find('\n'));
  static constexpr std::string_view kLead = "driftwire host: ";
  if (l.rfind(kLead, 0) == 0) l.erase(0, kLead.size());
  while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
  if (l.size() > 300) {
    std::size_t at = 300;
    while (at > 0 && (static_cast<unsigned char>(l[at]) & 0xC0) == 0x80) --at;  // never half a character
    l.resize(at);
  }
  return l;
}

// The program's last line (`host drives` says what it made, or why not, last).
std::string last_line(const std::string& out) {
  std::string s = out;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  const auto nl = s.rfind('\n');
  std::string l = first_line(nl == std::string::npos ? s : s.substr(nl + 1));
  static constexpr std::string_view kLead = "driftwire host drives: ";
  if (l.rfind(kLead, 0) == 0) l.erase(0, kLead.size());
  return l;
}

// A JSON document the program printed, trimmed ("" when it is not one).
std::string json_out(const std::string& out, int depth) {
  std::string s = out;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return json::parse(s, depth) ? s : std::string();
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
  if (why == "passphrase-short") return "passphrase_short";
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

namespace {

// Whether the NAS path of a backup folder ("/volume1/photo/Phone backup") is under the
// computer's Local search root `local` ("/Volumes/photo", "/Volumes/photo/Phone backup",
// "\\nas\photo"): the share is the folder right under /volumeN on a Synology, mounted under
// its own name, compared without case. A heuristic for the card's wording; it never changes
// what is indexed.
bool nas_covered_by(const std::string& nas, const std::string& local) {
  const auto split = [](const std::string& p) {
    std::vector<std::string> out;
    std::string part;
    for (char ch : p + "/") {
      if (ch == '/' || ch == '\\') {
        if (!part.empty()) out.push_back(part);
        part.clear();
      } else {
        part += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
      }
    }
    return out;
  };
  std::vector<std::string> n = split(nas);
  const std::vector<std::string> l = split(local);
  if (n.size() >= 2 && n[0].rfind("volume", 0) == 0) n.erase(n.begin());  // Synology /volumeN/share/...
  else if (n.size() >= 3 && n[0] == "mnt") n.erase(n.begin(), n.begin() + 2);  // TrueNAS /mnt/<pool>/share/...
  if (n.empty()) return false;
  // Where the share is mounted: right under /Volumes on a Mac, the second part of a
  // \\host\share path on Windows. Anywhere else (a local ~/Pictures/photo) is not it.
  const bool unc = local.rfind("\\\\", 0) == 0 || local.rfind("//", 0) == 0;
  std::size_t at = 0;
  if (l.size() >= 2 && l[0] == "volumes") {
    at = 1;
  } else if (unc && l.size() >= 2) {
    at = 1;
  } else {
    return false;
  }
  if (l[at] != n[0] && l[at].rfind(n[0] + "-", 0) != 0) return false;  // "photo-1": a second mount of it
  const std::size_t rest = l.size() - at;
  if (rest > n.size()) return false;
  return std::equal(l.begin() + static_cast<std::ptrdiff_t>(at) + 1, l.end(), n.begin() + 1);
}

}  // namespace

bool link::indexed_here(const std::string& nas_path) const {
  std::string text;
  {
    std::lock_guard lock(m_);
    text = pub_.source_json;
  }
  const auto doc = json::parse(text, 4);
  const json::value* roots = doc ? doc->find("search_roots") : nullptr;
  if (!roots || roots->k != json::kind::array) return false;
  for (const auto& r : roots->a) {
    if (r.k == json::kind::string && nas_covered_by(nas_path, r.s)) return true;
  }
  return false;
}

void link::read_source() {
  std::string text;
  bool any = false;
  {
    std::lock_guard lock(source_m_);
    any = static_cast<bool>(source_);
    if (any) text = source_.json();
  }
  // The add-on's source asks the host (catalogue_json): "" while Local search
  // is not loaded, which is no source at all.
  if (text.empty()) any = false;
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
  bool host_known = false;
  {
    std::lock_guard lock(m_);
    host_known = v_.host_read && v_.host.set_up;
  }
  if (host_known) read_host();  // three small files and a moment's lock: the status row
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
          r.backup = v.boolean("backup").value_or(!r.guest);
          r.desktop = v.boolean("desktop").value_or(false);
          if (const std::string* sh = v.str("share")) r.share = *sh;
          if (const std::string* pe = v.str("person")) r.person = *pe;
          rows.push_back(std::move(r));
        }
      }
      // Whom they belong to (§13): a service from before people answers nothing, so none.
      std::vector<person_row> folk;
      if (auto pe = route("people", "{}")) {
        const auto pdoc = json::parse(*pe, 5);
        if (const json::value* pl = pdoc ? pdoc->find("people") : nullptr; pl && pl->k == json::kind::array) {
          for (const json::value& v : pl->a) {
            const std::string* name = v.str("name");
            const std::string* role = v.str("role");
            if (name && role) folk.push_back({*name, *role});
          }
        }
      }
      std::lock_guard lock(m_);
      v_.people = std::move(folk);
      v_.devices = std::move(rows);
      v_.reachable = doc && doc->boolean("reachable").value_or(false);
      v_.code_waiting = doc && doc->boolean("code_waiting").value_or(false);
      v_.shares_leaving = doc ? doc->integer("shares_leaving").value_or(0) : 0;
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
        const std::string* relay = doc->str("relay");
        v_.relay = relay ? *relay : std::string();
        v_.nas_vpn_port = doc->integer("vpn_port").value_or(0);
        v_.nas_vpn_running = doc->boolean("vpn_running").value_or(false);
        v_.unlocked_s = doc->integer("unlocked_s").value_or(0);
        v_.unlocked_until = std::chrono::steady_clock::now() + std::chrono::seconds(std::max<std::int64_t>(0, v_.unlocked_s));
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
          if (!id || !label || !path) continue;
          folder_row row{*id, *label, *path};
          if (const json::value* b = v.find("backup"); b && b->k == json::kind::object) {
            const auto num = [&](const char* k) { return static_cast<std::uint64_t>(std::max<std::int64_t>(0, b->integer(k).value_or(0))); };
            row.backup = true;
            row.limit = num("limit");
            row.reserve = num("reserve");
            row.used = num("used");
            row.room = num("room");
            row.indexed = indexed_here(*path);
          }
          rows.push_back(std::move(row));
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
    if (e == status::permission_denied || e == status::invalid_arg) {
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
  if (what == "host_status") {
    read_host();
    return;
  }
  if (what == "host_setup") {
    host_setup(request);
    return;
  }
  // ---- this computer's users and its LAN share (self-host-users-smb.md §8) ----
  if (what == "users_refresh") {
    read_users();
    return;
  }
  if (what == "user_set" || what == "user_remove") {
    json::writer w;
    w.begin_object().key("name").string(text("name"));
    if (what == "user_set") {
      w.key("role").string(text("role"));
      if (const json::value* f = doc->find("folders"); f && f->k == json::kind::array) {
        w.key("folders").begin_array();
        for (const auto& e : f->a) {
          if (e.k == json::kind::string) w.string(e.s);
        }
        w.end_array();
      }
    }
    w.end_object();
    if (auto r = route(what.c_str(), w.str()); !r) return fail_status(r.error());
    {
      std::lock_guard lock(m_);
      v_.error.clear();
    }
    // The password goes to the step's stdin and nowhere else.
    std::vector<self_host::user_password> pw;
    if (what == "user_set" && !text("password").empty()) pw.push_back({text("name"), text("password")});
    users_apply(pw);
    for (auto& p : pw) std::fill(p.password.begin(), p.password.end(), '\0');
    return;
  }
  if (what == "users_apply") {
    users_apply({});
    refresh();  // the computers' share accounts it made (self-host-users-smb.md §12)
    return;
  }
  if (what == "host_drives" || what == "host_drives_choices" || what == "host_drives_combine" ||
      what == "host_drives_replace" || what == "host_drives_copy" || what == "host_drives_copy_stop") {
    host_drives(what, request);
    return;
  }
  if (what == "host_convert") {
    // docs/plans/phone-clip-convert.md: on, with this app's clip helper; or off (the cache goes).
    const bool on = doc->boolean("on").value_or(false);
    const auto w = host_where();
    std::vector<std::string> args{"convert", on ? "on" : "off"};
    const std::string helper = self_host::clip_helper();
    if (on && !helper.empty()) args.insert(args.end(), {"--helper", helper});
    const auto r = self_host::run_host(w, args);
    if (!r || r->code != 0) {
      std::lock_guard lock(m_);
      v_.host_said = r ? first_line(r->output) : std::string();
      v_.error = "host_convert";
    }
    read_host();
    return;
  }
  if (what == "host_start") {
    const auto w = host_where();
    if (w.program.empty()) {
      read_host();
      return fail("no_driftwire_program");
    }
    {
      std::lock_guard lock(m_);
      v_.host_step = "start";
      v_.host_said.clear();
    }
    const expected started = start_host_now(w);
    // The OS starts it a moment later: look for a few seconds, not only at the next tick.
    for (int i = 0; started && i < 40 && !stopping_.load(); ++i) {
      const auto st = self_host::read_status(w);
      if (st.running && self_host::answers(st.service_port, 300)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    read_host();
    std::lock_guard lock(m_);
    v_.host_step.clear();
    if (!started) v_.error = "host_start";
    return;
  }
  if (what == "host_stop") {
    const auto w = host_where();
    if (w.program.empty()) {
      read_host();
      return fail("no_driftwire_program");
    }
    {
      std::lock_guard lock(m_);
      v_.host_said.clear();
    }
    const expected stopped = stop_host_now(w);
    read_host();
    std::lock_guard lock(m_);
    if (!stopped) v_.error = "host_stop";
    return;
  }
  if (what == "host_vpn" || what == "host_vpn_line") {
    // The VPN through this computer (docs/design/28 "Self-hosting"): the
    // program's own `host vpn on|off`, the CLI's logic and words; never a
    // second relay or forwarder in the app. `host_vpn_line` asks `vpn on`
    // again for its relay line (nothing else changes: the same host.json).
    const bool on = what == "host_vpn_line" || doc->boolean("on").value_or(false);
    const auto w = host_where();
    if (w.program.empty()) {
      read_host();
      return fail("no_driftwire_program");
    }
    {
      std::lock_guard lock(m_);
      v_.host_said.clear();
      wipe_string(v_.relay_line);
    }
    auto r = self_host::run_host(w, {"vpn", on ? "on" : "off"});
    std::string line = r && r->code == 0 && on ? self_host::relay_line_in(r->output) : std::string();
    std::string said = r ? first_line(r->output) : std::string();
    if (r) wipe_string(r->output);
    // A running host takes host.json within 2 s: look for a few seconds so the
    // switch shows what the relay does, not only what was asked.
    self_host::host_status st;
    for (int i = 0; r && r->code == 0 && i < 20 && !stopping_.load(); ++i) {
      st = self_host::read_status(w);
      if (!st.running || st.vpn_on == on) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // The line names the port state.json had when it was printed: a relay
    // that started again for the VPN (or a router that mapped it anew) has
    // another by now, so the line is asked for once more with the port it has.
    // With the web-site look the line names its port (the code says relay-tls).
    const bool look = line.rfind(serve::kRelayTlsCodePrefix, 0) == 0;
    // A name carries the router's outside port; a LAN address (no name) the one it listens on.
    const bool named = !st.duckdns.empty() || !st.domain.empty();
    const std::uint16_t inside = look ? st.relay_tls_port : st.relay_port;
    const std::uint16_t outside = look ? st.tls_outside_port : st.relay_outside_port;
    const std::uint16_t port = named && outside != 0 ? outside : inside;
    if (!line.empty() && st.vpn_on && port != 0 && line.find(":" + std::to_string(port) + "#") == std::string::npos) {
      wipe_string(line);
      if (auto again = self_host::run_host(w, {"vpn", "on"}); again) {
        if (again->code == 0) line = self_host::relay_line_in(again->output);
        wipe_string(again->output);
      }
    }
    read_host();
    std::lock_guard lock(m_);
    if (!r || r->code != 0) {
      v_.error = "host_vpn";
      v_.host_said = std::move(said);
      return;
    }
    v_.relay_line = std::move(line);
    return;
  }
  if (what == "host_vpn_hide") {
    std::lock_guard lock(m_);
    wipe_string(v_.relay_line);
    return;
  }
  if (what == "host_domain") {
    // The VPN's own domain (docs/plans/vpn-stealth.md §3): the program's own `host name domain`,
    // as Use as VPN runs `host vpn` (no second writer of host.json in the app).
    const std::string domain = text("domain");
    const auto w = host_where();
    if (w.program.empty()) {
      read_host();
      return fail("no_driftwire_program");
    }
    {
      std::lock_guard lock(m_);
      v_.host_said.clear();
      wipe_string(v_.relay_line);  // the code changes with the name: asked for again
    }
    auto r = self_host::run_host(w, {"name", "domain", domain.empty() ? std::string("none") : domain});
    read_host();
    if (!r || r->code != 0) {
      std::lock_guard lock(m_);
      v_.error = "host_domain";
      v_.host_said = r ? first_line(r->output) : std::string();
      return;
    }
    return;
  }
  if (what == "host_name") {
    // This computer's DuckDNS name after setup: checked as setup checks it, kept
    // as setup keeps it (the token in the host's own store, the running host takes
    // the name from host.json), then the address phones dial from outside follows.
    const auto sub = self_host::parse_subdomain(text("duckdns"));
    if (!sub) return fail("host_bad_name");
    if (!self_host::parse_token(text("duckdns_token"))) return fail("host_bad_token");
    const auto w = host_where();
    if (w.program.empty()) {
      read_host();
      return fail("no_driftwire_program");
    }
    if (!self_host::set_duckdns(w, *sub, text("duckdns_token"))) return fail("host_name");
    const auto st = self_host::read_status(w);
    read_host();
    std::lock_guard lock(m_);
    if (v_.claimed) {
      const std::uint16_t outside = st.outside_port != 0 ? st.outside_port : st.service_port;
      json::writer a;
      a.begin_object().key("do").string("set_addresses").key("local").string("");
      a.key("public").string(self_host::public_route(*sub, outside)).end_object();
      requests_.push_back(a.take());
    }
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
    } else if (const auto a = json::parse(*r, 2); a && a->boolean("passphrase_set") == false) {
      // Claimed, but the NAS is too old to keep a passphrase: nothing protects it yet.
      std::lock_guard lock(m_);
      v_.error = "nas_too_old";
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
    const auto signing_public = catalog::public_of(*secret);
    const std::int64_t for_s = std::max<std::int64_t>(0, doc->integer("for_s").value_or(0));
    json::writer w;
    w.begin_object()
        .key("ttl_ms").integer(static_cast<std::int64_t>(o_.code_ttl_s * 1000))
        .key("library_id").string(library_id)
        .key("publisher_key").string(catalog::to_hex(signing_public));
    if (for_s > 0) w.key("for_s").integer(for_s);
    w.end_object();
    auto answer = route("pair", w.str());
    if (!answer) return fail_status(answer.error());
    const auto a = json::parse(*answer, 4);
    if (!a) return fail("failed");
    catalog::pair_payload p;
    p.library_id = library_id;
    p.library_name = last_part(destination);
    p.publisher_key = signing_public;
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
    {
      std::lock_guard lock(m_);
      p.relay = v_.relay;  // the phone tries it after the direct addresses
    }
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
  if (what == "add_folder" || what == "remove_folder" || what == "set_backup") {
    // Sizes come from the card as decimal byte counts (strings, as the card's fields are).
    const auto bytes = [&](const char* k) -> std::int64_t {
      const std::string t = text(k);
      std::int64_t v = 0;
      for (char ch : t) {
        if (ch < '0' || ch > '9' || v > (INT64_MAX - 9) / 10) return -1;
        v = v * 10 + (ch - '0');
      }
      return t.empty() ? -1 : v;
    };
    json::writer w;
    w.begin_object();
    if (what == "add_folder") {
      w.key("label").string(text("label")).key("path").string(text("path"));
    } else {
      w.key("id").string(text("id"));
    }
    if (what == "set_backup" || text("backup") == "1") {
      // "none": no limit of its own, only the reserve (serve_wire.h kNoLimit).
      const auto limit = text("limit") == "none" ? static_cast<std::int64_t>(driftwire::serve::kNoLimit) : bytes("limit");
      const auto reserve = bytes("reserve");
      if (limit <= 0 || reserve < 0) return fail("invalid_arg");
      if (what == "add_folder") w.key("backup").boolean(true);
      w.key("limit").integer(limit).key("reserve").integer(reserve);
    }
    w.end_object();
    const char* verb = what == "add_folder" ? "add_root" : what == "remove_folder" ? "remove_root" : "set_backup";
    if (auto r = route(verb, w.str()); !r) return fail_status(r.error());
    refresh();
    return;
  }
  if (what == "person_set" || what == "person_remove" || what == "device_person") {
    json::writer w;
    w.begin_object();
    for (const char* k : {"name", "role", "key", "person"}) {
      if (doc->str(k)) w.key(k).string(text(k));
    }
    w.end_object();
    if (auto r = route(what.c_str(), w.str()); !r) return fail_status(r.error());
    refresh();
    return;
  }
  if (what == "rename") {
    json::writer w;
    w.begin_object().key("key").string(text("key")).key("name").string(text("name")).end_object();
    if (auto r = route("rename", w.str()); !r) return fail_status(r.error());
    refresh();
    return;
  }
  if (what == "phone_backup") {
    json::writer w;
    w.begin_object().key("key").string(text("key")).key("on").boolean(doc->boolean("on").value_or(false)).end_object();
    if (auto r = route("phone_backup", w.str()); !r) return fail_status(r.error());
    refresh();
    prepare_nearby();
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
  if (what == "nas_vpn") {
    // The NAS as a VPN host (docs/design/28): the service's own relay, turned
    // on or off by its owner. On answers the port and the key once: the relay
    // line is the NAS's own name (its address from outside, else on the
    // network) with that port, shown on the card as `host vpn on`'s is.
    const bool on = doc->boolean("on").value_or(false);
    json::writer w;
    w.begin_object().key("on").boolean(on).end_object();
    {
      std::lock_guard lock(m_);
      wipe_string(v_.relay_line);
    }
    auto r = route("set_vpn", w.str());
    if (!r) return fail_status(r.error());
    const auto doc2 = json::parse(*r, 2);
    wipe_string(*r);
    std::string line;
    if (doc2 && on) {
      const std::string* key = doc2->str("key");
      const auto port = doc2->integer("port").value_or(0);
      std::string where;
      {
        std::lock_guard lock(m_);
        where = !v_.public_name.empty() ? v_.public_name : v_.local;
      }
      std::string host;
      std::uint16_t ignored = 0;
      // With the web-site look, its port and its code (relay_line holds the code then).
      const auto tls_port = doc2->integer("tls_port").value_or(0);
      if (key && port > 0 && serve::parse_relay_address(where, host, ignored)) {
        const std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
        line = h + ":" + std::to_string(tls_port > 0 && tls_port <= 65535 ? tls_port : port) + "#" + *key;
        if (tls_port > 0 && tls_port <= 65535) line = serve::relay_connect_code(line, true);
      }
    }
    refresh();
    std::lock_guard lock(m_);
    v_.relay_line = std::move(line);
    return;
  }
  if (what == "set_relay") {
    // The person's own relay (docs/plans/nas-relay.md); the key goes to the
    // service, kept nowhere here. "" for the address clears it.
    json::writer w;
    w.begin_object().key("address").string(text("address")).key("key").string(text("key")).end_object();
    if (auto r = route("set_relay", w.str()); !r) return fail_status(r.error());
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

// ---- this computer as the server (docs/plans/self-hosted.md slice 6) ------------------------

self_host::where link::host_where() const { return o_.self_host ? o_.self_host() : self_host::find(); }

void link::read_users() {
  std::vector<view::user_row> rows;
  bool read = false;
  if (auto r = route("users", "{}")) {
    const auto doc = json::parse(*r, 5);
    if (const json::value* list = doc ? doc->find("users") : nullptr; list && list->k == json::kind::array) {
      read = true;
      for (const auto& e : list->a) {
        view::user_row u;
        const std::string* n = e.str("name");
        const std::string* ro = e.str("role");
        if (!n || !ro) continue;
        u.name = *n;
        u.role = *ro;
        u.applied = e.integer("applied").value_or(0) != 0;
        if (const json::value* f = e.find("folders"); f && f->k == json::kind::array) {
          std::vector<std::string> labels;
          for (const auto& x : f->a) {
            if (x.k == json::kind::string) labels.push_back(x.s);
          }
          u.folders = std::move(labels);
        }
        rows.push_back(std::move(u));
      }
    }
  }
  auto addresses = self_host::share_addresses(host_where());
  std::lock_guard lock(m_);
  if (read) v_.users = std::move(rows);
  v_.users_read = v_.users_read || read;
  v_.share_addresses = std::move(addresses);
}

void link::users_apply(std::span<const self_host::user_password> passwords) {
  const auto w = host_where();
  if (w.program.empty()) {
    read_users();
    std::lock_guard lock(m_);
    v_.error = "no_driftwire_program";
    return;
  }
  {
    std::lock_guard lock(m_);
    v_.host_step = "users";
    v_.host_said.clear();
  }
  auto lines = self_host::apply_lines(passwords);
  // The OS asks for its administrator here (the dialog, or UAC): no time limit worth cutting it at.
  auto r = self_host::run_host(w, {"users", "apply", "--input-stdin"}, lines, 600000);
  for (auto& l : lines) std::fill(l.begin(), l.end(), '\0');
  read_users();
  std::lock_guard lock(m_);
  v_.host_step.clear();
  if (!r) {
    v_.error = "host_users";
  } else if (r->code != 0) {
    v_.error = "host_users";
    v_.host_said = r->output;
  }
}

void link::read_host() {
  const auto w = host_where();
  auto st = self_host::read_status(w);
  // The host converts with this app's clip helper: told once per run of the app, and again when
  // the app has moved (an update to another place). Never when its owner turned it off.
  if (st.set_up && st.convert_on && !told_helper_.exchange(true)) {
    const std::string helper = self_host::clip_helper();
    if (!helper.empty() && helper != st.convert_helper) {
      (void)self_host::run_host(w, {"convert", "on", "--helper", helper});
      st = self_host::read_status(w);
    }
  }
  std::lock_guard lock(m_);
  v_.host = std::move(st);
  v_.host_read = true;
}

expected link::start_host_now(const self_host::where& w) {
  if (o_.start_host) return o_.start_host(w);
  // Registers the entry and starts it now (launchctl bootstrap, schtasks /Run,
  // systemctl --user enable --now): the host is the OS's to run, never the app's child.
  // Task Scheduler may take a minute for each of /Create and /Run: never cut it short.
  auto r = self_host::run_host(w, {"start-at-login", "on"}, {}, 150000);
  if (r && r->code == 0) return {};
  std::lock_guard lock(m_);
  v_.host_said = r ? first_line(r->output) : std::string();
  return err(r ? status::io : r.error());
}

expected link::stop_host_now(const self_host::where& w) {
  const auto said = [this](const result<self_host::ran>& r) -> expected {
    std::lock_guard lock(m_);
    v_.host_said = r ? first_line(r->output) : std::string();
    return err(r ? status::io : r.error());
  };
  // The login entry first: a host that ends would otherwise be started again
  // (Windows tries every 5 minutes, launchd keeps it alive). On Windows and
  // Linux that ends the running host too; `host quit` covers the Mac and waits
  // up to 10 s for it (nothing to do when it has gone).
  if (o_.login_off) {
    if (expected off = o_.login_off(w); !off) return off;
  } else if (auto off = self_host::run_host(w, {"start-at-login", "off"}, {}, 150000); !off || off->code != 0) {
    return said(off);
  }
  auto quit = self_host::run_host(w, {"quit"}, {}, 30000);
  if (!quit || quit->code != 0) return said(quit);
  return {};
}

void link::host_drives(const std::string& what, const std::string& request) {
  const auto doc = json::parse(request, 4);
  const auto text = [&](const char* k) -> std::string {
    const std::string* v = doc ? doc->str(k) : nullptr;
    return v ? *v : std::string();
  };
  const auto list = [&](const char* k) {
    std::vector<std::string> out;
    if (const json::value* a = doc ? doc->find(k) : nullptr; a && a->k == json::kind::array) {
      for (const auto& e : a->a) {
        if (e.k == json::kind::string) out.push_back(e.s);
      }
    }
    return out;
  };
  const auto w = host_where();
  const auto fail = [this](const char* why, std::string said = {}) {
    std::lock_guard lock(m_);
    v_.error = why;
    v_.host_step.clear();
    v_.host_said = std::move(said);
  };
  if (w.program.empty()) return fail("no_driftwire_program");
  // A drive id is what the program listed ("disk4", "2"): letters and digits only, never an option.
  const auto good_id = [](const std::string& id) {
    return !id.empty() && id.size() <= 32 &&
           std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
  };
  const std::vector<std::string> ids = list("ids");
  for (const auto& id : ids) {
    if (!good_id(id)) return fail("host_drives");
  }
  if (what == "host_drives") {
    auto r = self_host::run_host(w, {"drives", "list", "--json"}, {}, 60000);
    std::lock_guard lock(m_);
    v_.drives_json = r ? json_out(r->output, 16) : std::string();
    if (v_.drives_json.empty()) v_.error = "host_drives";
    return;
  }
  if (what == "host_drives_choices") {
    std::vector<std::string> args = {"drives", "choices"};
    args.insert(args.end(), ids.begin(), ids.end());
    args.push_back("--json");
    auto r = self_host::run_host(w, args, {}, 60000);
    std::lock_guard lock(m_);
    v_.drives_choices_json = r && r->code == 0 ? json_out(r->output, 8) : std::string();
    if (v_.drives_choices_json.empty()) v_.error = "host_drives";
    return;
  }
  if (what == "host_drives_copy" || what == "host_drives_copy_stop") {
    // Keep a copy: nothing is erased, so nothing is typed and no administrator is asked.
    std::vector<std::string> args;
    if (what == "host_drives_copy") {
      if (ids.size() != 2) return fail("host_drives");
      args = {"drives", "copy", ids[0], ids[1]};
    } else {
      const std::string label = text("label");
      if (label.empty() || label.size() > 64 || label.front() == '-') return fail("host_drives");
      args = {"drives", "copy-stop", label};
    }
    {
      std::lock_guard lock(m_);
      v_.host_said.clear();
    }
    auto r = self_host::run_host(w, args, {}, 60000);
    if (!r || r->code != 0) return fail("host_drives_failed", r ? last_line(r->output) : std::string());
    {
      std::lock_guard lock(m_);
      v_.host_said = last_line(r->output);
    }
    read_host();
    auto again = self_host::run_host(w, {"drives", "list", "--json"}, {}, 60000);
    std::lock_guard lock(m_);
    if (again) {
      if (std::string j = json_out(again->output, 16); !j.empty()) v_.drives_json = std::move(j);
    }
    return;
  }
  std::vector<std::string> args;
  std::vector<std::string> names;
  if (what == "host_drives_combine") {
    const std::string choice = text("choice");
    if (choice != "separate" && choice != "mirror" && choice != "span" && choice != "stripe" && choice != "parity") {
      return fail("host_drives");
    }
    args = {"drives", "combine", choice};
    args.insert(args.end(), ids.begin(), ids.end());
    if (choice != "separate") {
      args.push_back("--label");
      args.push_back(text("label"));
    }
    names = list("names");
  } else {
    if (ids.size() != 1) return fail("host_drives");
    args = {"drives", "replace", text("set"), ids.front()};
    names = {text("name")};
  }
  args.push_back("--confirm-stdin");
  {
    std::lock_guard lock(m_);
    v_.host_step = what == "host_drives_combine" ? "drives_combine" : "drives_replace";
    v_.host_said.clear();
  }
  // The administrator prompt waits for the person, and a big mirror takes a while to make.
  auto r = self_host::run_host(w, args, names, 30 * 60 * 1000);
  if (!r || r->code != 0) return fail("host_drives_failed", r ? last_line(r->output) : std::string());
  {
    std::lock_guard lock(m_);
    v_.host_step.clear();
    v_.host_said = last_line(r->output);
  }
  read_host();
  auto again = self_host::run_host(w, {"drives", "list", "--json"}, {}, 60000);
  std::lock_guard lock(m_);
  if (again) {
    if (std::string j = json_out(again->output, 16); !j.empty()) v_.drives_json = std::move(j);
  }
}

void link::host_setup(const std::string& request) {
  const auto doc = json::parse(request, 4);
  const auto text = [&](const char* k) -> std::string {
    const std::string* v = doc ? doc->str(k) : nullptr;
    return v ? *v : std::string();
  };
  const auto step = [this](const char* what) {
    std::lock_guard lock(m_);
    v_.host_step = what;
  };
  const auto fail = [this](const char* why, std::string said = {}) {
    std::lock_guard lock(m_);
    v_.error = why;
    v_.host_step.clear();
    if (!said.empty()) v_.host_said = std::move(said);
  };
  {
    std::lock_guard lock(m_);
    v_.host_said.clear();
  }
  std::vector<self_host::folder_choice> folders;
  if (const json::value* a = doc ? doc->find("folders") : nullptr; a && a->k == json::kind::array) {
    for (const auto& f : a->a) {
      if (f.k != json::kind::object) continue;
      self_host::folder_choice c;
      if (const std::string* p = f.str("path")) c.path = *p;
      if (const std::string* l = f.str("label")) c.label = *l;
      c.backup = f.boolean("backup").value_or(false);
      folders.push_back(std::move(c));
    }
  }
  if (const std::string why = self_host::check_folders(folders); !why.empty()) {
    return fail(why == "no_library" ? "host_no_library" : why == "twice" ? "host_twice" : "host_no_folders");
  }
  // The DuckDNS name: both or neither (the token is checked here, and only hex
  // and '-' ever reach the program's input).
  std::optional<std::string> sub;
  const std::string typed_sub = text("duckdns"), typed_token = text("duckdns_token");
  if (!typed_sub.empty() || !typed_token.empty()) {
    sub = self_host::parse_subdomain(typed_sub);
    if (!sub) return fail("host_bad_name");
    if (!self_host::parse_token(typed_token)) return fail("host_bad_token");
  }
  const auto w = host_where();
  if (w.program.empty()) {
    read_host();
    return fail("no_driftwire_program");
  }

  // 1. The folders, each with the program's own `host share` (its rules: never
  // the host's own folder, nor one holding it). One already shared stays, made a
  // backup folder when it is ticked as one (self_host::share_plan).
  step("share");
  for (const auto& f : self_host::share_plan(folders, self_host::shared_folders(w))) {
    auto r = self_host::run_host(w, self_host::share_args(f.path, f.label, f.backup));
    if (!r) return fail("host_failed");
    if (r->code != 0) return fail("host_share", first_line(r->output));
  }
  // 2. The name, kept by the host's DuckDNS keeper.
  if (sub) {
    step("name");
    if (!self_host::set_duckdns(w, *sub, typed_token)) return fail("host_name");
  }
  // 3. Started, and at every login from now.
  step("start");
  if (!start_host_now(w)) return fail("host_start");
  self_host::host_status st;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(o_.host_wait_ms);
  for (;;) {
    st = self_host::read_status(w);
    // A state.json left by a host that crashed names a port nobody holds: it counts once it answers.
    if (st.running && self_host::answers(st.service_port, 300)) break;
    if (stopping_.load() || std::chrono::steady_clock::now() >= until) {
      read_host();
      return fail("host_not_listening");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  {
    std::lock_guard lock(m_);
    v_.host = st;
    v_.host_read = true;
  }
  // 4. The library goes in the first folder phones read, as a NAS's share.
  step("claim");
  const self_host::folder_choice* lib = self_host::library_folder(folders);
  std::string destination;
  {
    std::lock_guard lock(m_);
    destination = pub_.destination;
  }
  if (destination.empty() || self_host::same_path_key(destination) != self_host::same_path_key(lib->path)) {
    choose_folder(lib->path);
    std::lock_guard lock(m_);
    if (pub_.destination != lib->path) {
      if (v_.error.empty()) v_.error = "failed";
      v_.host_step.clear();
      return;
    }
  }
  // 5. Claimed over loopback: the host's own network by its own rule, and the
  // token written on this computer's own drive.
  // The host listens on both where it can ([::] dual stack), on one where the
  // OS has only one: IPv4's loopback first, then IPv6's.
  for (const char* loopback : {"127.0.0.1", "::1"}) {
    json::writer c;
    c.begin_object().key("do").string("claim").key("local").string(self_host::host_port(loopback, st.service_port));
    c.key("public").string("").end_object();
    {
      std::lock_guard lock(m_);
      v_.error.clear();
    }
    run(c.str());
    std::lock_guard lock(m_);
    if (v_.claimed && v_.error.empty()) break;
    if (v_.error != "unreachable" || std::string_view(loopback) == "::1") {
      if (v_.error.empty()) v_.error = "unreachable";
      v_.host_step.clear();
      return;
    }
  }
  // 6. The addresses a phone dials: this computer on its network, and the
  // DuckDNS name from outside (the one set now, or the host's already).
  const std::string lan = o_.lan_address ? o_.lan_address() : self_host::lan_address();
  if (!sub && !st.duckdns.empty()) sub = st.duckdns;
  {
    json::writer a;
    a.begin_object().key("do").string("set_addresses");
    a.key("local").string(lan.empty() ? std::string() : self_host::host_port(lan, st.service_port));
    std::string keep;
    {
      std::lock_guard lock(m_);
      keep = v_.public_name;  // one typed before stays when there is no DuckDNS name
    }
    // From outside, the port the router opened for it when it has (it may differ), else the service's own.
    const std::uint16_t outside = st.outside_port != 0 ? st.outside_port : st.service_port;
    a.key("public").string(sub ? self_host::public_route(*sub, outside) : keep);
    a.end_object();
    run(a.str());
    std::lock_guard lock(m_);
    if (!v_.error.empty()) {
      v_.host_step.clear();
      return;
    }
    if (lan.empty()) {
      v_.error = "host_no_network";  // claimed; the code would carry loopback only
      v_.host_step.clear();
      return;
    }
  }
  // 7. A code for the first phone, as Add a phone makes it.
  std::lock_guard lock(m_);
  v_.host_step.clear();
  requests_.push_back("{\"do\":\"pair\"}");
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
  w.key("shares_leaving").integer(v_.shares_leaving);
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
    w.key("backup").boolean(d.backup);
    if (d.desktop) w.key("desktop").boolean(true).key("share").string(d.share);
    if (!d.person.empty()) w.key("person").string(d.person);
    w.end_object();
  }
  w.end_array();
  w.key("people").begin_array();
  for (const auto& p : v_.people) w.begin_object().key("name").string(p.name).key("role").string(p.role).end_object();
  w.end_array();
  // The NAS owner's side: its passphrase ("set", "none", "broken"; "" from a NAS
  // too old to say), what it can do, this desktop's unlock and any lockout.
  w.key("nas_passphrase").string(v_.nas_passphrase);
  w.key("relay").string(v_.relay);
  w.key("nas_features").begin_array();
  for (const auto& f : v_.nas_features) w.string(f);
  w.end_array();
  {
    // Counted down from when the NAS last said; 0 once it has run out.
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(v_.unlocked_until - std::chrono::steady_clock::now()).count();
    w.key("unlocked_s").integer(v_.unlocked_s > 0 ? std::max<std::int64_t>(0, left) : 0);
  }
  w.key("locked_until").integer(v_.locked_until);
  w.key("qr_for_s").integer(v_.qr_for_s);
  w.key("folders").begin_array();
  for (const auto& f : v_.folders) {
    w.begin_object().key("id").string(f.id).key("label").string(f.label).key("path").string(f.path);
    if (f.backup) {
      w.key("backup").boolean(true).key("indexed").boolean(f.indexed);
      w.key("limit").integer(static_cast<std::int64_t>(f.limit)).key("reserve").integer(static_cast<std::int64_t>(f.reserve));
      w.key("used").integer(static_cast<std::int64_t>(f.used)).key("room").integer(static_cast<std::int64_t>(f.room));
    }
    w.end_object();
  }
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
  // This computer as the server: what its host's files said (ports and counts,
  // never an address); host_said is the program's own line, which may name a
  // folder as the card's other rows do. On screen only, never logged.
  if (v_.host_read) {
    const auto& h = v_.host;
    w.key("host").begin_object();
    w.key("installed").boolean(h.installed).key("set_up").boolean(h.set_up).key("running").boolean(h.running);
    w.key("at_login").boolean(h.at_login);
    w.key("service_port").integer(h.service_port).key("relay_port").integer(h.relay_port);
    w.key("configured_port").integer(h.configured_port).key("outside_port").integer(h.outside_port);
    w.key("folders").integer(static_cast<std::int64_t>(h.folders)).key("offline").integer(static_cast<std::int64_t>(h.offline));
    w.key("name").string(h.name).key("duckdns").string(h.duckdns).key("domain").string(h.domain);
    w.key("convert_on").boolean(h.convert_on).key("convert_available").boolean(h.convert_available);
    w.key("convert_busy").boolean(h.convert_busy).key("convert_encoder").string(h.convert_encoder);
    w.key("convert_helper").boolean(!h.convert_helper.empty());
    w.key("vpn").boolean(h.vpn).key("vpn_on").boolean(h.vpn_on).key("tls_port").integer(h.tls_port);
    w.key("tls_failed").boolean(h.tls_failed).key("relay_outside_port").integer(h.relay_outside_port);
    w.key("tls_outside_port").integer(h.tls_outside_port).key("relay_tls_port").integer(h.relay_tls_port);
    w.key("paused").boolean(h.paused).key("stats").begin_array();
    for (const auto& k : h.stats) w.string(k);
    w.end_array();
    w.key("keepers").begin_array();
    for (const auto& k : h.keepers) w.string(k);
    w.end_array();
    w.end_object();
  }
  w.key("host_step").string(v_.host_step);
  w.key("host_said").string(v_.host_said);
  if (!v_.relay_line.empty()) {
    w.key("relay_line").string(v_.relay_line);
    // The look's line is its code already (self_host::relay_line_in).
    w.key("relay_code").string(v_.relay_line.rfind(serve::kRelayTlsCodePrefix, 0) == 0
                                   ? v_.relay_line
                                   : serve::relay_connect_code(v_.relay_line));
  }
  w.key("nas_vpn_port").integer(v_.nas_vpn_port).key("nas_vpn_running").boolean(v_.nas_vpn_running);
  // This computer's users (names and roles only) and its share's addresses (screen only).
  if (v_.users_read) {
    w.key("users").begin_array();
    for (const auto& u : v_.users) {
      w.begin_object().key("name").string(u.name).key("role").string(u.role).key("applied").boolean(u.applied);
      if (u.folders) {
        w.key("folders").begin_array();
        for (const auto& f : *u.folders) w.string(f);
        w.end_array();
      }
      w.end_object();
    }
    w.end_array();
  }
  w.key("share_addresses").begin_array();
  for (const auto& a : v_.share_addresses) w.string(a);
  w.end_array();
  if (!v_.drives_json.empty()) w.key("drives").raw(v_.drives_json);
  if (!v_.drives_choices_json.empty()) w.key("drives_choices").raw(v_.drives_choices_json);
  w.end_object();
  return w.take();
}

}  // namespace mv::phone
