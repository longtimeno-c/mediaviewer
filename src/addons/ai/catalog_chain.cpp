// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/catalog_chain.h"

#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "addons/ai/sha256.h"
#include "core/json.h"

namespace mv::ai::catalog {

namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())); }

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// Whole and renamed in, so a reader never sees half of it.
[[nodiscard]] bool replace_file(const fs::path& to, std::string_view text) {
  fs::path tmp = to;
  tmp += ".part";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) return false;
  }
  std::error_code ec;
  fs::rename(tmp, to, ec);
  return !ec;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
  std::vector<std::uint8_t> out(hex.size() / 2);
  std::size_t len = 0;
  if (hex.size() % 2 != 0 ||
      sodium_hex2bin(out.data(), out.size(), hex.data(), hex.size(), nullptr, &len, nullptr) != 0 || len != out.size()) {
    return {};
  }
  return out;
}

std::string base(const std::string& destination) {
  std::string d = destination;
  while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
  return d + "/.mediaviewer";
}

// Exactly the bytes a record's signature covers; each text field has its
// length first, as current.json's does.
std::string move_message(std::string_view library_id, std::uint64_t epoch, std::string_view from_hex_key, std::string_view to_hex_key) {
  const auto number = [](std::uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
    return s;
  };
  std::string m = "mediaviewer.catalog.keymove.v1";
  m += number(library_id.size());
  m += library_id;
  m += number(epoch);
  m += number(from_hex_key.size());
  m += from_hex_key;
  m += number(to_hex_key.size());
  m += to_hex_key;
  return m;
}

struct chain_row {
  std::string from, to, signature;
  std::uint64_t epoch = 0;
};

std::vector<chain_row> read_chain(const json::value& doc) {
  std::vector<chain_row> out;
  const json::value* list = doc.find("chain");
  if (!list || list->k != json::kind::array) return out;
  for (const json::value& v : list->a) {
    const std::string* from = v.str("from_key");
    const std::string* to = v.str("to_key");
    const std::string* signature = v.str("signature");
    const auto epoch = v.integer("epoch");
    if (!from || !to || !signature || !epoch || *epoch < 0) continue;  // the walk below cannot use it, and stops there
    out.push_back({*from, *to, *signature, static_cast<std::uint64_t>(*epoch)});
  }
  return out;
}

result<std::string> write_library(const fs::path& file, const library_info& l, const std::vector<chain_row>& chain) {
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(l.library_id);
  w.key("public_key").string(l.public_key);
  w.key("epoch").integer(static_cast<std::int64_t>(l.epoch));
  if (l.restarted) w.key("restarted").boolean(true);
  w.key("chain").begin_array();
  for (const chain_row& r : chain) {
    w.begin_object();
    w.key("from_key").string(r.from);
    w.key("to_key").string(r.to);
    w.key("epoch").integer(static_cast<std::int64_t>(r.epoch));
    w.key("signature").string(r.signature);
    w.end_object();
  }
  w.end_array();
  w.end_object();
  if (!replace_file(file, w.str())) return err(status::io);
  return w.take();
}

}  // namespace

result<library_info> read_library(const std::string& destination) {
  const fs::path file = fs_path(base(destination)) / "library.json";
  std::error_code ec;
  if (!fs::exists(file, ec)) return err(ec ? status::io : status::not_found);
  const auto doc = json::parse(slurp(file), 6);
  const std::string* id = doc ? doc->str("library_id") : nullptr;
  const std::string* key = doc ? doc->str("public_key") : nullptr;
  if (!id || !key) return err(status::corrupt);
  library_info l;
  l.library_id = *id;
  l.public_key = *key;
  l.epoch = static_cast<std::uint64_t>(std::max<std::int64_t>(1, doc->integer("epoch").value_or(1)));
  l.restarted = doc->boolean("restarted").value_or(false);
  return l;
}

result<followed_key> follow_chain(std::string_view library_json, std::string_view library_id, std::span<const std::uint8_t> pinned,
                                  std::uint64_t pinned_epoch) {
  (void)sodium_init();
  if (pinned.size() != crypto_sign_ed25519_PUBLICKEYBYTES) return err(status::invalid_arg);
  const auto doc = json::parse(library_json, 6);
  if (!doc) return err(status::corrupt);
  const std::string* id = doc->str("library_id");
  const std::string* current = doc->str("public_key");
  if (!id || !current) return err(status::corrupt);
  if (*id != library_id) return err(status::permission_denied);

  std::string at = to_hex(pinned);
  std::uint64_t epoch = pinned_epoch;
  for (const chain_row& r : read_chain(*doc)) {
    // Records older than where this phone stands are behind it. Only a record
    // that moves on from the key it holds counts, and only upward.
    if (r.from != at || r.epoch <= epoch) continue;
    const auto signature = from_hex(r.signature);
    const auto from_key = from_hex(r.from);
    if (signature.size() != crypto_sign_ed25519_BYTES || from_key.size() != crypto_sign_ed25519_PUBLICKEYBYTES) {
      return err(status::permission_denied);
    }
    const std::string m = move_message(library_id, r.epoch, r.from, r.to);
    if (crypto_sign_ed25519_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(m.data()), m.size(), from_key.data()) != 0) {
      return err(status::permission_denied);
    }
    at = r.to;
    epoch = r.epoch;
  }
  // However the file got here, it has to end on the key this phone reached.
  if (at != *current) return err(status::permission_denied);
  const auto now_epoch = doc->integer("epoch").value_or(1);
  if (now_epoch < 1 || static_cast<std::uint64_t>(now_epoch) < epoch) return err(status::permission_denied);
  followed_key out;
  out.public_key = from_hex(at);
  out.epoch = epoch;
  if (out.public_key.size() != crypto_sign_ed25519_PUBLICKEYBYTES) return err(status::permission_denied);
  return out;
}

result<library_info> hand_over(const std::string& destination, std::span<const std::uint8_t> secret, std::span<const std::uint8_t> to_public) {
  (void)sodium_init();
  if (to_public.size() != kPublicKeyBytes) return err(status::invalid_arg);
  const std::vector<std::uint8_t> from = public_of(secret);
  if (from.empty()) return err(status::invalid_arg);
  MV_TRY(library_info l, read_library(destination));
  if (l.public_key != to_hex(from)) return err(status::permission_denied);
  const fs::path file = fs_path(base(destination)) / "library.json";
  const auto doc = json::parse(slurp(file), 6);
  if (!doc) return err(status::corrupt);
  auto chain = read_chain(*doc);

  chain_row row;
  row.from = l.public_key;
  row.to = to_hex(to_public);
  row.epoch = l.epoch + 1;
  const std::string m = move_message(l.library_id, row.epoch, row.from, row.to);
  std::vector<std::uint8_t> signature(crypto_sign_ed25519_BYTES);
  if (crypto_sign_ed25519_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(m.data()), m.size(), secret.data()) != 0) {
    return err(status::internal);
  }
  row.signature = to_hex(signature);
  chain.push_back(std::move(row));

  l.public_key = to_hex(to_public);
  l.epoch += 1;
  l.restarted = false;
  MV_TRY(const std::string written, write_library(file, l, chain));
  (void)written;
  clear_takeover(destination);
  return l;
}

result<library_info> restart_publisher(const std::string& destination, std::span<const std::uint8_t> to_public) {
  if (to_public.size() != kPublicKeyBytes) return err(status::invalid_arg);
  MV_TRY(library_info l, read_library(destination));
  const fs::path file = fs_path(base(destination)) / "library.json";
  l.public_key = to_hex(to_public);
  l.epoch += 1;
  l.restarted = true;
  // The old records could only mislead: nothing leads from them to this key.
  MV_TRY(const std::string written, write_library(file, l, {}));
  (void)written;
  clear_takeover(destination);
  return l;
}

expected request_takeover(const std::string& destination, std::span<const std::uint8_t> public_key, const std::string& name) {
  if (public_key.size() != kPublicKeyBytes) return err(status::invalid_arg);
  json::writer w;
  w.begin_object().key("public_key").string(to_hex(public_key)).key("name").string(name).end_object();
  if (!replace_file(fs_path(base(destination)) / "takeover.request", w.str())) return err(status::io);
  return {};
}

result<takeover_request> read_takeover(const std::string& destination) {
  const fs::path file = fs_path(base(destination)) / "takeover.request";
  std::error_code ec;
  if (!fs::exists(file, ec)) return err(ec ? status::io : status::not_found);
  const auto doc = json::parse(slurp(file), 4);
  const std::string* key = doc ? doc->str("public_key") : nullptr;
  const std::string* name = doc ? doc->str("name") : nullptr;
  if (!key || !name) return err(status::corrupt);
  return takeover_request{*key, *name};
}

void clear_takeover(const std::string& destination) {
  std::error_code ec;
  fs::remove(fs_path(base(destination)) / "takeover.request", ec);
}

namespace {

std::string gen_folder(std::uint64_t g) {
  std::string s = std::to_string(g);
  return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
}

// A link where the disk allows it: a model file is hundreds of megabytes.
[[nodiscard]] bool place(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  ec.clear();
  fs::remove(to, ec);
  ec.clear();
  fs::create_hard_link(from, to, ec);
  if (!ec) return true;
  ec.clear();
  return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec) && !ec;
}

}  // namespace

expected prepare_nearby(const nearby_options& o) {
  (void)sodium_init();
  if (o.secret_key.size() != kSecretKeyBytes || o.public_key.size() != kPublicKeyBytes) return err(status::invalid_arg);
  const fs::path dir = fs_path(o.dir);
  std::error_code ec;
  fs::create_directories(dir / "gen", ec);
  if (ec) return err(status::io);
  // Newest wins here too: an older generation never replaces a newer one.
  if (fs::exists(dir / "current.json", ec)) {
    const auto cur = json::parse(slurp(dir / "current.json"), 4);
    const auto had = cur ? cur->integer("generation") : std::nullopt;
    if (had && *had >= 0 && static_cast<std::uint64_t>(*had) > o.generation) return err(status::invalid_arg);
  }
  const std::string name = gen_folder(o.generation);
  const fs::path part = dir / "gen" / (name + ".part");
  const fs::path done = dir / "gen" / name;
  fs::remove_all(part, ec);
  ec.clear();
  fs::create_directories(part, ec);
  if (ec) return err(status::io);
  for (fs::directory_iterator it(fs_path(o.gen_dir), ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || ec) return err(status::io);
    if (!place(it->path(), part / it->path().filename())) return err(status::io);
  }
  if (ec) return err(status::io);
  for (const shared_file& f : o.shared) {
    if (!f.rel.starts_with("models/")) continue;  // the packs of 512 px tiles stay the NAS's
    if (f.rel.find("..") != std::string::npos) return err(status::invalid_arg);
    if (!place(fs_path(f.source), dir / fs_path(f.rel))) return err(status::io);
  }
  // The folder is whole before anything names it.
  fs::remove_all(done, ec);
  ec.clear();
  fs::rename(part, done, ec);
  if (ec) return err(status::io);
  const auto manifest = sha256_file_hex(o.gen_dir + "/manifest.json");
  if (!manifest) return err(status::io);
  current_info info;
  info.library_id = o.library_id;
  info.epoch = o.epoch;
  info.generation = o.generation;
  info.manifest_sha256 = *manifest;
  const std::string current = signed_current_json(info, o.secret_key);
  if (current.empty()) return err(status::internal);
  if (!o.library_json.empty() && !replace_file(dir / "library.json", o.library_json)) return err(status::io);
  if (!replace_file(dir / "current.json", current)) return err(status::io);
  // A phone may still be copying the one before; nothing older.
  std::map<std::uint64_t, fs::path> gens;
  for (fs::directory_iterator it(dir / "gen", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    std::uint64_t g = 0;
    const auto [stop, why] = std::from_chars(n.data(), n.data() + n.size(), g);
    if (why == std::errc{} && stop == n.data() + n.size()) gens[g] = it->path();
  }
  while (gens.size() > 2) {
    std::error_code e;
    fs::remove_all(gens.begin()->second, e);
    gens.erase(gens.begin());
  }
  return {};
}

std::string pair_check(std::string_view library_id, std::string_view library_name,
                       std::span<const std::uint8_t> publisher_key, std::span<const std::uint8_t> service_identity,
                       std::span<const std::uint8_t> access_key) {
  (void)sodium_init();
  if (publisher_key.size() != 32 || service_identity.size() != 32 || access_key.size() != 32) return {};
  const auto length = [](std::uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
    return s;
  };
  std::string m = "mediaviewer.pair.check.v1";
  m += length(library_id.size());
  m += library_id;
  m += length(library_name.size());
  m += library_name;
  for (const auto key : {publisher_key, service_identity, access_key}) m.append(reinterpret_cast<const char*>(key.data()), key.size());
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(m.data()), m.size());
  static constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  std::string out;
  for (int i = 0; i < 20; ++i) {
    unsigned v = 0;
    for (int b = 0; b < 5; ++b) {
      const int bit = i * 5 + b;
      v = (v << 1) | ((h[bit / 8] >> (7 - bit % 8)) & 1u);
    }
    if (i != 0 && i % 5 == 0) out += '-';
    out += kAlphabet[v];
  }
  return out;
}

std::string pair_url(const pair_payload& p) {
  (void)sodium_init();
  json::writer w;
  w.begin_object();
  w.key("v").integer(1);
  w.key("library_id").string(p.library_id);
  w.key("library_name").string(p.library_name);
  w.key("publisher_key").string(to_hex(p.publisher_key));
  w.key("service").begin_object();
  w.key("local").string(p.service_local);
  w.key("public").string(p.service_public);
  w.key("identity").string(p.service_identity_hex);
  w.end_object();
  w.key("access_key").string(p.access_key_hex);
  w.key("code").string(p.code);
  w.key("expires").integer(p.expires);
  w.end_object();
  const std::string& text = w.str();
  std::string b64(sodium_base64_ENCODED_LEN(text.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(b64.data(), b64.size(), reinterpret_cast<const unsigned char*>(text.data()), text.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  b64.resize(std::char_traits<char>::length(b64.c_str()));
  return "mediaviewer://pair#" + b64;
}

}  // namespace mv::ai::catalog
