// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/photos_backup_mac.h"

#include "shell/photos_items_mac.h"

namespace mv::shell::backup {
namespace {

class photokit_source final : public source {
 public:
  result<std::vector<asset_file>> enumerate(const std::atomic<bool>* cancel) override {
    auto files = photos::enumerate_files(cancel);
    if (!files) return err(files.error());
    std::vector<asset_file> out;
    out.reserve(files->size());
    for (auto& f : files.value()) {
      asset_file a;
      a.id = std::move(f.key);
      a.kind = static_cast<file_kind>(f.kind);
      a.filename = std::move(f.name);
      a.created_unix = f.created;
      out.push_back(std::move(a));
    }
    return out;
  }

  result<std::string> local_file(const asset_file& file) override {
    return photos::local_original(file.id, static_cast<int>(file.kind));
  }

  expected fetch(const asset_file& file, const std::string& tmp_utf8, const std::atomic<bool>* cancel) override {
    return photos::fetch_file(file.id, static_cast<int>(file.kind), tmp_utf8, cancel);
  }
};

}  // namespace

std::unique_ptr<source> make_photos_source() { return std::make_unique<photokit_source>(); }

}  // namespace mv::shell::backup
