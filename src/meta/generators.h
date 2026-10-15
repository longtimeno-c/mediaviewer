// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: the one table of known image generators, and the rule that turns what a
// file says (parse::evidence) into summary::made_with and summary::ai_generated
// (docs/design/06 "Made with and AI provenance").
//
// Positive evidence only. A file is called AI-made because a field it carries
// says so: a C2PA digitalSourceType of trainedAlgorithmicMedia or
// compositeWithTrainedAlgorithmicMedia (or the same IPTC term in XMP), or a
// C2PA claim generator / software agent, PNG text, EXIF Software or XMP
// CreatorTool that names a generator in this table. Never from the file name,
// never from pixels, and a file with no provenance is not "not AI": missing
// metadata proves nothing.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "meta/parse.h"

namespace mv::meta {

struct generator {
  std::string_view name;  // what the pane shows: "ChatGPT", "Adobe Firefly"
  bool ai = true;         // a generative-AI tool (every entry today)
};

// The table entry a text names ("Adobe Firefly 2.1", "ChatGPT", "ComfyUI"), by
// whole-word, case-insensitive match; nullopt when it names none.
[[nodiscard]] std::optional<generator> match_generator(std::string_view text) noexcept;

// Every entry, in match order (for the tests and doc 06).
[[nodiscard]] std::span<const std::pair<std::string_view, generator>> generator_table() noexcept;

// The IPTC Digital Source Type term at the end of a URI or bare
// ("http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia" →
// "trainedAlgorithmicMedia").
[[nodiscard]] std::string_view source_type_term(std::string_view uri) noexcept;
// True for trainedAlgorithmicMedia and compositeWithTrainedAlgorithmicMedia.
[[nodiscard]] bool source_type_is_ai(std::string_view uri) noexcept;
// A person's reading of the term: "AI-generated (trainedAlgorithmicMedia)".
[[nodiscard]] std::string describe_source_type(std::string_view uri);
// #402: the All tags value of XMP Iptc4xmpExt:DigitalSourceType: the reading,
// then the value exactly as written ("AI-generated
// (http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia)").
// Exiv2's own print knows five terms and shows any other cut to "(...)".
[[nodiscard]] std::string describe_source_type_value(std::string_view value);

struct provenance {
  std::string made_with;  // generator or provenance tool; empty when nothing says
  bool ai_generated = false;
  // #345: why ai_generated is true, for a person (the canvas badge's tooltip):
  // the field that said so and what it said, e.g. "Content Credentials (C2PA):
  // AI-generated (trainedAlgorithmicMedia)" or "XMP CreatorTool: Midjourney".
  // Empty when ai_generated is false: there is no evidence of "not AI".
  std::string evidence;

  friend bool operator==(const provenance&, const provenance&) = default;
};

// The rule (doc 06): made_with is the first table match in C2PA claim generator,
// C2PA software agents, PNG Software / generator text, XMP CreatorTool, EXIF
// Software; else "Stable Diffusion" for a PNG `parameters` chunk or "ComfyUI"
// for its workflow chunks; else the C2PA claim generator as written (a
// provenance tool, e.g. "Adobe Photoshop 25.0"). A camera's or editor's Software
// that is not in the table is not "made with". ai_generated: an AI source type,
// a table match flagged ai, or the PNG generator chunks. evidence names the
// deciding fact, most authoritative first: an AI source type (C2PA before XMP),
// then the field whose generator matched, then the PNG generator chunks.
[[nodiscard]] provenance derive_provenance(const parse::evidence& ev);

// #345: one evidence line, "<field>: <value>", the value cleaned and capped at
// 80 bytes on a character boundary (a PNG `parameters` tail can be a prompt).
[[nodiscard]] std::string evidence_line(std::string_view field, std::string_view value);

}  // namespace mv::meta
