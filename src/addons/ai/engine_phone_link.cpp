// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Setting a phone up, from the Phone access card (docs/plans/mobile.md §5 rule
// 5, §7.2, §7.3, §7.4, §7.5). The network half is the Driftwire add-on's,
// reached through the host table: add-ons reach each other no other way.
//
// One request at a time on the thread made here. It waits on the NAS and the
// Keychain, so none of this runs on the UI, the render or the control thread;
// the card asks (phone_link) and watches (phone_link_json).
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <fstream>
#include <sstream>

#include "addons/ai/catalog_chain.h"
#include "addons/ai/engine.h"
#include "addons/driftwire/secrets.h"
#include "core/json.h"

namespace mv::ai {

std::vector<catalog::shared_file> read_shared_list(const std::string& file);

namespace {

// Whether the NAS path of a backup folder ("/volume1/photo/Phone backup") is under the Mac's
// Local search root `local` ("/Volumes/photo" or "/Volumes/photo/Phone backup"): the share is the
// folder right under /volumeN on a Synology and is mounted under its own name. A heuristic for the
// card's "indexed" line; it never changes what is indexed.
bool phone_nas_covered_by(const std::string& nas, const std::string& local) {
  std::vector<std::string> n, l;
  const auto split = [](const std::string& p, std::vector<std::string>& out) {
    std::string part;
    for (char c : p + "/") {
      if (c == '/') {
        if (!part.empty()) out.push_back(part);
        part.clear();
      } else {
        part += c;
      }
    }
  };
  split(nas, n);
  split(local, l);
  if (n.size() >= 2 && n[0].rfind("volume", 0) == 0) n.erase(n.begin());  // share/...
  if (n.empty()) return false;
  // Where the share's name is in the local path: from there the two must agree.
  for (std::size_t i = 0; i < l.size(); ++i) {
    if (l[i] != n[0]) continue;
    const std::size_t rest = l.size() - i;
    if (rest > n.size()) continue;
    if (std::equal(l.begin() + static_cast<std::ptrdiff_t>(i), l.end(), n.begin())) return true;
  }
  return false;
}

}  // namespace

namespace {

constexpr int kPairPollMs = 2000;
constexpr const char* kKeyName = "catalogue-signing-key";

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

// What a failure from the other add-on is called on the card.
const char* error_name(status s) noexcept {
  switch (s) {
    case status::not_found: return "no_driftwire";
    case status::permission_denied: return "refused";
    case status::invalid_arg: return "bad_address";
    case status::busy: return "too_many";
    default: return "unreachable";
  }
}

}  // namespace

// What must survive Local search restarting (phone/link.json): a hand-over
// not yet confirmed by a share that reads back, and a code left on the NAS.
void engine::phone_link_load_flags() {
  const std::string file = data_dir_ + "/phone/link.json";
  // Read whole: its size, then exactly that many bytes. A read that fails is
  // tried once more after a moment (a file being replaced, a disk that hiccuped).
  const auto read_whole = [&]() -> std::optional<std::string> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(fs_path(file), ec);
    if (ec || size > 64 * 1024) return std::nullopt;
    std::ifstream in(fs_path(file), std::ios::binary);
    if (!in) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (in.gcount() != static_cast<std::streamsize>(text.size())) return std::nullopt;
    return text;
  };
  std::error_code ec;
  const bool there = std::filesystem::exists(fs_path(file), ec);
  if (!ec && !there) {
    // Never written: nothing handed over, nothing left. The one case that is open.
    std::lock_guard lock(phone_link_m_);
    link_.flags_read = true;
    return;
  }
  auto text = read_whole();
  if (!text) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    text = read_whole();
  }
  const auto doc = text ? json::parse(*text, 4) : std::nullopt;
  std::lock_guard lock(phone_link_m_);
  if (!doc || doc->k != json::kind::object) {
    // There and not read, or read and not ours to understand: fail closed.
    // Signing nearby copies with a key that may have been handed away is worse
    // than not serving nearby. Nothing is written over it until a read
    // succeeds (a later run), so a passing failure cannot erase a hand-over.
    link_.handed_over = true;
    link_.other_publisher = true;
    link_.flags_read = false;
    return;
  }
  link_.flags_read = true;
  link_.handed_over = doc->boolean("handed_over").value_or(false);
  link_.code_left = doc->boolean("code_left").value_or(false);
  link_.code_left_until = doc->integer("code_left_until").value_or(0);
  if (link_.handed_over) link_.other_publisher = true;
}

void engine::phone_link_save_flags() {
  bool handed = false, left = false;
  std::int64_t until = 0;
  {
    std::lock_guard lock(phone_link_m_);
    // The file there could not be read: never overwritten blind, except to
    // record a hand-over, which is the side a failed read assumes anyway.
    if (!link_.flags_read && !link_.handed_over) {
      link_.flags_unsaved = true;
      return;
    }
    handed = link_.handed_over;
    left = link_.code_left;
    until = link_.code_left_until;
  }
  json::writer w;
  w.begin_object()
      .key("handed_over").boolean(handed)
      .key("code_left").boolean(left)
      .key("code_left_until").integer(until)
      .end_object();
  const std::string file = data_dir_ + "/phone/link.json";
  const std::string part = file + ".part";
  std::error_code ec;
  std::filesystem::create_directories(fs_path(data_dir_ + "/phone"), ec);
  // Whole, on the disk, then renamed in: a crash leaves the old file or this one.
  bool ok = false;
  if (FILE* f = std::fopen(part.c_str(), "wb")) {
    const std::string& text = w.str();
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
  // The engine has no log of its own; the card's state says it, once.
  std::lock_guard lock(phone_link_m_);
  link_.flags_unsaved = !ok;
  if (ok) link_.flags_read = true;  // what is there now is what this wrote
}

result<std::string> engine::phone_route(const char* method, const std::string& json) {
  // Stopping: no new wait on the NAS, so stop never sits through one bound
  // after another. What only tidies this Mac (or withdraws a code) still goes:
  // Driftwire keeps running after Local search stops.
  const std::string_view m(method);
  if (stopping_.load() && m != "nearby_stop" && m != "release" && m != "pair_stop") return err(status::cancelled);
  auto r = host_.addon_request("driftwire", method, json);
  if (!r && r.error() == status::not_found && std::string_view(method) == "status") {
    std::lock_guard lock(phone_link_m_);
    link_.driftwire = false;
  }
  return r;
}

result<std::vector<std::uint8_t>> engine::phone_signing_key(bool make) {
  auto keys = deps_.key_store ? deps_.key_store() : driftwire::secrets::os_store("MediaViewer Local search");
  if (!keys) return err(status::unsupported_format);
  if (auto kept = keys->get(kKeyName)) return std::move(*kept);
  if (!make) return err(status::not_found);
  auto secret = catalog::make_keypair().second;
  if (secret.empty()) return err(status::io);  // no usable libsodium: never store an empty key
  if (!keys->put(kKeyName, secret)) return err(status::io);
  auto back = keys->get(kKeyName);
  if (!back || *back != secret) return err(status::io);
  return secret;
}

expected engine::phone_link(const std::string& request_json) {
  if (stopping_.load()) return err(status::cancelled);
  const auto doc = json::parse(request_json, 4);
  if (!doc || !doc->str("do")) return err(status::invalid_arg);
  std::lock_guard lock(phone_link_m_);
  if (stopping_.load()) return err(status::cancelled);  // checked again where the thread is taken
  link_.busy = *doc->str("do");  // from now, so a poll right after this sees it
  link_requests_.push_back(request_json);
  if (!phone_link_thread_.joinable()) phone_link_thread_ = std::thread([this] { phone_link_loop(); });
  phone_link_cv_.notify_all();
  return {};
}

void engine::phone_link_stop() noexcept {
  std::thread t;
  {
    std::lock_guard lock(phone_link_m_);
    t = std::move(phone_link_thread_);
    phone_link_cv_.notify_all();
  }
  if (t.joinable()) t.join();
  // Local search going (an update, a removal, quitting): Driftwire may stay,
  // and must not go on serving a catalogue nobody here keeps up to date. Both
  // calls are on this Mac. A code showing is not withdrawn here: that is a
  // round trip to a NAS that may not answer, and quitting must not wait on
  // it. It is written down and withdrawn at the next start; the NAS lets it
  // lapse within five minutes anyway.
  bool nearby = false, showing = false;
  {
    std::lock_guard lock(phone_link_m_);
    nearby = link_.nearby;
    showing = link_.qr_expires != 0;
  }
  try {
    if (showing) {
      // Withdrawn at the next start (phone_link_refresh), not now.
      {
        std::lock_guard lock(phone_link_m_);
        link_.code_left = true;
        link_.code_left_until = static_cast<std::int64_t>(std::time(nullptr)) + static_cast<std::int64_t>(deps_.phone_link_code_ttl_s) + 1;
      }
      phone_link_save_flags();
    }
    if (nearby) (void)phone_route("nearby_stop", "{}");
    if (nearby || showing) (void)phone_route("release", "{}");
  } catch (...) {
  }
}

void engine::phone_link_loop() {
  phone_link_load_flags();  // before the first request: a small file, read once per run
  for (;;) {
    std::string request;
    bool poll = false;
    {
      std::unique_lock lock(phone_link_m_);
      const auto ready = [this] { return stopping_.load() || !link_requests_.empty(); };
      if (link_.qr_expires != 0) {
        poll = !phone_link_cv_.wait_for(lock, std::chrono::milliseconds(kPairPollMs), ready);
      } else {
        phone_link_cv_.wait(lock, ready);
      }
      if (stopping_.load()) return;
      if (!poll) {
        request = std::move(link_requests_.front());
        link_requests_.pop_front();
        const auto doc = json::parse(request, 4);
        link_.busy = *doc->str("do");
        link_.error.clear();
      }
    }
    try {
      if (poll) {
        phone_link_do("{\"do\":\"poll\"}");
      } else {
        const auto doc = json::parse(request, 4);
        const bool pairing = doc && doc->str("do") && *doc->str("do") == "pair";
        {
          // Set by this thread alone, around the one request that makes a
          // code: `busy` follows whatever is queued next and cannot say so.
          std::lock_guard lock(phone_link_m_);
          link_.pairing = pairing;
        }
        phone_link_do(request);
        {
          std::lock_guard lock(phone_link_m_);
          link_.pairing = false;
        }
        // The owner's session is kept only while a code shows: one forwarded
        // service, a few session slots, and a Mac that may sit here for days.
        bool showing = false;
        {
          std::lock_guard lock(phone_link_m_);
          showing = link_.qr_expires != 0;
        }
        if (!showing) (void)phone_route("release", "{}");
      }
      bool dirty = false;
      {
        std::lock_guard lock(phone_link_m_);
        dirty = link_.flags_dirty;
        link_.flags_dirty = false;
      }
      if (dirty) phone_link_save_flags();
    } catch (...) {
      std::lock_guard lock(phone_link_m_);
      link_.error = "failed";
    }
    std::lock_guard lock(phone_link_m_);
    if (link_requests_.empty()) link_.busy.clear();
  }
}

void engine::phone_link_tick() {
  // Once, for someone who has published before: bring the nearby service back.
  std::uint64_t newest = 0;
  bool used = false;
  {
    std::lock_guard lock(phone_m_);
    used = !phone_.library_id.empty();
    newest = std::max(phone_.waiting, phone_.generation);
  }
  if (!used) return;
  std::lock_guard lock(phone_link_m_);
  if (stopping_.load()) return;  // phone_link_stop takes the thread under this lock
  // A phone revoked here while the NAS was away keeps the NAS's door open
  // until the NAS hears it: ask again, backing off, until it has.
  const auto now = std::chrono::steady_clock::now();
  if (link_tick_seen_ && link_.pending > 0 && now >= link_.retry_at && link_requests_.empty() && link_.busy.empty()) {
    link_.retry_s = link_.retry_s <= 0 ? deps_.phone_link_retry_s : std::min(link_.retry_s * 2, 3600.0);
    link_.retry_at = now + std::chrono::milliseconds(static_cast<std::int64_t>(link_.retry_s * 1000));
    link_requests_.push_back("{\"do\":\"refresh\"}");
    if (!phone_link_thread_.joinable()) phone_link_thread_ = std::thread([this] { phone_link_loop(); });
    phone_link_cv_.notify_all();
    return;
  }
  const bool first = !link_tick_seen_;
  // One try per generation: no paired phone, or a copy that failed, is not
  // asked about again on every tick. A phone added later asks by itself.
  const bool stale = link_.claimed && newest != 0 && newest != link_.tried;
  if (!first && (!stale || !link_requests_.empty() || !link_.busy.empty())) return;
  link_tick_seen_ = true;
  if (!first) link_.tried = newest;
  link_requests_.push_back(first ? "{\"do\":\"refresh\"}" : "{\"do\":\"nearby\"}");
  if (!phone_link_thread_.joinable()) phone_link_thread_ = std::thread([this] { phone_link_loop(); });
  phone_link_cv_.notify_all();
}

void engine::phone_link_refresh(bool phones_only) {
  bool left = false, lapsed = false;
  {
    std::lock_guard lock(phone_link_m_);
    // Never while a code is showing or being made: the withdraw would take
    // the new one down (the service holds one code at a time).
    left = link_.code_left && link_.qr_expires == 0 && !link_.pairing;
    // The NAS drops a code by itself once its lifetime is over.
    lapsed = link_.code_left && static_cast<std::int64_t>(std::time(nullptr)) >= link_.code_left_until;
  }
  // A code was showing when Local search last stopped: it goes now. A
  // definite answer (done, refused, not owned) settles it; a NAS out of reach
  // or not ready leaves it for the next refresh, and never past the code's
  // own lifetime.
  bool settled = lapsed;
  if (left && !lapsed) {
    const auto r = phone_route("pair_stop", "{}");
    // Out of reach, or not able to answer yet: the NAS away, too busy for
    // another session, or Driftwire not registered yet at startup. The
    // lifetime above still bounds how long it is asked.
    const mv::status e = r ? mv::status::ok : r.error();
    settled = r || (e != status::io && e != status::timeout && e != status::cancelled && e != status::busy &&
                    e != status::not_found);
  }
  if (left && settled) {
    {
      std::lock_guard lock(phone_link_m_);
      link_.code_left = false;
    }
    phone_link_save_flags();
  }
  auto status_json = phone_route("status", "{}");
  if (!status_json) {
    std::lock_guard lock(phone_link_m_);
    link_.claimed = false;
    return;
  }
  const auto s = json::parse(*status_json, 4);
  const bool claimed = s && s->boolean("claimed").value_or(false);
  {
    std::lock_guard lock(phone_link_m_);
    link_.driftwire = true;
    link_.claimed = claimed;
    const std::int64_t pending = s ? s->integer("pending_revokes").value_or(0) : 0;
    if (pending > 0 && link_.pending == 0) {
      link_.retry_s = 0;  // a new one: the first retry comes soon
      link_.retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<std::int64_t>(deps_.phone_link_retry_s * 1000));
    }
    link_.pending = pending;
    if (s) {
      if (const std::string* v = s->str("local")) link_.local = *v;
      if (const std::string* v = s->str("public")) link_.public_name = *v;
    }
  }
  if (claimed) {
    if (auto d = phone_route("devices", "{}")) {
      const auto doc = json::parse(*d, 6);
      std::vector<phone_link_view::phone_row> rows;
      if (const json::value* list = doc ? doc->find("devices") : nullptr; list && list->k == json::kind::array) {
        for (const json::value& v : list->a) {
          const std::string* name = v.str("name");
          const std::string* key = v.str("key");
          if (name && key) rows.push_back({*name, *key});
        }
      }
      std::lock_guard lock(phone_link_m_);
      link_.devices = std::move(rows);
      link_.reachable = doc && doc->boolean("reachable").value_or(false);
      link_.code_waiting = doc && doc->boolean("code_waiting").value_or(false);
    }
    if (phones_only) return;  // the two-second poll under a showing code
    if (auto r = phone_route("roots", "{}")) {
      const auto doc = json::parse(*r, 6);
      std::vector<phone_link_view::folder_row> rows;
      if (const json::value* list = doc ? doc->find("roots") : nullptr; list && list->k == json::kind::array) {
        for (const json::value& v : list->a) {
          const std::string* id = v.str("id");
          const std::string* label = v.str("label");
          const std::string* path = v.str("path");
          if (!id || !label || !path) continue;
          phone_link_view::folder_row row{*id, *label, *path};
          if (const json::value* b = v.find("backup"); b && b->k == json::kind::object) {
            const auto num = [&](const char* k) { return static_cast<std::uint64_t>(std::max<std::int64_t>(0, b->integer(k).value_or(0))); };
            row.backup = true;
            row.limit = num("limit");
            row.reserve = num("reserve");
            row.used = num("used");
            row.room = num("room");
            std::vector<root_row> roots;
            {
              std::lock_guard lock(status_m_);
              roots = roots_cache_;
            }
            for (const auto& r : roots) {
              if (r.enabled && r.recursive && phone_nas_covered_by(*path, r.path)) row.indexed = true;
            }
          }
          rows.push_back(std::move(row));
        }
      }
      std::lock_guard lock(phone_link_m_);
      link_.folders = std::move(rows);
    }
  }

  // Who signs this library now, as the share says.
  std::string destination;
  {
    std::lock_guard lock(phone_m_);
    destination = phone_.destination;
  }
  if (destination.empty()) return;
  auto lib = catalog::read_library(destination);
  auto secret = phone_signing_key(false);
  std::string mine;
  if (secret) mine = catalog::to_hex(catalog::public_of(*secret));
  bool other = false, mismatch = false;
  bool handed_over = false;
  {
    std::lock_guard lock(phone_link_m_);
    handed_over = link_.handed_over;
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
        std::lock_guard lock(phone_m_);
        const bool never_published = phone_.generation == 0 && phone_.waiting == 0;
        if (lib->library_id == phone_.library_id) {
          if (lib->epoch > phone_.epoch) {
            phone_.epoch = lib->epoch;
            adopted = true;
          } else if (lib->epoch < phone_.epoch) {
            mismatch = true;
          }
        } else if (never_published || phone_.library_id.empty()) {
          phone_.library_id = lib->library_id;
          phone_.epoch = lib->epoch;
          adopted = true;
        } else {
          mismatch = true;
        }
      }
      if (adopted) (void)save_phone_state();
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
      std::lock_guard lock(phone_link_m_);
      link_.handed_over = false;
    }
    phone_link_save_flags();
  }
  std::lock_guard lock(phone_link_m_);
  link_.mismatch = mismatch;
  if (link_.handed_over) other = true;
  if (!other) link_.other_fingerprint.clear();
  link_.other_publisher = other;
  if (!other_fp.empty()) link_.other_fingerprint = other_fp;
  link_.asking_name = asking_name;
  link_.asking_key = asking_key;
  link_.asking_fingerprint = asking_fp;
}

void engine::phone_link_prepare_nearby() {
  std::uint64_t generation = 0, epoch = 1;
  std::string library_id, destination;
  {
    std::lock_guard lock(phone_m_);
    generation = std::max(phone_.waiting, phone_.generation);
    epoch = phone_.epoch;
    library_id = phone_.library_id;
    destination = phone_.destination;
  }
  bool any = false;
  {
    std::lock_guard lock(phone_link_m_);
    // Handed over: this desktop's key signs nothing a phone should take now.
    any = link_.claimed && !link_.devices.empty() && !link_.other_publisher;
    if (!any && link_.nearby) link_.nearby = false;
  }
  if (!any) {
    (void)phone_route("nearby_stop", "{}");
    return;
  }
  if (generation == 0) return;
  auto secret = phone_signing_key(false);
  if (!secret) return;
  const std::string dir = data_dir_ + "/phone/nearby";
  const std::string gen_dir = data_dir_ + "/phone/gen/" + std::to_string(generation);
  catalog::nearby_options o;
  o.dir = dir;
  o.gen_dir = gen_dir;
  o.library_id = library_id;
  o.epoch = epoch;
  o.generation = generation;
  o.secret_key = *secret;
  o.public_key = catalog::public_of(*secret);
  o.shared = read_shared_list(gen_dir + ".shared.json");
  if (!destination.empty()) o.library_json = slurp(destination + "/.mediaviewer/library.json");
  std::error_code ec;
  if (!std::filesystem::is_directory(fs_path(gen_dir), ec)) return;
  if (const expected made = catalog::prepare_nearby(o); !made) return;
  json::writer w;
  w.begin_object().key("folder").string(dir).key("library_id").string(library_id).end_object();
  const bool up = phone_route("nearby", w.str()).has_value();
  std::lock_guard lock(phone_link_m_);
  link_.nearby = up;
  link_.prepared = generation;
}

void engine::phone_link_do(const std::string& request) {
  const auto doc = json::parse(request, 4);
  const std::string what = *doc->str("do");
  const auto text = [&](const char* k) -> std::string {
    const std::string* v = doc->str(k);
    return v ? *v : std::string();
  };
  const auto fail = [this](const char* why) {
    std::lock_guard lock(phone_link_m_);
    link_.error = why;
  };
  const auto clear_qr = [this] {
    std::lock_guard lock(phone_link_m_);
    link_.qr_url.clear();
    link_.qr_library.clear();
    link_.qr_fingerprint.clear();
    link_.qr_expires = 0;
  };
  std::string destination;
  {
    std::lock_guard lock(phone_m_);
    destination = phone_.destination;
  }

  if (what == "poll") {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    std::int64_t expires = 0;
    {
      std::lock_guard lock(phone_link_m_);
      expires = link_.qr_expires;
    }
    if (expires != 0 && now >= expires) {
      clear_qr();
      (void)phone_route("pair_stop", "{}");
      (void)phone_route("release", "{}");
      return;
    }
    phone_link_refresh(true);
    std::lock_guard lock(phone_link_m_);
    if (!link_.code_waiting && link_.qr_expires != 0) {
      // Used by a phone already paired (a re-scan), or gone from the service:
      // a code it would not take again does not stay on the card.
      bool any_new = false;
      for (const auto& d : link_.devices) {
        any_new = any_new || std::find(link_before_.begin(), link_before_.end(), d.key) == link_before_.end();
      }
      if (!any_new) {
        link_.qr_url.clear();
        link_.qr_library.clear();
        link_.qr_fingerprint.clear();
        link_.qr_expires = 0;
        return;
      }
    }
    bool landed = false;
    for (const auto& d : link_.devices) {
      if (std::find(link_before_.begin(), link_before_.end(), d.key) == link_before_.end()) {
        link_.added.push_back(d.name);
        link_before_.push_back(d.key);
        landed = true;
      }
    }
    if (landed) {
      // The code is spent; the next phone gets a new one.
      link_.qr_url.clear();
      link_.qr_library.clear();
      link_.qr_fingerprint.clear();
      link_.qr_expires = 0;
      // The new phone can be served nearby from now on, and the session
      // kept open for this code is let go after it.
      link_requests_.push_back("{\"do\":\"nearby\"}");
    }
    return;
  }
  if (what == "refresh") {
    phone_link_refresh();
    phone_link_prepare_nearby();
    return;
  }
  if (what == "nearby") {
    phone_link_prepare_nearby();
    return;
  }
  if (what == "claim") {
    if (destination.empty()) return fail("no_folder");
    json::writer w;
    w.begin_object().key("share").string(destination).key("local").string(text("local")).key("public").string(text("public")).end_object();
    auto r = phone_route("claim", w.str());
    // Timed out: the service may have taken it and the answer been lost. A
    // second claim from this Mac is answered with the same keys.
    if (!r && r.error() == status::timeout) r = phone_route("claim", w.str());
    if (!r && r.error() != status::timeout) return fail(error_name(r.error()));
    // A slow claim may still have gone through: the service says which.
    phone_link_refresh();
    if (!r) {
      std::lock_guard lock(phone_link_m_);
      if (!link_.claimed) link_.error = "unreachable";
    }
    return;
  }
  if (what == "pair") {
    std::string library_id, name;
    {
      std::lock_guard lock(phone_m_);
      library_id = phone_.library_id;
    }
    if (library_id.empty()) return fail("not_published");
    auto secret = phone_signing_key(false);
    if (!secret) return fail("not_published");
    const auto publisher = catalog::public_of(*secret);
    json::writer w;
    w.begin_object()
        .key("ttl_ms").integer(static_cast<std::int64_t>(deps_.phone_link_code_ttl_s * 1000))
        .key("library_id").string(library_id)
        .key("publisher_key").string(catalog::to_hex(publisher))
        .end_object();
    auto answer = phone_route("pair", w.str());
    if (!answer) return fail(error_name(answer.error()));
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
    const std::string url = catalog::pair_url(p);
    phone_link_refresh();  // who is paired before this code
    std::lock_guard lock(phone_link_m_);
    link_before_.clear();
    for (const auto& d : link_.devices) link_before_.push_back(d.key);
    link_.added.clear();
    link_.qr_url = url;
    link_.qr_library = p.library_name;
    link_.qr_fingerprint = field("check");  // Driftwire's pairing_check: the phone computes the same
    if (link_.qr_fingerprint.empty()) {
      // A Driftwire too old to say it: no code to compare is no code to show.
      link_.qr_url.clear();
      link_.qr_expires = 0;
      link_.error = "no_driftwire";
      return;
    }
    link_.qr_expires = p.expires;
    // A new code is the one on the service now: nothing left from before to withdraw.
    const bool was_left = link_.code_left;
    link_.code_left = false;
    if (was_left) {
      // Saved outside the lock, below.
      link_.flags_dirty = true;
    }
    link_.code_waiting = true;
    return;
  }
  if (what == "pair_stop") {
    clear_qr();
    (void)phone_route("pair_stop", "{}");  // a photo of it pairs nothing now
    (void)phone_route("release", "{}");
    return;
  }
  if (what == "revoke") {
    json::writer w;
    w.begin_object().key("key").string(text("key")).end_object();
    auto r = phone_route("revoke", w.str());
    if (!r) return fail(error_name(r.error()));
    const auto a = json::parse(*r, 4);
    if (a && !a->boolean("nas_told").value_or(true)) fail("revoke_waits");
    phone_link_refresh();
    phone_link_prepare_nearby();
    return;
  }
  if (what == "add_folder" || what == "remove_folder" || what == "set_backup") {
    // Sizes come from the card as decimal byte counts (strings, as every field of a request is).
    const auto bytes = [&](const char* k) -> std::int64_t {
      const std::string t = text(k);
      std::int64_t v = 0;
      for (char c : t) {
        if (c < '0' || c > '9' || v > (INT64_MAX - 9) / 10) return -1;
        v = v * 10 + (c - '0');
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
      const auto limit = bytes("limit");
      const auto reserve = bytes("reserve");
      if (limit <= 0 || reserve < 0) return fail("invalid_arg");
      if (what == "add_folder") w.key("backup").boolean(true);
      w.key("limit").integer(limit).key("reserve").integer(reserve);
    }
    w.end_object();
    const char* verb = what == "add_folder" ? "add_root" : what == "remove_folder" ? "remove_root" : "set_backup";
    if (auto r = phone_route(verb, w.str()); !r) return fail(error_name(r.error()));
    phone_link_refresh();
    return;
  }
  if (what == "set_addresses") {
    json::writer l, p, c;
    l.begin_object().key("local").string(text("local")).end_object();
    if (!text("local").empty()) {
      if (auto r = phone_route("set_local", l.str()); !r) return fail(error_name(r.error()));
    }
    p.begin_object().key("public").string(text("public")).end_object();
    if (auto r = phone_route("set_public", p.str()); !r) return fail(error_name(r.error()));
    std::string warning;
    if (!text("public").empty()) {
      c.begin_object().key("address").string(text("public")).end_object();
      if (auto r = phone_route("check_address", c.str())) {
        const auto a = json::parse(*r, 4);
        if (const std::string* v = a ? a->str("warning") : nullptr) warning = *v;
      }
    }
    {
      std::lock_guard lock(phone_link_m_);
      link_.warning = warning;
    }
    phone_link_refresh();
    return;
  }
  if (what == "takeover_ask" || what == "takeover_restart") {
    if (destination.empty()) return fail("no_folder");
    auto secret = phone_signing_key(true);
    if (!secret) return fail("no_key_store");
    const auto pub = catalog::public_of(*secret);
    if (what == "takeover_ask") {
      std::string machine = "Another computer";
      if (auto s = phone_route("status", "{}")) {
        const auto d = json::parse(*s, 4);
        if (const std::string* v = d ? d->str("machine") : nullptr; v && !v->empty()) machine = *v;
      }
      if (const expected r = catalog::request_takeover(destination, pub, machine); !r) return fail("unreachable");
      return;
    }
    if (auto r = catalog::restart_publisher(destination, pub); !r) return fail("unreachable");
    phone_link_refresh();
    return;
  }
  if (what == "takeover_allow") {
    if (destination.empty()) return fail("no_folder");
    auto request_seen = catalog::read_takeover(destination);
    if (!request_seen || request_seen->public_key != text("key")) return fail("failed");
    auto secret = phone_signing_key(false);
    if (!secret) return fail("no_key_store");
    const auto to = key_bytes(request_seen->public_key);
    if (to.empty()) return fail("failed");
    auto moved = catalog::hand_over(destination, *secret, to);
    if (!moved) return fail(moved.error() == status::permission_denied ? "refused" : "unreachable");
    if (deps_.phone_link_after_hand_over) deps_.phone_link_after_hand_over();
    {
      // What hand_over wrote is the truth now, whether or not the share reads
      // back on the next try: this desktop is no longer the publisher.
      std::lock_guard lock(phone_link_m_);
      link_.other_publisher = true;
      link_.handed_over = true;
      link_.other_fingerprint = catalog::fingerprint(to);
    }
    phone_link_save_flags();
    phone_link_prepare_nearby();  // its key is not the library's now: the nearby copy goes
    phone_link_refresh();
    return;
  }
  fail("failed");
}

std::string engine::phone_link_json() const {
  json::writer w;
  std::lock_guard lock(phone_link_m_);
  w.begin_object();
  w.key("driftwire").boolean(link_.driftwire);
  w.key("busy").string(link_.busy);
  w.key("error").string(link_.error);
  w.key("claimed").boolean(link_.claimed);
  w.key("reachable").boolean(link_.reachable);
  w.key("local").string(link_.local);
  w.key("public").string(link_.public_name);
  w.key("warning").string(link_.warning);
  w.key("devices").begin_array();
  for (const auto& d : link_.devices) w.begin_object().key("name").string(d.name).key("key").string(d.key).end_object();
  w.end_array();
  w.key("folders").begin_array();
  for (const auto& f : link_.folders) {
    w.begin_object().key("id").string(f.id).key("label").string(f.label).key("path").string(f.path);
    if (f.backup) {
      w.key("backup").boolean(true).key("indexed").boolean(f.indexed);
      w.key("limit").integer(static_cast<std::int64_t>(f.limit)).key("reserve").integer(static_cast<std::int64_t>(f.reserve));
      w.key("used").integer(static_cast<std::int64_t>(f.used)).key("room").integer(static_cast<std::int64_t>(f.room));
    }
    w.end_object();
  }
  w.end_array();
  w.key("qr").string(link_.qr_url);
  w.key("qr_library").string(link_.qr_library);
  w.key("qr_fingerprint").string(link_.qr_fingerprint);
  w.key("qr_expires").integer(link_.qr_expires);
  w.key("added").begin_array();
  for (const auto& n : link_.added) w.string(n);
  w.end_array();
  w.key("nearby").boolean(link_.nearby);
  w.key("other_publisher").boolean(link_.other_publisher);
  w.key("mismatch").boolean(link_.mismatch);
  w.key("flags_unsaved").boolean(link_.flags_unsaved);
  w.key("other_fingerprint").string(link_.other_fingerprint);
  w.key("asking_name").string(link_.asking_name);
  w.key("asking_key").string(link_.asking_key);
  w.key("asking_fingerprint").string(link_.asking_fingerprint);
  w.end_object();
  return w.take();
}

}  // namespace mv::ai
