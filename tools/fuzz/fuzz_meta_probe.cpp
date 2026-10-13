// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#345): meta::probe_evidence over memory -- the AI badge's
// provenance probe, every container it walks by seeking (JPEG segments, PNG
// chunks, WebP chunks, TIFF / ORF / RW2 IFD0, RAF's embedded JPEG, ISO-BMFF top
// level, a HEIF meta box's iinf / iloc / idat, a clip's moov / udta / meta / keys
// / ilst, CR3's Canon box). Invariants: it returns, its strings are valid UTF-8
// once cleaned, and the provenance rule accepts whatever it collected.
#include "fuzz_meta.h"
#include "meta/probe.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mv::meta::parse::memory_source src({data, size});
  mv::meta::parse::evidence ev;
  mv::meta::probe_evidence(src, ev);
  const mv::meta::provenance p = mv::meta::derive_provenance(ev);
  if (!mv::fuzz::utf8_ok(p.evidence)) mv::fuzz::meta_fail("probe evidence");
  if (!p.ai_generated && !p.evidence.empty()) mv::fuzz::meta_fail("probe evidence without ai");
  return 0;
}
