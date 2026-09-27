// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by every hwencode_*.cpp: the label a host shows for an encoder.
#include <cstring>

namespace mv::edit::hwencode {

const char* family_label(const char* name) noexcept {
  if (name == nullptr) return "";
  if (std::strstr(name, "nvenc")) return "NVENC";
  if (std::strstr(name, "qsv")) return "Quick Sync";
  if (std::strstr(name, "amf")) return "AMF";
  if (std::strstr(name, "_mf")) return "Media Foundation";
  if (std::strstr(name, "videotoolbox")) return "VideoToolbox";
  return name;
}

}  // namespace mv::edit::hwencode
