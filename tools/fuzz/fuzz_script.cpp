// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: arbitrary text as an add-on script, loaded into the
// sandbox under tight limits (docs/design/25 §9 A, PR 57). Each input is one
// add-on: its top level runs, then its function `f` if it defines one, with
// every permission granted so the host bindings are reached too.
//
// What it holds: no crash, no sanitizer report, no hang past the limits (the
// watchdog's bound), and the memory ceiling never exceeded. A script error, a
// load error and a stop are all fine outcomes.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "addon/open_manifest.h"
#include "addon/script_runtime.h"

namespace {

mv::addon::script_services fake_services() {
  mv::addon::script_services s;
  s.folder_count = [] { return 3u; };
  s.folder_path = [] { return std::string("/fuzz"); };
  s.folder_item_name = [](std::uint32_t i) {
    return i < 3 ? std::string("IMG_000") + static_cast<char>('0' + i) + ".JPG" : std::string();
  };
  s.current_path = [] { return std::string("/fuzz/IMG_0000.JPG"); };
  s.notify = [](const std::string&, const std::string&) {};
  s.log = [](const std::string&, int, const std::string&) {};
  return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace std::chrono_literals;
  mv::addon::script_config c;
  c.addon_id = "fuzz.script";
  c.permissions = mv::addon::kPermissionsAll;
  c.limits.memory_bytes = 8u << 20;
  c.limits.instructions = 2'000'000;
  c.limits.wall = 250ms;
  c.limits.queue = 4;
  c.sources.push_back({"fuzz.lua", std::string(reinterpret_cast<const char*>(data), size)});
  auto addon = mv::addon::script_addon::start(std::move(c), fake_services());
  // The top level, then `f`: each bounded by the limits above. Five seconds
  // is far past both; a wait that long is a hang, and a hang is a finding.
  if (!addon->wait_idle(5s)) std::abort();
  (void)addon->run_function("f", "f", mv::json::value{});
  if (!addon->wait_idle(5s)) std::abort();
  if (addon->status().memory_peak > (8u << 20)) std::abort();
  addon.reset();  // stops it and joins the worker
  return 0;
}
