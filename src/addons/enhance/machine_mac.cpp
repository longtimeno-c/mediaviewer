// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The local check on macOS: memory (hw.memsize; free is the free and
// inactive pages, which the system hands out without paging) and, on Apple
// silicon, the chip, whose GPU shares that memory (vram 0). Enhance builds
// arm64 only on the Mac (cmake/enhance.cmake), as the AI pack.
#include <mach/mach.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "addons/enhance/machine.h"

namespace mv::enhance {
namespace {

#if defined(__aarch64__)
std::string sysctl_string(const char* name) {
  std::size_t n = 0;
  if (sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) return {};
  std::string s(n, '\0');
  if (sysctlbyname(name, s.data(), &n, nullptr, 0) != 0) return {};
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}
#endif

}  // namespace

platform_facts platform_probe() {
  platform_facts f;
  std::uint64_t mem = 0;
  std::size_t n = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &n, nullptr, 0) == 0) f.ram_total = mem;
  const mach_port_t host = mach_host_self();
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
    vm_size_t page = 0;
    if (host_page_size(host, &page) != KERN_SUCCESS || page == 0) page = static_cast<vm_size_t>(sysconf(_SC_PAGESIZE));
    f.ram_free = (static_cast<std::uint64_t>(vm.free_count) + vm.inactive_count) * page;
    if (f.ram_total && f.ram_free > f.ram_total) f.ram_free = f.ram_total;
  }
  (void)mach_port_deallocate(mach_task_self(), host);
#if defined(__aarch64__)
  f.accelerator = "apple";
  f.gpu = sysctl_string("machdep.cpu.brand_string");  // "Apple M2 Pro"
#endif
  return f;
}

void enter_background() noexcept {
  // QOS_CLASS_BACKGROUND throttles CPU, I/O and timers, and keeps the work
  // off the performance cores (src/addons/ai/platform_posix.cpp).
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
}

}  // namespace mv::enhance
