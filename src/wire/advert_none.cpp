// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/advert.h"

namespace mv::driftwire::advert {

std::unique_ptr<registration> publish(std::uint16_t, const std::vector<std::pair<std::string, std::string>>&) {
  return nullptr;
}

}  // namespace mv::driftwire::advert
