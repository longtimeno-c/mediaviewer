// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The terminal port on Windows: the console in VT mode (Windows 10 1607 and
// later; without it enter_raw() fails and the program draws plainly), UTF-8
// both ways, key input read as records and handed on as the VT bytes the
// console makes of them (ENABLE_VIRTUAL_TERMINAL_INPUT), so keys.cpp decodes
// one stream on every OS.
#include "addons/driftwire/cli/term.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

namespace mv::driftwire::cli {

namespace {

// Mouse reporting off too: the session turns it on while something can be clicked.
constexpr std::string_view kRestore = "\x1B[?2026l\x1B[0m\x1B[?7h\x1B[?25h\x1B[?2004l\x1B[?1000l\x1B[?1006l";

HANDLE g_wake = nullptr;  // auto-reset: wake() and the control handler
HANDLE g_done = nullptr;  // manual-reset: the session is done with the console
std::atomic<bool> g_quit{false};
std::atomic<bool> g_interrupt{false};

struct saved_console {
  HANDLE in = nullptr, out = nullptr;
  DWORD in_mode = 0, out_mode = 0;
  UINT cp = 0, out_cp = 0;
  bool active = false;
  bool sequences = true;
};
saved_console g_saved;

std::wstring to_wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

void write_handle(HANDLE out, bool console, std::string_view bytes) {
  if (bytes.empty()) return;
  if (!console) {
    DWORD done = 0;
    while (!bytes.empty() &&
           ::WriteFile(out, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1 << 20)), &done,
                       nullptr) &&
           done > 0) {
      bytes.remove_prefix(done);
    }
    return;
  }
  const std::wstring w = to_wide(bytes);
  std::size_t at = 0;
  while (at < w.size()) {
    std::size_t n = std::min<std::size_t>(w.size() - at, 8192);
    // Never split a surrogate pair between two writes.
    if (at + n < w.size() && w[at + n - 1] >= 0xD800 && w[at + n - 1] <= 0xDBFF) --n;
    DWORD done = 0;
    if (!::WriteConsoleW(out, w.data() + at, static_cast<DWORD>(n), &done, nullptr) || done == 0) return;
    at += done;
  }
}

void restore(const saved_console& s) {
  DWORD mode = 0;
  if (s.sequences) write_handle(s.out, ::GetConsoleMode(s.out, &mode) != 0, kRestore);
  ::SetConsoleMode(s.in, s.in_mode);
  ::SetConsoleMode(s.out, s.out_mode);
  if (s.cp) ::SetConsoleCP(s.cp);
  if (s.out_cp) ::SetConsoleOutputCP(s.out_cp);
}

void restore_at_exit() {
  if (g_saved.active) {
    restore(g_saved);
    g_saved.active = false;
  }
  if (g_done) ::SetEvent(g_done);
}

// On the console's own thread. Closing the window (and logoff, shutdown)
// ends the process when this returns, so it holds on until the session has
// stopped the engine and restored the console, within the OS's 5 s.
BOOL WINAPI on_control(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:  // only outside raw mode: in it Ctrl+C is a key
      g_interrupt = true;
      if (g_wake) ::SetEvent(g_wake);
      return TRUE;
    case CTRL_BREAK_EVENT:
      g_quit = true;
      if (g_wake) ::SetEvent(g_wake);
      return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_quit = true;
      if (g_wake) ::SetEvent(g_wake);
      if (g_done) ::WaitForSingleObject(g_done, 4500);
      return TRUE;
    default: return FALSE;
  }
}

}  // namespace

struct console::impl {
  HANDLE in = nullptr;
  HANDLE out = nullptr;
  bool out_console = false;
  bool signals = true;
  bool sequences = true;
  bool raw = false;
  saved_console before;
  wchar_t high = 0;  // a high surrogate whose pair is in the next read
};

bool console::is_terminal() noexcept {
  DWORD mode = 0;
  return ::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &mode) != 0 &&
         ::GetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

bool console::utf8() noexcept { return ::GetConsoleOutputCP() == CP_UTF8; }

console::console() : console(options{}) {}

console::console(options o) : p_(std::make_unique<impl>()) {
  p_->in = ::GetStdHandle(STD_INPUT_HANDLE);
  p_->out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  p_->out_console = ::GetConsoleMode(p_->out, &mode) != 0;
  p_->signals = o.signals;
  p_->sequences = o.sequences;
  if (!g_wake) g_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (p_->signals) {
    if (!g_done) g_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::ResetEvent(g_done);
    ::SetConsoleCtrlHandler(on_control, TRUE);
  }
}

console::~console() {
  leave_raw();
  if (p_->signals) {
    if (g_done) ::SetEvent(g_done);  // a close waiting in on_control may go on
    ::SetConsoleCtrlHandler(on_control, FALSE);
  }
}

bool console::enter_raw() {
  if (p_->raw) return true;
  saved_console& b = p_->before;
  b.in = p_->in;
  b.out = p_->out;
  if (!::GetConsoleMode(p_->in, &b.in_mode) || !::GetConsoleMode(p_->out, &b.out_mode)) return false;
  b.sequences = p_->sequences;
  b.cp = ::GetConsoleCP();
  b.out_cp = ::GetConsoleOutputCP();
  const DWORD out_mode =
      b.out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  if (!::SetConsoleMode(p_->out, out_mode)) return false;  // no VT here
  const DWORD in_mode = (b.in_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) |
                        ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
  if (!::SetConsoleMode(p_->in, in_mode)) {
    ::SetConsoleMode(p_->out, b.out_mode);
    return false;
  }
  ::SetConsoleCP(CP_UTF8);
  ::SetConsoleOutputCP(CP_UTF8);
  p_->raw = true;
  static bool hooked = false;
  if (!hooked) {
    std::atexit(restore_at_exit);
    hooked = true;
  }
  g_saved = b;
  g_saved.active = true;
  if (p_->sequences) write("\x1B[?2004h");  // bracketed paste (Windows Terminal; the old console ignores it)
  return true;
}

void console::leave_raw() noexcept {
  if (!p_->raw) return;
  restore(p_->before);
  p_->raw = false;
  g_saved.active = false;
}

bool console::raw() const noexcept { return p_->raw; }

void console::suspend_self() {}

term_size console::size() const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (::GetConsoleScreenBufferInfo(p_->out, &info)) {
    const int cols = info.srWindow.Right - info.srWindow.Left + 1;
    const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (cols > 0 && rows > 0) return {cols, rows};
  }
  return {};
}

void console::wait(term_input& in, int timeout_ms) {
  in = {};
  const ULONGLONG start = ::GetTickCount64();
  while (true) {
    DWORD wait_ms = INFINITE;
    if (timeout_ms >= 0) {
      const ULONGLONG spent = ::GetTickCount64() - start;
      const auto limit = static_cast<ULONGLONG>(timeout_ms);
      wait_ms = spent >= limit ? 0 : static_cast<DWORD>(limit - spent);
    }
    HANDLE handles[2] = {p_->in, g_wake};
    const DWORD rc = ::WaitForMultipleObjects(2, handles, FALSE, wait_ms);
    if (rc == WAIT_TIMEOUT) {
      in.timed_out = true;
      return;
    }
    if (rc == WAIT_OBJECT_0 + 1) {
      in.woken = true;
      in.quit = g_quit.exchange(false);
      in.interrupt = g_interrupt.exchange(false);
      return;
    }
    if (rc != WAIT_OBJECT_0) {
      in.eof = true;
      return;
    }
    INPUT_RECORD records[128];
    DWORD n = 0;
    if (!::ReadConsoleInputW(p_->in, records, 128, &n)) {
      in.eof = true;
      return;
    }
    std::wstring units;
    if (p_->high) {
      units += p_->high;
      p_->high = 0;
    }
    for (DWORD i = 0; i < n; ++i) {
      const INPUT_RECORD& r = records[i];
      if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        in.resized = true;
      } else if (r.EventType == KEY_EVENT) {
        const KEY_EVENT_RECORD& k = r.Event.KeyEvent;
        const wchar_t ch = k.uChar.UnicodeChar;
        // Key-downs carry the characters (VT sequences for the special
        // keys); Alt+numpad's character comes with Alt's release.
        if (ch == 0 || (!k.bKeyDown && k.wVirtualKeyCode != VK_MENU)) continue;
        for (WORD rep = 0; rep < std::max<WORD>(1, k.wRepeatCount); ++rep) units += ch;
      }
    }
    if (!units.empty() && units.back() >= 0xD800 && units.back() <= 0xDBFF) {
      p_->high = units.back();
      units.pop_back();
    }
    in.bytes += to_utf8(units);
    if (!in.bytes.empty() || in.resized) return;
    // Only records we do not use (focus, menus, key-ups): wait on.
  }
}

void console::wake() noexcept {
  if (g_wake) ::SetEvent(g_wake);
}

void console::write(std::string_view bytes) { write_handle(p_->out, p_->out_console, bytes); }

}  // namespace mv::driftwire::cli
