// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/pipeline_mac.h"

#include "image/pipeline.h"

namespace mv::image {

result<display_image> decode_bytes_mac(std::span<const std::uint8_t> bytes,
                                       const job_context* ctx) {
  // The full D5 still set now builds on Darwin, so this is just the portable
  // entry (image/pipeline.cpp); the _mac name stays for the callers written
  // when only JPEG/PNG/BMP existed here.
  return decode_bytes(bytes, ctx);
}

}  // namespace mv::image
