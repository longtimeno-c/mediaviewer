// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/script_thread.h"

#include <windows.h>

#include <process.h>

#include <cstdint>
#include <utility>

namespace mv::addon {

struct script_thread::impl {
  HANDLE thread = nullptr;
  std::function<void()> body;
};

namespace {
unsigned __stdcall trampoline(void* arg) {
  auto* i = static_cast<script_thread::impl*>(arg);
  i->body();
  return 0;
}
}  // namespace

script_thread::~script_thread() { join(); }

bool script_thread::start(std::size_t stack_bytes, std::function<void()> body) {
  if (handle_) return false;
  auto* i = new impl;
  i->body = std::move(body);
  // A reservation, not a commit: the pages are touched only as the stack grows.
  const std::uintptr_t h =
      _beginthreadex(nullptr, static_cast<unsigned>(stack_bytes), trampoline, i,
                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (h == 0) {
    delete i;
    return false;
  }
  i->thread = reinterpret_cast<HANDLE>(h);
  handle_ = i;
  return true;
}

void script_thread::join() noexcept {
  if (!handle_) return;
  WaitForSingleObject(handle_->thread, INFINITE);
  CloseHandle(handle_->thread);
  delete handle_;
  handle_ = nullptr;
}

}  // namespace mv::addon
