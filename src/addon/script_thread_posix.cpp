// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/script_thread.h"

#include <pthread.h>

#include <utility>

namespace mv::addon {

struct script_thread::impl {
  pthread_t thread{};
  std::function<void()> body;
};

namespace {
void* trampoline(void* arg) {
  auto* i = static_cast<script_thread::impl*>(arg);
  i->body();
  return nullptr;
}
}  // namespace

script_thread::~script_thread() { join(); }

bool script_thread::start(std::size_t stack_bytes, std::function<void()> body) {
  if (handle_) return false;
  auto* i = new impl;
  i->body = std::move(body);
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) {
    delete i;
    return false;
  }
  (void)pthread_attr_setstacksize(&attr, stack_bytes);
  const int rc = pthread_create(&i->thread, &attr, trampoline, i);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    delete i;
    return false;
  }
  handle_ = i;
  return true;
}

void script_thread::join() noexcept {
  if (!handle_) return;
  pthread_join(handle_->thread, nullptr);
  delete handle_;
  handle_ = nullptr;
}

}  // namespace mv::addon
