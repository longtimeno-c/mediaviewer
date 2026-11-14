// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// No PDF text where the engine never indexes (iOS): the phone searches what the
// publisher already read, and PDFium is a downloaded library it could not load.
#include "addons/ai/pdf_reader.h"

namespace mv::ai {

std::unique_ptr<pdf_reader> make_pdf_reader() { return nullptr; }
std::unique_ptr<pdf_reader> make_pdf_reader_at(const std::string&) { return nullptr; }

}  // namespace mv::ai
