// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A core call that does something (an upload, a move, a new folder on a server) and answers
// with text (the new key, a local path) is called once, never again to size a buffer: a second
// call would do the change twice (an upload going up again as "name (2)"). The buffer is large
// enough for any key; a longer answer comes back cut, flagged, and the change still stands
// (mediaviewer.h 0.45: those calls return MV_OK on a short buffer). Pure, no ABI header.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mv::shell {

inline constexpr std::uint32_t kAbiTextBytes = 16 * 1024;

struct abi_text_result {
  std::int32_t status = 0;  // the call's mv_status
  std::string text;         // what it answered ("" on a failure)
  bool cut = false;         // longer than the buffer: `text` is not the whole answer
};

// `call(buf, cap, &needed)` -> mv_status. Called exactly once.
[[nodiscard]] inline abi_text_result abi_text_once(
    const std::function<std::int32_t(char*, std::uint32_t, std::uint32_t*)>& call) {
  abi_text_result r;
  std::string buf(kAbiTextBytes, '\0');
  std::uint32_t need = 0;
  r.status = call(buf.data(), kAbiTextBytes, &need);
  if (r.status != 0) return r;
  r.cut = need > kAbiTextBytes;
  const std::size_t len = r.cut ? kAbiTextBytes - 1 : (need > 0 ? need - 1 : 0);
  buf.resize(len);
  if (!r.cut) r.text = std::move(buf);
  return r;
}

}  // namespace mv::shell
