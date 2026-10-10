// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/clipboard_image.h"

#include "codec/dib.h"
#include "codec/format.h"
#include "io/file_port.h"

namespace mv::shell {

result<std::string> adopt_clipboard_image(std::vector<std::uint8_t> bytes, clipboard_flavor flavor) {
  if (flavor == clipboard_flavor::packed_dib) {
    auto bmp = codec::dib::bmp_from_packed(bytes);
    if (!bmp) return err(bmp.error() == status::out_of_memory ? status::out_of_memory : status::unsupported_format);
    bytes = std::move(bmp).value();
  }
  switch (codec::probe(bytes)) {
    case codec::format_family::jpeg:
    case codec::format_family::png:
    case codec::format_family::bmp:
    case codec::format_family::gif:
    case codec::format_family::webp:
    case codec::format_family::tiff:
    case codec::format_family::ico:
    case codec::format_family::heic:
    case codec::format_family::avif:
      break;
    default:
      return err(status::unsupported_format);
  }
  return io::put_memory_file(std::move(bytes), kClipboardItemName);
}

std::string unsaved_copy_name(std::string_view item_path, bool png) {
  std::string out(io::file_name_of(item_path));
  if (out.empty()) out = kClipboardItemName;
  out += png ? ".png" : ".jpg";
  return out;
}

}  // namespace mv::shell
