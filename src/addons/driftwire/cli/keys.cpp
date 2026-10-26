// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/keys.h"

#include <algorithm>

namespace mv::driftwire::cli {

namespace {

constexpr char kEsc = '\x1B';
constexpr std::string_view kPasteEnd = "\x1B[201~";
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD
constexpr std::size_t kMaxSequence = 64;  // longer is noise, not a key

key_event of(key k) {
  key_event e;
  e.k = k;
  return e;
}

key_event text_of(std::string_view s, key k = key::text) {
  key_event e;
  e.k = k;
  e.text.assign(s);
  return e;
}

// The bytes a UTF-8 lead byte announces; 0 for a byte that cannot lead.
std::size_t utf8_length(unsigned char c) {
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) return 3;
  if (c >= 0xF0 && c <= 0xF4) return 4;
  return 0;
}

bool printable(unsigned char c) { return c >= 0x20 && c != 0x7F; }

// One read with text, then a newline, then more text, and no escape
// sequence: pasted (see keys.h). A person typing does not fit that in one.
bool looks_pasted(std::string_view bytes) {
  if (bytes.find(kEsc) != std::string_view::npos) return false;
  std::size_t text_before = 0;
  bool newline = false;
  for (const char ch : bytes) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '\r' || c == '\n') {
      if (text_before > 0) newline = true;
      continue;
    }
    if (!printable(c) && c != '\t') return false;
    if (newline) return true;
    ++text_before;
  }
  return false;
}

// xterm's modifier parameter: 1 + (shift 1 | alt 2 | ctrl 4 | meta 8).
void apply_modifiers(key_event& e, int m) {
  if (m < 2) return;
  const int bits = m - 1;
  e.shift = (bits & 1) != 0;
  e.alt = e.alt || (bits & 2) != 0 || (bits & 8) != 0;
  e.ctrl = (bits & 4) != 0;
}

key letter_key(unsigned char f) {
  switch (f) {
    case 'A': return key::up;
    case 'B': return key::down;
    case 'C': return key::right;
    case 'D': return key::left;
    case 'H': return key::home;
    case 'F': return key::end;
    default: return key::unknown;
  }
}

// Where `bytes` stops being whole UTF-8: before a last character cut by the
// read (its lead byte and fewer continuation bytes than it announces).
std::size_t whole_utf8(std::string_view bytes) {
  for (std::size_t back = 1; back <= 3 && back <= bytes.size(); ++back) {
    const auto c = static_cast<unsigned char>(bytes[bytes.size() - back]);
    if ((c & 0xC0) == 0x80) continue;  // a continuation byte: look further back
    const std::size_t n = utf8_length(c);
    return n > back ? bytes.size() - back : bytes.size();
  }
  return bytes.size();
}

}  // namespace

void key_decoder::feed(std::string_view bytes, std::vector<key_event>& out) {
  if (buf_.empty() && !in_paste_ && looks_pasted(bytes)) {
    // A character the read cut waits for the rest, as typed text does.
    const std::size_t whole = whole_utf8(bytes);
    out.push_back(text_of(bytes.substr(0, whole), key::paste));
    buf_.assign(bytes.substr(whole));
    return;
  }
  buf_.append(bytes);
  std::size_t pos = 0;
  while (pos < buf_.size()) {
    if (in_paste_) {
      const std::size_t end = buf_.find(kPasteEnd, pos);
      if (end == std::string::npos) {
        // Keep back the start of an end marker cut by the read.
        std::size_t keep = 0;
        for (std::size_t k = std::min<std::size_t>(kPasteEnd.size() - 1, buf_.size() - pos); k > 0; --k) {
          if (std::string_view(buf_).substr(buf_.size() - k) == kPasteEnd.substr(0, k)) {
            keep = k;
            break;
          }
        }
        paste_.append(buf_, pos, buf_.size() - pos - keep);
        pos = buf_.size() - keep;
        break;
      }
      paste_.append(buf_, pos, end - pos);
      pos = end + kPasteEnd.size();
      in_paste_ = false;
      out.push_back(text_of(paste_, key::paste));
      paste_.clear();
      continue;
    }
    std::size_t p = pos;
    if (!decode_one(p, out)) break;  // cut off: wait for the rest
    pos = p;
  }
  buf_.erase(0, pos);
}

bool key_decoder::decode_one(std::size_t& pos, std::vector<key_event>& out) {
  const auto c = static_cast<unsigned char>(buf_[pos]);
  if (c == kEsc) return decode_escape(pos, out);
  if (c < 0x20 || c == 0x7F) {
    switch (c) {
      case '\r':
      case '\n': out.push_back(of(key::enter)); break;
      case '\t': out.push_back(of(key::tab)); break;
      case 0x08:
      case 0x7F: out.push_back(of(key::backspace)); break;
      default:
        if (c >= 0x01 && c <= 0x1A) {
          key_event e = of(key::ctrl);
          e.letter = static_cast<char>('a' + c - 1);
          out.push_back(e);
        } else {
          out.push_back(of(key::unknown));  // Ctrl+Space, Ctrl+\ and the like
        }
    }
    ++pos;
    return true;
  }
  const std::size_t n = utf8_length(c);
  if (n == 0) {
    out.push_back(text_of(kReplacement));
    ++pos;
    return true;
  }
  for (std::size_t k = 1; k < n; ++k) {
    if (pos + k >= buf_.size()) return false;  // the rest is in the next read
    if ((static_cast<unsigned char>(buf_[pos + k]) & 0xC0) != 0x80) {
      out.push_back(text_of(kReplacement));
      ++pos;
      return true;
    }
  }
  out.push_back(text_of(std::string_view(buf_).substr(pos, n)));
  pos += n;
  return true;
}

bool key_decoder::decode_escape(std::size_t& pos, std::vector<key_event>& out) {
  if (pos + 1 >= buf_.size()) return false;  // a bare ESC: Esc, or a sequence still coming
  const auto n = static_cast<unsigned char>(buf_[pos + 1]);
  bool alt = false;
  std::size_t at = pos + 1;  // the byte after the ESC that starts the sequence
  if (n == kEsc) {
    // ESC ESC [ ...: Alt with a sequence (some terminals); else Esc, twice.
    if (pos + 2 >= buf_.size()) return false;
    const auto n2 = static_cast<unsigned char>(buf_[pos + 2]);
    if (n2 != '[' && n2 != 'O') {
      out.push_back(of(key::esc));
      ++pos;
      return true;
    }
    alt = true;
    at = pos + 2;
  }
  const auto intro = static_cast<unsigned char>(buf_[at]);
  if (intro == '[') {
    std::size_t p = at + 1;
    while (p < buf_.size() && buf_[p] >= 0x30 && buf_[p] <= 0x3F) ++p;
    const std::size_t params_end = p;
    while (p < buf_.size() && buf_[p] >= 0x20 && buf_[p] <= 0x2F) ++p;
    if (p - pos > kMaxSequence) {
      out.push_back(of(key::unknown));
      pos = p;
      return true;
    }
    if (p >= buf_.size()) return false;
    const auto f = static_cast<unsigned char>(buf_[p]);
    if (f < 0x40 || f > 0x7E) {  // not a sequence after all: drop what was read
      out.push_back(of(key::unknown));
      pos = p;
      return true;
    }
    const std::size_t before = out.size();
    decode_csi(std::string_view(buf_).substr(at + 1, params_end - at - 1), static_cast<char>(f), out, alt);
    // ESC then a report (the cursor's, the mouse's) is the Esc key and then
    // the report: a terminal's answer can arrive inside Esc's wait, and no
    // one holds Alt over a report.
    if (alt && out.size() == before + 1 && (out.back().is(key::cursor) || out.back().is(key::mouse))) {
      const key_event report = out.back();  // a mouse report's own modifier bits stay
      out.back() = of(key::esc);
      out.push_back(report);
    }
    pos = p + 1;
    return true;
  }
  if (intro == 'O' && (alt || at == pos + 1)) {
    if (at + 1 >= buf_.size()) return false;
    const auto f = static_cast<unsigned char>(buf_[at + 1]);
    key_event e = of(f == 'M' ? key::enter : letter_key(f));
    e.alt = alt;
    out.push_back(e);
    pos = at + 2;
    return true;
  }
  // Alt + a key: ESC then the key's own bytes (Option+Left on macOS is ESC b).
  std::size_t p = pos + 1;
  std::vector<key_event> one;
  if (!decode_one(p, one)) return false;
  for (auto& e : one) {
    e.alt = true;
    out.push_back(std::move(e));
  }
  pos = p;
  return true;
}

// Up to three numbers of a CSI's parameters ("12;40" -> 12, 40); how many there were.
static int csi_numbers(std::string_view params, int (&n)[3]) {
  int count = 0;
  int value = 0;
  bool any = false;
  for (const char c : params) {
    if (c >= '0' && c <= '9') {
      value = std::min(value * 10 + (c - '0'), 100000);
      any = true;
    } else if (c == ';') {
      if (count < 3) n[count] = any ? value : 0;
      ++count;
      value = 0;
      any = false;
    } else {
      return -1;
    }
  }
  if (count < 3) n[count] = any ? value : 0;
  return count + 1;
}

void key_decoder::decode_csi(std::string_view params, char final, std::vector<key_event>& out, bool alt) {
  // SGR mouse: CSI < button ; x ; y M (pressed, or moved) or m (released).
  if (!params.empty() && params[0] == '<' && (final == 'M' || final == 'm')) {
    int n[3] = {0, 0, 0};
    if (csi_numbers(params.substr(1), n) == 3 && n[1] > 0 && n[2] > 0) {
      key_event e = of(key::mouse);
      e.motion = (n[0] & 32) != 0;
      e.shift = (n[0] & 4) != 0;
      e.alt = (n[0] & 8) != 0;
      e.ctrl = (n[0] & 16) != 0;
      e.button = n[0] & ~(4 | 8 | 16 | 32);
      e.x = n[1];
      e.y = n[2];
      e.press = final == 'M';
      out.push_back(e);
      return;
    }
  }
  // The cursor position report: CSI row ; col R (CSI ? row ; col R, DECXCPR).
  // F3 with a modifier is CSI 1 ; m R as well; the command line uses no F3.
  if (final == 'R') {
    int n[3] = {0, 0, 0};
    const std::string_view nums = !params.empty() && params[0] == '?' ? params.substr(1) : params;
    if (csi_numbers(nums, n) == 2 && n[0] > 0 && n[1] > 0) {
      key_event e = of(key::cursor);
      e.y = n[0];
      e.x = n[1];
      out.push_back(e);
      return;
    }
  }
  if (!params.empty() && (params[0] == '?' || params[0] == '<' || params[0] == '>' || params[0] == '=')) {
    out.push_back(of(key::unknown));  // another report, not a key
    return;
  }
  int p[2] = {0, 1};
  int count = 0;
  bool any = false;
  int value = 0;
  for (const char c : params) {
    if (c >= '0' && c <= '9') {
      value = std::min(value * 10 + (c - '0'), 100000);
      any = true;
    } else if (c == ';' || c == ':') {
      if (count < 2) p[count] = any ? value : (count == 0 ? 0 : 1);
      ++count;
      value = 0;
      any = false;
    }
  }
  if (count < 2) p[count] = any ? value : (count == 0 ? 0 : 1);

  key_event e;
  if (final == '~') {
    switch (p[0]) {
      case 1:
      case 7: e.k = key::home; break;
      case 4:
      case 8: e.k = key::end; break;
      case 2: e.k = key::insert; break;
      case 3: e.k = key::del; break;
      case 5: e.k = key::page_up; break;
      case 6: e.k = key::page_down; break;
      case 200:
        in_paste_ = true;
        paste_.clear();
        return;
      case 201: return;  // an end with no start
      default: e.k = key::unknown; break;
    }
  } else if (final == 'Z') {
    e.k = key::backtab;
  } else {
    e.k = letter_key(static_cast<unsigned char>(final));
  }
  e.alt = alt;
  if (e.k != key::unknown) apply_modifiers(e, p[1]);
  out.push_back(e);
}

int key_decoder::pending_ms() const noexcept {
  if (in_paste_) return kPasteWaitMs;
  if (buf_.empty()) return -1;
  if (buf_.find_first_not_of(kEsc) == std::string::npos && buf_.size() <= 2) return kEscWaitMs;
  return kCutWaitMs;
}

void key_decoder::flush(std::vector<key_event>& out) {
  if (in_paste_) {
    paste_ += buf_;
    out.push_back(text_of(paste_, key::paste));
    paste_.clear();
    buf_.clear();
    in_paste_ = false;
    return;
  }
  if (buf_.empty()) return;
  if (buf_.find_first_not_of(kEsc) == std::string::npos) {
    for (std::size_t i = 0; i < buf_.size(); ++i) out.push_back(of(key::esc));
  } else if (buf_.size() == 2 && buf_[0] == kEsc && printable(static_cast<unsigned char>(buf_[1]))) {
    // "ESC [" or "ESC O" and nothing after: Alt+[ / Alt+Shift+O after all.
    key_event e = text_of(std::string_view(buf_).substr(1));
    e.alt = true;
    out.push_back(e);
  }
  // Anything else held is half a sequence or a character: dropped.
  buf_.clear();
}

}  // namespace mv::driftwire::cli
