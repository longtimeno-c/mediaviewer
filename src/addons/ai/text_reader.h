// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #403 — text in a picture, read by the operating system (docs/design/17 "Text
// in pictures"): Apple Vision's VNRecognizeTextRequest on the Mac,
// Windows.Media.Ocr on Windows, nothing elsewhere. No model ships for it and
// nothing leaves the machine: the OS reads the pixels in this process. A port
// like cloud_files.h, in the pack (D9: one interface, one half per host). The
// same port can serve scanned PDFs (#354) and Live Text in the viewer (#289).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "addons/ai/host.h"
#include "core/result.h"

namespace mv::ai {

class text_reader {
 public:
  virtual ~text_reader() = default;
  // The lines of text the OS found, top to bottom, as it read them (case
  // kept). Empty when there is none. [worker-thread]; the OS's own work, so
  // it may take a few hundred milliseconds on a large photo.
  [[nodiscard]] virtual result<std::vector<std::string>> read(const rgb_frame& image) = 0;
  // The index key of what read it ("ocr/vision/1"): a different reader, or a
  // new rule here, reads every photo again.
  [[nodiscard]] virtual const std::string& spec_key() const noexcept = 0;
};

// The platform's, or nullptr where there is none (or the OS has no
// recogniser for the user's languages).
[[nodiscard]] std::unique_ptr<text_reader> make_text_reader();

}  // namespace mv::ai
