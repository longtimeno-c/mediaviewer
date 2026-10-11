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

struct provenance {
  std::string made_with;  // generator or provenance tool; empty when nothing says
  bool ai_generated = false;
};

// The rule (doc 06): made_with is the first table match in C2PA claim generator,
// C2PA software agents, PNG Software / generator text, XMP CreatorTool, EXIF
// Software; else "Stable Diffusion" for a PNG `parameters` chunk or "ComfyUI"
// for its workflow chunks; else the C2PA claim generator as written (a
// provenance tool, e.g. "Adobe Photoshop 25.0"). A camera's or editor's Software
// that is not in the table is not "made with". ai_generated: an AI source type,
// a table match flagged ai, or the PNG generator chunks.
[[nodiscard]] provenance derive_provenance(const parse::evidence& ev);

}  // namespace mv::meta
