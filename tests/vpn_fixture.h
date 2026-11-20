// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN add-on's service (src/addons/vpn/service.h) as the tests drive it:
// a host that keeps what was posted, and a Phone access that answers as told.
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "addons/vpn/service.h"

namespace vpn_test {

namespace vpn = mv::driftwire::vpn;

class fake_host final : public vpn::service_host {
 public:
  // Phone access: (method, json) -> its answer; nullopt: not installed.
  std::function<std::optional<std::string>(const std::string&, const std::string&)> phone;

  void post_settings(const std::string& tree) override {
    std::lock_guard lock(m_);
    settings_ = tree;
  }
  bool settings_shown() override {
    std::lock_guard lock(m_);
    return shown_;
  }
  void log(int, const char*) override {}
  bool keep_alive(bool on, const std::string& label) override {
    std::lock_guard lock(m_);
    kept_ = on;
    label_ = label;
    return true;
  }
  bool post_menu(const std::string& menu) override {
    std::lock_guard lock(m_);
    menu_ = menu;
    return true;
  }
  std::optional<std::string> ask_phone(const std::string& method, const std::string& json) override {
    std::function<std::optional<std::string>(const std::string&, const std::string&)> p;
    {
      std::lock_guard lock(m_);
      asked_.emplace_back(method, json);
      p = phone;
    }
    return p ? p(method, json) : std::nullopt;
  }

  void show(bool on) {
    std::lock_guard lock(m_);
    shown_ = on;
  }
  std::string settings() {
    std::lock_guard lock(m_);
    return settings_;
  }
  std::string menu() {
    std::lock_guard lock(m_);
    return menu_;
  }
  bool kept() {
    std::lock_guard lock(m_);
    return kept_;
  }
  std::string label() {
    std::lock_guard lock(m_);
    return label_;
  }
  std::vector<std::pair<std::string, std::string>> asked() {
    std::lock_guard lock(m_);
    return asked_;
  }

 private:
  std::mutex m_;
  bool shown_ = true;
  bool kept_ = false;
  std::string settings_, menu_, label_;
  std::vector<std::pair<std::string, std::string>> asked_;
};

}  // namespace vpn_test
