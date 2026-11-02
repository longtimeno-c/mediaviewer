// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/provider_verdict.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace mv::ai {
namespace {

constexpr const char* kHeader = "mv-ai-provider 1";

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

void write_all(std::ofstream& out, const std::map<std::string, provider_verdict>& all) {
  out << kHeader << '\n';
  for (const auto& [k, e] : all) out << k << '\t' << e.outcome << '\t' << e.open_s << '\t' << e.detail << '\n';
}

}  // namespace

std::string verdict_key(std::string_view runtime_version, std::string_view provider, std::string_view spec_key) {
  std::string key;
  key.reserve(runtime_version.size() + provider.size() + spec_key.size() + 2);
  key.append(runtime_version).append("|").append(provider).append("|").append(spec_key);
  return key;
}

provider_verdict verdict_of(bool passed, std::string_view fault, double open_s, std::string detail) {
  if (passed) return provider_verdict{"ok", open_s, std::string()};
  return provider_verdict{std::string(fault), open_s, std::move(detail)};
}

std::map<std::string, provider_verdict> read_verdicts(const std::string& path) {
  std::map<std::string, provider_verdict> out;
  std::ifstream in(fs_path(path), std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != kHeader) return out;
  while (std::getline(in, line)) {
    const std::size_t a = line.find('\t');
    const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    const std::size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
    if (c == std::string::npos) continue;
    provider_verdict v;
    v.outcome = line.substr(a + 1, b - a - 1);
    v.open_s = std::atof(line.substr(b + 1, c - b - 1).c_str());
    v.detail = line.substr(c + 1);
    out[line.substr(0, a)] = std::move(v);
  }
  return out;
}

void provider_verdicts::record(const std::string& key, provider_verdict v) {
  for (char& ch : v.detail) {
    if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
  }
  if (v.detail.size() > 400) v.detail.resize(400);
  std::lock_guard lock(m_);
  auto all = read_verdicts(path_);
  all[key] = std::move(v);
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
    if (!out) return;
    write_all(out, all);
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(fs_path(tmp), fs_path(path_), ec);
}

void provider_verdicts::forget(const std::string& key) {
  std::lock_guard lock(m_);
  auto all = read_verdicts(path_);
  if (all.erase(key) == 0) return;
  std::ofstream out(fs_path(path_), std::ios::binary | std::ios::trunc);
  write_all(out, all);
}

std::optional<provider_verdict> provider_verdicts::find(const std::string& key) const {
  std::lock_guard lock(m_);
  auto all = read_verdicts(path_);
  auto it = all.find(key);
  if (it == all.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> provider_verdicts::failure(const std::string& key) const {
  auto v = find(key);
  if (!v || v->outcome == "ok") return std::nullopt;
  return v->detail;
}

}  // namespace mv::ai
