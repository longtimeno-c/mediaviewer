// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over a folder of files: the tests, and the portable build where
// there is no OS store. Names are hex-encoded into file names.
#include <cstdio>
#include <fstream>
#include <iterator>

#include "addons/driftwire/secrets.h"

namespace mv::driftwire::secrets {

namespace {

class folder_store final : public store {
 public:
  explicit folder_store(std::string folder) : folder_(std::move(folder)) {}
  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue) return false;
    std::ofstream f(path(name), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(f);
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    std::ifstream f(path(name), std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<std::uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (v.size() > kMaxValue) return std::nullopt;
    return v;
  }
  bool erase(const std::string& name) override { return std::remove(path(name).c_str()) == 0; }

 private:
  std::string path(const std::string& name) const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string p = folder_ + "/secret-";
    for (const unsigned char c : name) {
      p.push_back(kHex[c >> 4]);
      p.push_back(kHex[c & 15]);
    }
    return p;
  }
  std::string folder_;
};

}  // namespace

std::unique_ptr<store> file_store(const std::string& folder) { return std::make_unique<folder_store>(folder); }

#if !defined(__APPLE__) && !defined(_WIN32)
std::unique_ptr<store> os_store(const std::string&) { return nullptr; }
std::unique_ptr<store> os_store() { return os_store(kAppService); }
#endif

}  // namespace mv::driftwire::secrets
