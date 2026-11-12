// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue's generic half (catalog.h): keys, current.json, delivery to
// the share, retention and the pack readers. Building a generation from the
// index stays in Local search (addons/ai/catalog.cpp).
#include "catalogue/catalog.h"

#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#include "catalogue/sha256.h"
#include "core/json.h"

namespace mv::ai::catalog {
namespace fs = std::filesystem;
namespace {

fs::path fs_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8_of(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string trimmed(std::string path) {
#if defined(_WIN32)
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
#else
  while (path.size() > 1 && path.back() == '/') path.pop_back();
#endif
  return path;
}

std::string slurp(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A local file: deliver() copies it with a verified, flushed copy.
[[nodiscard]] bool write_text(const std::string& path, std::string_view text) {
  std::ofstream out(fs_path(path), std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  return static_cast<bool>(out);
}

bool cancelled(const control& c) { return c.cancelled && c.cancelled(); }
void pause_for_viewer(const control& c) {
  if (c.yield) c.yield();
}
void report(const control& c, double f) {
  if (c.report) c.report(f);
}

}  // namespace

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> make_keypair() {
  if (sodium_init() < 0) return {};  // no usable libsodium: no key (callers check the sizes)
  std::vector<std::uint8_t> pub(crypto_sign_ed25519_PUBLICKEYBYTES), sec(crypto_sign_ed25519_SECRETKEYBYTES);
  crypto_sign_ed25519_keypair(pub.data(), sec.data());
  return {std::move(pub), std::move(sec)};
}

std::vector<std::uint8_t> public_of(std::span<const std::uint8_t> secret) {
  if (secret.size() != crypto_sign_ed25519_SECRETKEYBYTES) return {};
  std::vector<std::uint8_t> pub(crypto_sign_ed25519_PUBLICKEYBYTES);
  if (crypto_sign_ed25519_sk_to_pk(pub.data(), secret.data()) != 0) return {};
  return pub;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15u]);
  }
  return out;
}

std::string fingerprint(std::span<const std::uint8_t> public_key) {
  sha256 h;
  h.update(public_key);
  const std::string hex = h.finish_hex();
  return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4);
}

namespace {

// Exactly the bytes a signature covers. Each text field has its length first,
// so no two different sets of fields can read as the same message.
std::string signed_message(const current_info& c) {
  const auto number = [](std::uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
    return s;
  };
  std::string m = "mediaviewer.catalog.current.v1";
  m += number(c.library_id.size()) + c.library_id;
  m += number(c.epoch);
  m += number(c.generation);
  m += number(c.manifest_sha256.size()) + c.manifest_sha256;
  return m;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  if (hex.size() % 2 != 0) return {};
  std::vector<std::uint8_t> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return {};
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return out;
}

}  // namespace

std::string signed_current_json(const current_info& c, std::span<const std::uint8_t> secret_key) {
  if (secret_key.size() != crypto_sign_ed25519_SECRETKEYBYTES) return {};
  const std::string message = signed_message(c);
  std::vector<std::uint8_t> signature(crypto_sign_ed25519_BYTES);
  if (crypto_sign_ed25519_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size(), secret_key.data()) != 0) {
    return {};
  }
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(c.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(c.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(c.generation));
  w.key("manifest_sha256").string(c.manifest_sha256);
  w.key("signature").string(to_hex(signature));
  w.end_object();
  return w.take();
}

result<current_info> verify_current(std::string_view text, std::span<const std::uint8_t> public_key) {
  if (sodium_init() < 0) return err(status::io);
  if (public_key.size() != crypto_sign_ed25519_PUBLICKEYBYTES) return err(status::invalid_arg);
  const auto doc = json::parse(text, 4);
  if (!doc) return err(status::corrupt);
  const std::string* id = doc->str("library_id");
  const std::string* manifest = doc->str("manifest_sha256");
  const std::string* signature = doc->str("signature");
  const auto epoch = doc->integer("epoch");
  const auto generation = doc->integer("generation");
  if (!id || !manifest || !signature || !epoch || !generation || *epoch < 0 || *generation < 0) return err(status::corrupt);
  const std::vector<std::uint8_t> sig = from_hex(*signature);
  if (sig.size() != crypto_sign_ed25519_BYTES) return err(status::corrupt);
  current_info c;
  c.library_id = *id;
  c.epoch = static_cast<std::uint64_t>(*epoch);
  c.generation = static_cast<std::uint64_t>(*generation);
  c.manifest_sha256 = *manifest;
  const std::string m = signed_message(c);
  if (crypto_sign_ed25519_verify_detached(sig.data(), reinterpret_cast<const unsigned char*>(m.data()), m.size(),
                                          public_key.data()) != 0) {
    return err(status::permission_denied);
  }
  return c;
}

namespace {

std::string base_of(const std::string& destination) { return trimmed(destination) + "/.mediaviewer"; }

// The generation directories present, by number; a .part or a stranger is not one.
std::vector<std::uint64_t> generations_in(const std::string& gen_root) {
  std::vector<std::uint64_t> out;
  std::error_code ec;
  for (fs::directory_iterator it(fs_path(gen_root), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    std::uint64_t g = 0;
    const char* const end_of_name = name.data() + name.size();
    const auto [stop, why] = std::from_chars(name.data(), end_of_name, g);
    if (name.empty() || why != std::errc{} || stop != end_of_name) continue;  // a stranger, or too long to be ours
    out.push_back(g);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string gen_name(std::uint64_t g) {
  std::string s = std::to_string(g);
  return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
}

// Every generation but the newest two goes once it is old enough, and so does
// a part folder a dead run left. Failing to tidy never fails a publish.
// When each generation became current, by this machine's clock: the share's
// file times are another clock, and a folder that never became current is no
// successor at all.
std::map<std::uint64_t, std::int64_t> read_arrivals(const std::string& file) {
  std::map<std::uint64_t, std::int64_t> out;
  if (file.empty()) return out;
  if (const auto doc = json::parse(slurp(file), 4); doc && doc->k == json::kind::object) {
    for (const auto& [g, v] : doc->o) {
      std::uint64_t n = 0;
      const auto [stop, why] = std::from_chars(g.data(), g.data() + g.size(), n);
      if (why == std::errc{} && stop == g.data() + g.size() && v.k == json::kind::number) out[n] = v.i;
    }
  }
  return out;
}

void write_arrivals(const std::string& file, const std::map<std::uint64_t, std::int64_t>& at) {
  if (file.empty()) return;
  json::writer w;
  w.begin_object();
  for (const auto& [g, when] : at) w.key(std::to_string(g)).integer(when);
  w.end_object();
  std::error_code ec;
  if (write_text(file + ".tmp", w.str())) fs::rename(fs_path(file + ".tmp"), fs_path(file), ec);
}

// Every generation but the newest two goes once the next one that became
// current has been current long enough; a dead run's part folder goes too.
// Failing to tidy never fails a publish.
void tidy(const deliver_options& o, const std::string& gen_root) {
  const auto gens = generations_in(gen_root);
  auto arrived = read_arrivals(o.arrivals_file);
  for (auto it = arrived.begin(); it != arrived.end();) {
    it = std::binary_search(gens.begin(), gens.end(), it->first) ? std::next(it) : arrived.erase(it);
  }
  write_arrivals(o.arrivals_file, arrived);
  std::error_code ec;
  // A reader may have started on a generation until the one after it arrived,
  // so the grace runs from then, not from when this one was written.
  for (std::size_t i = 0; i + 2 < gens.size(); ++i) {
    const auto next = arrived.upper_bound(gens[i]);
    if (next != arrived.end() && std::time(nullptr) - next->second >= o.keep_seconds) {
      fs::remove_all(fs_path(gen_root + "/" + gen_name(gens[i])), ec);
    }
  }
}

// A small file on the share, whole or not at all: the verified copy flushes it
// and reads it back under a part name, and only then does a rename replace.
expected put_durable(const deliver_options& o, const std::string& text, const std::string& final_path) {
  const std::string local = o.gen_dir + ".put";
  const std::string part = final_path + ".part";
  if (!write_text(local, text)) return err(status::io);
  std::error_code ec;
  fs::remove(fs_path(part), ec);
  const expected copied = o.copy(local, part);
  fs::remove(fs_path(local), ec);
  if (!copied) return copied;
  ec.clear();
  fs::rename(fs_path(part), fs_path(final_path), ec);
  return ec ? expected(err(status::io)) : expected{};
}

}  // namespace

result<std::uint64_t> published_generation(const std::string& destination, const std::string& library_id,
                                           std::span<const std::uint8_t> public_key) {
  std::error_code ec;
  if (!fs::is_directory(fs_path(destination), ec)) return err(status::io);
  const std::string base = base_of(destination);
  if (fs::exists(fs_path(base + "/library.json"), ec)) {
    // Only a file that reads and names someone else is another library's.
    const auto lib = json::parse(slurp(base + "/library.json"), 4);
    const std::string* id = lib ? lib->str("library_id") : nullptr;
    if (!id) return err(status::io);
    const std::string* key = lib->str("public_key");
    if (*id != library_id || !key || *key != to_hex(public_key)) return err(status::permission_denied);
  }
  if (!fs::exists(fs_path(base + "/current.json"), ec)) return std::uint64_t{0};
  const auto cur = json::parse(slurp(base + "/current.json"), 4);
  if (!cur) return std::uint64_t{0};
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, cur->integer("generation").value_or(0)));
}

expected deliver(const deliver_options& o, const control& c) {
  if (o.secret_key.size() != kSecretKeyBytes || o.public_key.size() != kPublicKeyBytes) return err(status::invalid_arg);
  MV_TRY(const std::uint64_t there, published_generation(o.destination, o.library_id, o.public_key));
  if (there >= o.generation) return err(status::invalid_arg);  // numbers only go up

  const std::string base = base_of(o.destination);
  const std::string gen_root = base + "/gen";
  const std::string name = gen_name(o.generation);
  const std::string part = gen_root + "/" + name + ".part";
  const std::string done = gen_root + "/" + name;
  MV_TRY_VOID(o.make_directories(gen_root));
  std::error_code ec;
  // Any part folder is a run that died: one publisher writes here (§5 rule 5),
  // and its room is what this one needs.
  for (fs::directory_iterator it(fs_path(gen_root), ec), end; !ec && it != end; it.increment(ec)) {
    if (utf8_of(it->path().filename()).ends_with(".part")) {
      std::error_code e2;
      fs::remove_all(it->path(), e2);
    }
  }
  ec.clear();
  MV_TRY_VOID(o.make_directories(part));

  if (!fs::exists(fs_path(base + "/library.json"), ec)) {
    json::writer w;
    w.begin_object();
    w.key("format").integer(kFormat);
    w.key("library_id").string(o.library_id);
    w.key("public_key").string(to_hex(o.public_key));
    w.end_object();
    MV_TRY_VOID(put_durable(o, w.str(), base + "/library.json"));
  }

  // What the generation will name is on the share before anything names it.
  // A file that is there at its full size came from here: the name holds its
  // hash and it arrived through a verified copy.
  for (const shared_file& f : o.shared) {
    if (cancelled(c)) return err(status::cancelled);
    pause_for_viewer(c);
    const std::string to = base + "/" + f.rel;
    std::error_code e;
    if (fs::exists(fs_path(to), e)) {
      if (static_cast<std::uint64_t>(fs::file_size(fs_path(to), e)) == f.size && !e) continue;
      fs::remove(fs_path(to), e);
    }
    if (!fs::exists(fs_path(f.source), e)) return err(status::not_found);
    MV_TRY_VOID(o.make_directories(utf8_of(fs_path(to).parent_path())));
    MV_TRY_VOID(o.copy(f.source, to));
  }

  // The manifest goes last, so a part folder with one has every file.
  std::vector<std::string> names;
  for (fs::directory_iterator it(fs_path(o.gen_dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = utf8_of(it->path().filename());
    if (n != "manifest.json") names.push_back(n);
  }
  if (ec) return err(status::io);
  std::sort(names.begin(), names.end());
  names.push_back("manifest.json");
  double i = 0;
  for (const std::string& n : names) {
    if (cancelled(c)) return err(status::cancelled);
    report(c, i++ / static_cast<double>(names.size()));
    MV_TRY_VOID(o.copy(o.gen_dir + "/" + n, part + "/" + n));
  }
  const auto manifest_hash = sha256_file_hex(part + "/manifest.json");
  if (!manifest_hash) return err(status::io);

  // The two renames are the commit: first the generation exists, then it is
  // current. A folder already under this name is a run that got no further:
  // current.json names an older one (checked above), so no reader is on it.
  fs::remove_all(fs_path(done), ec);
  ec.clear();
  fs::rename(fs_path(part), fs_path(done), ec);
  if (ec) return err(status::io);
  current_info info;
  info.library_id = o.library_id;
  info.epoch = o.epoch;
  info.generation = o.generation;
  info.manifest_sha256 = *manifest_hash;
  const std::string message = signed_message(info);
  std::vector<std::uint8_t> signature(crypto_sign_ed25519_BYTES);
  if (crypto_sign_ed25519_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size(), o.secret_key.data()) != 0) {
    return err(status::io);
  }
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(o.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(o.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(o.generation));
  w.key("manifest_sha256").string(*manifest_hash);
  w.key("signature").string(to_hex(signature));
  w.end_object();
  MV_TRY_VOID(put_durable(o, w.str(), base + "/current.json"));
  {
    auto arrived = read_arrivals(o.arrivals_file);
    // When it really became current: a long copy must not shorten the
    // predecessor's grace, which runs from here.
    arrived[o.generation] = static_cast<std::int64_t>(std::time(nullptr));
    write_arrivals(o.arrivals_file, arrived);
  }
  report(c, 1.0);
  tidy(o, gen_root);
  if (!o.retention_file.empty()) retain_shared(base, o.retention_file, o.now_unix);
  return {};
}

void retain_shared(const std::string& base, const std::string& state_file, std::int64_t now_unix,
                   std::int64_t unused_seconds) {
  // What the generations still on the share name. One that cannot be read
  // might name anything: this round deletes nothing.
  std::set<std::string> named;
  for (const std::uint64_t g : generations_in(base + "/gen")) {
    const auto m = json::parse(slurp(base + "/gen/" + gen_name(g) + "/manifest.json"), 8);
    if (!m || m->k != json::kind::object) return;
    for (const char* key : {"thumb_packs", "preview_packs", "model_files"}) {
      if (const json::value* list = m->find(key); list && list->k == json::kind::array) {
        for (const json::value& v : list->a) {
          if (const std::string* rel = v.str("path")) named.insert(*rel);
        }
      }
    }
  }
  std::map<std::string, std::int64_t> since;
  if (const auto kept = json::parse(slurp(state_file), 4); kept && kept->k == json::kind::object) {
    for (const auto& [rel, v] : kept->o) {
      if (v.k == json::kind::number) since[rel] = v.i;
    }
  }
  bool listed = true;
  std::map<std::string, std::int64_t> next;
  const fs::path root = fs_path(base);
  for (const char* dir : {"thumbs", "previews", "models"}) {
    std::error_code ec;
    if (!fs::is_directory(root / dir, ec)) continue;
    fs::recursive_directory_iterator it(root / dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code e;
      if (!it->is_regular_file(e) || e) continue;
      const std::string rel = utf8_of(fs::relative(it->path(), root, e));
      if (e || named.count(rel)) continue;
      const auto seen = since.find(rel);
      const std::int64_t first = seen == since.end() ? now_unix : seen->second;
      const bool rolling = std::string_view(dir) == "thumbs" && it->file_size(e) < kFoldBytes && !e;
      if (now_unix - first >= (rolling ? std::min(unused_seconds, kRollingUnusedSeconds) : unused_seconds)) {
        fs::remove(it->path(), e);
      } else {
        next[rel] = first;
      }
    }
    if (ec) listed = false;
  }
  if (!listed) return;  // a share that dropped mid-listing keeps every date it had
  json::writer w;
  w.begin_object();
  for (const auto& [rel, first] : next) w.key(rel).integer(first);
  w.end_object();
  const std::string tmp = state_file + ".tmp";
  std::error_code e2;
  if (write_text(tmp, w.str())) fs::rename(fs_path(tmp), fs_path(state_file), e2);
}

namespace {
std::uint64_t get_u(std::span<const std::uint8_t> b, std::size_t at, int bytes) {
  std::uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | b[at + static_cast<std::size_t>(i)];
  return v;
}
}  // namespace

bool valid_pack_name(std::string_view n) {
  // t-<32 lower hex>.pack: never a path, whatever a damaged index says.
  if (n.size() != 39 || !n.starts_with("t-") || !n.ends_with(".pack")) return false;
  return std::all_of(n.begin() + 2, n.begin() + 34, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool within_pack(const thumb_location& at, std::uint64_t pack_size) {
  return at.offset <= pack_size && at.length <= pack_size - at.offset;
}

result<std::optional<thumb_location>> find_thumb(std::span<const std::uint8_t> b, std::int64_t asset) {
  if (b.size() < 16 || !std::equal(kIdxMagic, kIdxMagic + 8, b.begin())) return err(status::corrupt);
  const std::uint64_t packs = get_u(b, 8, 4), entries = get_u(b, 12, 4);
  const std::uint64_t table = 16 + packs * kPackNameBytes;
  if (table + entries * kIdxRow != b.size()) return err(status::corrupt);
  std::uint64_t lo = 0, hi = entries;
  while (lo < hi) {
    const std::uint64_t mid = lo + (hi - lo) / 2;
    const auto id = static_cast<std::int64_t>(get_u(b, table + mid * kIdxRow, 8));
    if (id < asset) {
      lo = mid + 1;
    } else if (id > asset) {
      hi = mid;
    } else {
      const std::size_t row = table + mid * kIdxRow;
      const std::uint64_t pack = get_u(b, row + 8, 4);
      if (pack >= packs) return err(status::corrupt);
      thumb_location out;
      const char* name = reinterpret_cast<const char*>(b.data()) + 16 + pack * kPackNameBytes;
      out.pack.assign(name, strnlen(name, kPackNameBytes));
      if (!valid_pack_name(out.pack)) return err(status::corrupt);
      out.length = static_cast<std::uint32_t>(get_u(b, row + 12, 4));
      out.offset = get_u(b, row + 16, 8);
      return std::optional<thumb_location>(std::move(out));
    }
  }
  return std::optional<thumb_location>();
}

result<std::optional<std::span<const std::uint8_t>>> find_tiny(std::span<const std::uint8_t> b, std::int64_t asset) {
  if (b.size() < 16 || !std::equal(kTinyMagic, kTinyMagic + 8, b.begin())) return err(status::corrupt);
  const std::uint64_t entries = get_u(b, 8, 4);
  if (16 + entries * kTinyRow > b.size()) return err(status::corrupt);
  std::uint64_t lo = 0, hi = entries;
  while (lo < hi) {
    const std::uint64_t mid = lo + (hi - lo) / 2;
    const std::size_t row = 16 + static_cast<std::size_t>(mid) * kTinyRow;
    const auto id = static_cast<std::int64_t>(get_u(b, row, 8));
    if (id < asset) {
      lo = mid + 1;
    } else if (id > asset) {
      hi = mid;
    } else {
      const std::uint64_t at = get_u(b, row + 8, 8), len = get_u(b, row + 16, 4);
      if (at > b.size() || len > b.size() - at) return err(status::corrupt);
      return std::optional<std::span<const std::uint8_t>>(b.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(len)));
    }
  }
  return std::optional<std::span<const std::uint8_t>>();
}

std::vector<shared_file> read_shared_list(const std::string& file) {
  std::ifstream in(fs_path(file), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  std::vector<shared_file> out;
  const auto doc = json::parse(ss.str(), 4);
  if (!doc || doc->k != json::kind::array) return out;
  for (const json::value& v : doc->a) {
    shared_file f;
    const std::string* source = v.str("source");
    const std::string* rel = v.str("rel");
    if (!source || !rel) continue;
    f.source = *source;
    f.rel = *rel;
    f.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, v.integer("size").value_or(0)));
    out.push_back(std::move(f));
  }
  return out;
}

}  // namespace mv::ai::catalog
