// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Text in images on the Mac: shell/text_in_image.h's port, implemented with
// Vision's VNRecognizeTextRequest (accurate, language correction on), each
// word boxed with boundingBoxForRange:. On
// device; nothing is sent anywhere (rule 6). macOS only.
#pragma once

#include "shell/text_in_image.h"

namespace mv::shell {

// Stateless; one per job is fine. Worker thread only.
class vision_text_recognizer final : public text_recognizer {
 public:
  [[nodiscard]] text_recognition recognise(const text_image& image, text_layout& layout) noexcept override;
};

}  // namespace mv::shell
