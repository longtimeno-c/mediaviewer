// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// How an accelerated provider did with a tower on this machine (2026-10-05,
// docs/design/17 "Auto never runs the large tower on CPU because a provider
// failed it"), kept in <data>/provider.txt, one line per
// "<runtime>|<provider>|<spec>": "ok" or the fault's name, the open's seconds,
// and the provider's message (session::last_error, paths replaced). Auto reads
// it so a large tower that failed here is not run on CPU at a twelfth of the
// small one's speed, nor compiled again (minutes, gigabytes) every start. A new
// runtime is a new key and tries again. Holds nothing of the user's.
//
// Plain files and strings, no ONNX Runtime: pack.cpp records and asks, and the
// tests (tests/test_ai_provider_verdict.cpp) drive the same rule without a
// runtime or a model.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mv::ai {

struct provider_verdict {
  std::string outcome;  // "ok" or infer::fault_name
  double open_s = -1;
  std::string detail;
};

// "<runtime version>|<provider name>|<spec key>".
[[nodiscard]] std::string verdict_key(std::string_view runtime_version, std::string_view provider,
                                      std::string_view spec_key);

// What an open of a tower on an accelerated provider leaves: "ok" with no
// message when it opened and passed the self-test, else the fault's name
// (infer::fault_name) and the provider's message.
[[nodiscard]] provider_verdict verdict_of(bool passed, std::string_view fault, double open_s,
                                          std::string detail);

// Every verdict in a provider.txt; empty when it is missing or not version 1.
// A line without its four fields is skipped.
[[nodiscard]] std::map<std::string, provider_verdict> read_verdicts(const std::string& path);

// The verdicts of one provider.txt. Each call reads the file, so a second
// instance on the same path (the next start) sees what this one wrote. Calls
// on one instance are serialised; any thread.
class provider_verdicts {
 public:
  explicit provider_verdicts(std::string path) : path_(std::move(path)) {}
  provider_verdicts(const provider_verdicts&) = delete;
  provider_verdicts& operator=(const provider_verdicts&) = delete;

  // Records (replaces) the verdict for key. The message goes on one line
  // (tabs and line breaks become spaces) of at most 400 bytes; written to a
  // temporary file and renamed over the old.
  void record(const std::string& key, provider_verdict v);
  // Forgets key's verdict (Settings' "Try the larger model again"). Nothing
  // recorded: the file is left alone.
  void forget(const std::string& key);
  [[nodiscard]] std::optional<provider_verdict> find(const std::string& key) const;
  // The rule Auto goes by (engine_deps::accelerated_failure): the recorded
  // message when the provider failed key here, else nullopt (it passed, or was
  // never tried).
  [[nodiscard]] std::optional<std::string> failure(const std::string& key) const;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
  mutable std::mutex m_;
};

}  // namespace mv::ai
