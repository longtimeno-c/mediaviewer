// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/convert_run.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "addons/driftwire/cli/host_proc.h"
#include "core/json.h"

namespace mv::driftwire::cli {

namespace fs = std::filesystem;
namespace cv = serve::convert;

namespace {

fs::path fs_path(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

constexpr int kCheckMs = 20000;
constexpr int kPlanMs = 60000;    // a probe of a clip on a slow drive
constexpr int kSegmentMs = 120000;

// The helper's exit codes (tools/clipjob/main.cpp): 0 done, 2 not a clip, 3 no hardware encoder.
cv::ran run(const std::string& helper, const std::vector<std::string>& args, int timeout_ms) {
  auto child = child_process::start(helper, args);
  if (!child) return cv::ran::failed;
  if (!child->wait(timeout_ms)) {
    child->kill();
    (void)child->wait(5000);
    return cv::ran::failed;
  }
  const auto e = child->how();
  if (e.crashed) return cv::ran::failed;
  return e.code == 0 ? cv::ran::ok : e.code == 2 ? cv::ran::not_a_clip : cv::ran::failed;
}

}  // namespace

convert_check check_converter(const std::string& helper, const std::string& scratch_dir) {
  convert_check c;
  std::error_code ec;
  if (helper.empty() || !fs::is_regular_file(fs_path(helper), ec)) return c;
  fs::create_directories(fs_path(scratch_dir), ec);
  const std::string out = scratch_dir + "/convert-check.json";
  fs::remove(fs_path(out), ec);
  if (run(helper, {"--convert-check", out}, kCheckMs) != cv::ran::ok) return c;
  std::ifstream f(fs_path(out), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();
  fs::remove(fs_path(out), ec);
  const auto doc = json::parse(text, 2);
  const std::string* enc = doc ? doc->str("encoder") : nullptr;
  const std::string* fam = doc ? doc->str("family") : nullptr;
  if (!enc || enc->empty() || enc->size() > 64) return c;
  c.ok = true;
  c.encoder = *enc;
  c.family = fam && fam->size() <= 64 ? *fam : *enc;
  return c;
}

cv::runner helper_runner(const std::string& helper) {
  cv::runner r;
  r.plan = [helper](const std::string& src, std::uint32_t segment_ms, const std::string& out) {
    return run(helper, {"--convert-plan", src, std::to_string(segment_ms), out}, kPlanMs);
  };
  r.segment = [helper](const std::string& src, std::uint32_t k, std::uint32_t segment_ms, const std::string& out) {
    return run(helper, {"--convert-segment", src, std::to_string(k), std::to_string(segment_ms), out}, kSegmentMs);
  };
  return r;
}

std::shared_ptr<cv::service> start_converter(const std::string& helper, const std::string& cache_dir, convert_check* found) {
  const convert_check c = check_converter(helper, cache_dir);
  if (found) *found = c;
  if (!c.ok) return nullptr;
  cv::service::options o;
  o.cache_dir = cache_dir;
  o.run = helper_runner(helper);
  return std::shared_ptr<cv::service>(cv::service::start(std::move(o)));
}

}  // namespace mv::driftwire::cli
