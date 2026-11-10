// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Setting a phone up, from the Phone access card (docs/plans/mobile.md §5 rule
// 5, §7.2, §7.3, §7.4, §7.5). The network half is the Driftwire add-on's,
// reached through the host table: add-ons reach each other no other way.
//
// One request at a time on the thread made here. It waits on the NAS and the
// Keychain, so none of this runs on the UI, the render or the control thread;
// the card asks (phone_link) and watches (phone_link_json).
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "addons/ai/catalog_chain.h"
#include "addons/ai/engine.h"
#include "addons/driftwire/secrets.h"
#include "core/json.h"

namespace mv::ai {

std::vector<catalog::shared_file> read_shared_list(const std::string& file);

namespace {

constexpr int kPairTtlMs = 300000;
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

result<std::string> engine::phone_route(const char* method, const std::string& json) {
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
}

void engine::phone_link_loop() {
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
        phone_link_do(request);
        // The owner's session is kept only while a code shows: one forwarded
        // service, a few session slots, and a Mac that may sit here for days.
        bool showing = false;
        {
          std::lock_guard lock(phone_link_m_);
          showing = link_.qr_expires != 0;
        }
        if (!showing) (void)phone_route("release", "{}");
      }
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
          if (id && label && path) rows.push_back({*id, *label, *path});
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
  bool other = false;
  std::string other_fp;
  if (lib) {
    if (lib->public_key == mine) {
      // A takeover this desktop asked for has landed: the library is its own now.
      bool adopted = false;
      {
        std::lock_guard lock(phone_m_);
        if (phone_.library_id != lib->library_id || phone_.epoch != lib->epoch) {
          phone_.library_id = lib->library_id;
          phone_.epoch = lib->epoch;
          adopted = true;
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
  std::lock_guard lock(phone_link_m_);
  link_.other_publisher = other;
  link_.other_fingerprint = other_fp;
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
    any = link_.claimed && !link_.devices.empty();
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
      (void)phone_route("release", "{}");
      return;
    }
    phone_link_refresh(true);
    std::lock_guard lock(phone_link_m_);
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
    if (auto r = phone_route("claim", w.str()); !r) return fail(error_name(r.error()));
    phone_link_refresh();
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
    json::writer w;
    w.begin_object().key("ttl_ms").integer(kPairTtlMs).end_object();
    auto answer = phone_route("pair", w.str());
    if (!answer) return fail(error_name(answer.error()));
    const auto a = json::parse(*answer, 4);
    if (!a) return fail("failed");
    catalog::pair_payload p;
    p.library_id = library_id;
    p.library_name = last_part(destination);
    p.publisher_key = catalog::public_of(*secret);
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
    link_.qr_fingerprint = catalog::fingerprint(p.publisher_key);
    link_.qr_expires = p.expires;
    return;
  }
  if (what == "pair_stop") {
    clear_qr();
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
  if (what == "add_folder" || what == "remove_folder") {
    json::writer w;
    w.begin_object();
    if (what == "add_folder") {
      w.key("label").string(text("label")).key("path").string(text("path"));
    } else {
      w.key("id").string(text("id"));
    }
    w.end_object();
    if (auto r = phone_route(what == "add_folder" ? "add_root" : "remove_root", w.str()); !r) return fail(error_name(r.error()));
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
    if (auto r = catalog::hand_over(destination, *secret, to); !r) return fail(r.error() == status::permission_denied ? "refused" : "unreachable");
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
  for (const auto& f : link_.folders) w.begin_object().key("id").string(f.id).key("label").string(f.label).key("path").string(f.path).end_object();
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
  w.key("other_fingerprint").string(link_.other_fingerprint);
  w.key("asking_name").string(link_.asking_name);
  w.key("asking_key").string(link_.asking_key);
  w.key("asking_fingerprint").string(link_.asking_fingerprint);
  w.end_object();
  return w.take();
}

}  // namespace mv::ai
