// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where a model's files are (docs/plans/model-tiers.md T3). The signed pack
// carries each model folder's model.json and our own exports; the upstream
// files the add-on fetched from Hugging Face sit under its data folder in the
// same layout. A file model.json names is taken from the model's own folder
// when it is there (sideloaded and older packs carry everything), else from
// the fetched one. Header-only: src/infer links nothing for it.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace mv::infer {

[[nodiscard]] inline std::string model_join(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

[[nodiscard]] inline std::string model_file(const std::string& folder, const std::string& fetched,
                                            const std::string& name) {
  const std::string own = model_join(folder, name);
  if (fetched.empty()) return own;
  std::error_code ec;
  if (std::filesystem::exists(std::filesystem::path(std::u8string(own.begin(), own.end())), ec)) return own;
  return model_join(fetched, name);
}

// A sibling folder (a shared tokenizer) of both.
[[nodiscard]] inline std::string fetched_sibling(const std::string& fetched, const std::string& name) {
  return fetched.empty() ? std::string() : model_join(model_join(fetched, ".."), name);
}

}  // namespace mv::infer
