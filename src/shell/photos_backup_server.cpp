// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/photos_backup_server.h"

#include <algorithm>
#include <unordered_set>

#include "io/content_hash.h"
#include "io/file_port.h"
#include "mobile/link.h"
#include "remote/key.h"
#include "remote/library.h"

namespace mv::shell::backup {
namespace {

class remote_folder final : public server_folder {
 public:
  remote_folder(remote::library& lib, remote::key_parts at) : lib_(lib), at_(std::move(at)) {}

  bool reachable() override {
    // One listing: the server answers now (a kept listing of an unreachable one is not an
    // answer), and with it what this computer may do there is known.
    const std::string key = remote::make_key(at_);
    const auto listed = lib_.list(key);
    return listed && !listed->stale && lib_.can_write(key);
  }

  void stop() noexcept override { stop_.stop.store(true, std::memory_order_release); }

  result<bool> has(const std::string& place) override { return lib_.exists(key_of(place, false)); }

  result<std::string> put(const std::string& local_utf8, const std::string& sub, const std::string& name,
                          const std::atomic<bool>* cancel,
                          const std::function<void(std::uint64_t delta)>& on_bytes) override {
    MV_TRY_VOID(ensure(sub));
    if (cancel && cancel->load(std::memory_order_acquire)) stop();
    if (stop_.stopped()) return err(status::cancelled);
    std::uint64_t last = 0;
    auto added = lib_.add_file(key_of(sub, true), local_utf8, name, &stop_, [&](std::uint64_t sent) {
      if (cancel && cancel->load(std::memory_order_acquire)) stop();
      if (sent > last) {
        if (on_bytes) on_bytes(sent - last);
        last = sent;
      }
    });
    if (!added) return err(added.error());
    return remote::leaf_name(*added);
  }

 private:
  std::string key_of(const std::string& place, bool folder) const {
    remote::key_parts k = at_;
    if (!place.empty()) k.rel = k.rel.empty() ? place : k.rel + "/" + place;
    k.folder = folder;
    return remote::make_key(k);
  }

  // Each folder of `sub`, made once a run; one already there is the one to use.
  expected ensure(const std::string& sub) {
    if (sub.empty() || made_.count(sub) > 0) return {};
    std::string done;
    std::size_t from = 0;
    while (from <= sub.size()) {
      const std::size_t slash = std::min(sub.find('/', from), sub.size());
      const std::string part = sub.substr(from, slash - from);
      from = slash + 1;
      if (part.empty()) continue;
      const std::string parent = done;
      done = done.empty() ? part : done + "/" + part;
      if (made_.count(done) > 0) continue;
      remote::library::change_refused why;
      const auto made = lib_.make_folder(key_of(parent, true), part, &why);
      if (!made && why.why != decltype(why.why)::exists) return err(made.error());
      made_.insert(done);
    }
    return {};
  }

  remote::library& lib_;
  remote::key_parts at_;
  mobile::cancel_flag stop_;  // one run's: a new run makes a new folder
  std::unordered_set<std::string> made_;
};

}  // namespace

std::shared_ptr<server_folder> make_server_folder(remote::library& lib, std::string folder_key) {
  auto k = remote::parse_key(folder_key);
  if (!k || !k->folder || k->root.empty()) return nullptr;
  return std::make_shared<remote_folder>(lib, std::move(*k));
}

std::string server_state_dir(std::string_view base, std::string_view folder_key) {
  const io::content_hash h = io::hash_bytes(
      {reinterpret_cast<const std::uint8_t*>(folder_key.data()), folder_key.size()});
  return io::join_path(base, h.hex().substr(0, 32));
}

}  // namespace mv::shell::backup
