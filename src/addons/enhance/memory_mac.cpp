// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <mach/mach.h>

#include "addons/enhance/memory.h"

namespace mv::enhance {

std::optional<std::uint64_t> available_memory() noexcept {
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_size_t page = 0;
  const mach_port_t host = mach_host_self();
  if (host_page_size(host, &page) != KERN_SUCCESS ||
      host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS) {
    return std::nullopt;
  }
  // Free, plus what the system takes back without writing it out.
  const std::uint64_t pages = std::uint64_t{vm.free_count} + vm.inactive_count + vm.purgeable_count;
  return pages * page;
}

}  // namespace mv::enhance
