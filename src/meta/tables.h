// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9: the metadata pane's three text tables (plan/06, plan/16 `I`).
//
// A host hands a `metadata` record to its chrome as flat text, one record per
// line and fields separated by tabs (tabs, CR and LF inside a value are
// flattened to spaces, so the separators stay unambiguous). Pure: no I/O, no
// platform header. Both hosts' chrome parse the same three formats:
//
//   summary:    "label\tvalue"                         every row for the kind; value may be empty
//   properties: "space\tgroup\tlabel\tvalue\traw_tag"  space = exif|iptc|xmp|container|computed
//   streams:    "S\tindex\tkind\tcodec" opens a stream, "F\tlabel\tvalue" adds a field to it,
//               "C\tstart_ms\ttitle" is a chapter; empty for a still
#pragma once

#include <string>

#include "meta/meta.h"

namespace mv::meta {

[[nodiscard]] std::string summary_table(const metadata& m);
[[nodiscard]] std::string properties_table(const metadata& m);
[[nodiscard]] std::string streams_table(const metadata& m);

}  // namespace mv::meta
