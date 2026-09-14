// SPDX-License-Identifier: GPL-2.0-or-later
// libFuzzer harness: probe + the codec::decode dispatcher (OS codec first,
// then the registry) and the image pipeline on top of it (decode_bytes,
// decode_preview, colour management). This is what a folder open runs.
#include "fuzz_common.h"
#include "image/pipeline.h"

namespace {

void check_display(const mv::result<mv::image::display_image>& r, const char* who) {
  if (!r) return;
  const auto& img = r.value();
  if (img.rgba.size() != static_cast<std::uint64_t>(img.width) * img.height * 4) {
    mv::fuzz::fail(who);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto b = mv::fuzz::bytes(data, size);
  (void)mv::codec::probe(b);
  mv::fuzz::check(mv::codec::decode(b), "codec::decode");
  check_display(mv::image::decode_preview(b), "image::decode_preview");
  check_display(mv::image::decode_bytes(b), "image::decode_bytes");
  return 0;
}
