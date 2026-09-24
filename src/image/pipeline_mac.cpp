// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/pipeline_mac.h"

#include "codec/decode.h"
#include "image/pipeline.h"

namespace mv::image {

result<display_image> decode_bytes_mac(std::span<const std::uint8_t> bytes,
                                       const job_context* ctx, unsigned raw_thread_limit) {
  // The full D5 still set now builds on Darwin, so this is just the portable
  // entry (image/pipeline.cpp); the _mac name stays for the callers written
  // when only JPEG/PNG/BMP existed here.
  const unsigned limit =
      raw_thread_limit == 0 ? codec::raw_foreground_threads() : raw_thread_limit;
  return decode_bytes(bytes, ctx, limit);
}

}  // namespace mv::image
