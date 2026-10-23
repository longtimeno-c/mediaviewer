// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "addons/driftwire/cli/text.h"
#include "addons/driftwire/eta.h"
#include "addons/driftwire/wire.h"

namespace mv::driftwire::cli {

namespace {

using ftxui::Element;
using ftxui::Elements;
using ftxui::hbox;
using ftxui::text;
using ftxui::vbox;

std::string spaces(int n) { return n > 0 ? std::string(static_cast<std::size_t>(n), ' ') : std::string(); }

std::string sep(const theme& th) { return " " + std::string(th.g->sep) + " "; }

Element dimmed(const theme& th, std::string s) { return paint(th, text(std::move(s)), tone::dim); }

Element blank() { return text(""); }

// A colour `t` of the way from `a` to `b`, the ends exactly `a` and `b`
// (FTXUI's gamma-correct blend rounds 64 to 63 on the way back).
ftxui::Color mix(float t, const ftxui::Color& a, const ftxui::Color& b) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  return ftxui::Color::Interpolate(t, a, b);
}

// Each glyph of `s` coloured along a gradient from `a` to `b`.
Element gradient(const theme& th, std::string_view s, ftxui::Color a, ftxui::Color b) {
  if (!th.colour()) return text(std::string(s));
  const auto spans = glyph_spans(s);
  Elements parts;
  const auto n = static_cast<float>(spans.size() > 1 ? spans.size() - 1 : 1);
  for (std::size_t i = 0; i < spans.size(); ++i) {
    parts.push_back(text(std::string(s.substr(spans[i].at, spans[i].len))) |
                    ftxui::color(mix(static_cast<float>(i) / n, a, b)));
  }
  return hbox(std::move(parts));
}

// Rows of `parts` joined by `glue`, as many per row as fit in `width`; a part
// wider than a row is cut.
struct part {
  std::string s;
  tone t = tone::normal;
  bool bold = false;
};

Element flow(const theme& th, const std::vector<part>& parts, std::string_view glue, int width, int indent) {
  Elements rows;
  Elements row;
  int used = 0;
  const int gw = display_width(glue);
  const int room = std::max(1, width - indent);
  for (const auto& p : parts) {
    std::string s = fit(p.s, room, th);
    const int w = display_width(s);
    if (used > 0 && used + gw + w > room) {
      rows.push_back(hbox(std::move(row)));
      row.clear();
      used = 0;
    }
    if (used == 0 && indent > 0) row.push_back(text(spaces(indent)));
    if (used > 0) {
      row.push_back(dimmed(th, std::string(glue)));
      used += gw;
    }
    Element e = paint(th, text(std::move(s)), p.t);
    if (p.bold) e = ftxui::bold(std::move(e));
    row.push_back(std::move(e));
    used += w;
  }
  if (!row.empty()) rows.push_back(hbox(std::move(row)));
  return vbox(std::move(rows));
}

// "<text> <accented digits>" on one row when it fits, else two.
Element check_line(const theme& th, const std::string& before, const std::string& digits, int width) {
  return flow(th, {{before, tone::normal, false}, {digits, tone::accent, true}}, " ", width, 2);
}

// clang-format off
// The code in three rows of rounded box drawing, four columns a digit. The
// table is a picture, so it is UTF-8 here rather than escapes (the builds
// compile UTF-8 sources: MSVC with /utf-8).
constexpr std::string_view kDigits[10][3] = {
    {"╭──╮", "│  │", "╰──╯"},  // 0
    {" ╶╮ ", "  │ ", " ╶┴╴"},  // 1
    {"╭──╮", "╭──╯", "╰──╴"},  // 2
    {"╶──╮", " ──┤", "╶──╯"},  // 3
    {"╷  ╷", "╰──┤", "   ╵"},  // 4
    {"╭──╴", "╰──╮", "╶──╯"},  // 5
    {"╭──╴", "├──╮", "╰──╯"},  // 6
    {"╶──╮", "   │", "   ╵"},  // 7
    {"╭──╮", "├──┤", "╰──╯"},  // 8
    {"╭──╮", "╰──┤", "╶──╯"},  // 9
};
// clang-format on

std::string digits_of(std::string_view s) {
  std::string d;
  for (const char c : s) {
    if (c >= '0' && c <= '9') d += c;
  }
  return d;
}

// "24 files", "1 file".
std::string files_text(std::size_t n) { return std::to_string(n) + (n == 1 ? " file" : " files"); }

std::string bearer_label(bearer b) {
  switch (b) {
    case bearer::bluetooth: return "Bluetooth";
    case bearer::internet: return "internet";
    case bearer::dht: return "DHT";
    case bearer::relay: return "relay";
    case bearer::wifi: break;
  }
  return "Wi-Fi";
}

// "9 min left", "under a minute left".
std::string life_text(double s) {
  if (s < 0) return "";
  if (s < 60) return "under a minute left";
  return std::to_string(std::max(1L, std::lround(s / 60))) + " min left";
}

// Six boxes of five columns, a column between them and three between the groups.
constexpr int kBoxesWidth = 6 * 5 + 4 + 3;
constexpr int kDigitsWidth = 6 * 5 - 1 + 3;  // big_digits' "123 456"

// The code under a card's title, indented as the mock-ups show it: big
// digits from 40 columns (the card's `cw` is the terminal's less four),
// plain bold digits below.
Element code_row(const theme& th, std::string_view code, int cw) {
  const int indent = std::min(5, std::max(2, cw - kDigitsWidth));
  return hbox({text(spaces(indent)), big_digits(th, code, cw + 4 >= 40 ? cw - indent : 0)});
}

// The six boxes the same way; one row of brackets where they do not fit.
Element boxes_row(const theme& th, std::string_view typed, int cw) {
  const int indent = std::min(5, std::max(2, cw - kBoxesWidth));
  return hbox({text(spaces(indent)), code_boxes(th, typed, cw - indent)});
}

// `parts` joined by " · " in at most `room` columns: whole parts drop, from
// the second-last backwards (the first says how to begin, the last how to
// finish), then the last. "" when not even the first fits.
std::string fit_parts(const theme& th, const std::vector<std::string>& parts, int room) {
  const std::string glue = sep(th);
  std::vector<std::size_t> keep;
  for (std::size_t i = 0; i < parts.size(); ++i) keep.push_back(i);
  while (!keep.empty()) {
    std::string s;
    for (const std::size_t i : keep) s += (s.empty() ? "" : glue) + parts[i];
    if (display_width(s) <= room) return s;
    keep.erase(keep.size() > 2 ? keep.end() - 2 : keep.end() - 1);
  }
  return {};
}

// A card's last row: what to do on the left, the keys flush right; when the
// two do not share a row, the words wrap and the keys go under them (as
// many whole keys as fit).
Element say_and_keys(const theme& th, std::string_view say, tone say_tone, const std::vector<std::string>& keys,
                     int cw) {
  const int room = cw - 2;
  const std::string every = fit_parts(th, keys, 1 << 20);
  const auto keys_row = [&](const std::string& k) {
    return hbox({text(spaces(2 + std::max(0, room - display_width(k)))), dimmed(th, k)});
  };
  if (say.empty()) return keys_row(fit_parts(th, keys, room));
  if (every.empty()) return lines(th, say, cw, say_tone, 2);
  if (display_width(say) + 2 + display_width(every) <= room) {
    return hbox({text("  "), spread(th, say, say_tone, every, tone::dim, room)});
  }
  return vbox({lines(th, say, cw, say_tone, 2), keys_row(fit_parts(th, keys, room))});
}

}  // namespace

// ---- words --------------------------------------------------------------------------

std::string fit(std::string_view utf8, int width, const theme& th) { return fit(utf8, width, th.g->ellipsis); }

std::string elapsed_text(double seconds) {
  if (seconds < 0.5) return "under a second";
  const long s = std::lround(seconds);
  if (s < 60) return std::to_string(s) + " s";
  if (s < 3600) {
    const long m = s / 60, r = s % 60;
    return std::to_string(m) + " min" + (r ? " " + std::to_string(r) + " s" : std::string());
  }
  const long h = s / 3600, m = (s % 3600) / 60;
  return std::to_string(h) + " h" + (m ? " " + std::to_string(m) + " min" : std::string());
}

std::string wait_text(double seconds) {
  const long s = std::max(0L, static_cast<long>(seconds));
  char buf[32];
  if (s < 60) {
    std::snprintf(buf, sizeof buf, "%lds", s);
  } else if (s < 3600) {
    std::snprintf(buf, sizeof buf, "%ldm %02lds", s / 60, s % 60);
  } else {
    std::snprintf(buf, sizeof buf, "%ldh %02ldm", s / 3600, (s % 3600) / 60);
  }
  return buf;
}

std::string home_short(std::string_view path, std::string_view home) {
  while (home.size() > 1 && (home.back() == '/' || home.back() == '\\')) home.remove_suffix(1);
  if (home.empty() || path.size() < home.size() || path.substr(0, home.size()) != home) return std::string(path);
  if (path.size() == home.size()) return "~";
  if (path[home.size()] != '/') return std::string(path);
  return "~" + std::string(path.substr(home.size()));
}

std::string names_text(const std::vector<std::string>& names, std::size_t count, int width, const theme& th) {
  if (names.empty()) return files_text(count);
  const std::size_t most = std::min<std::size_t>(names.size(), 4);
  const auto more = [&](std::size_t k) {
    return count > k ? " and " + std::to_string(count - k) + " more" : std::string();
  };
  for (std::size_t k = most; k >= 1; --k) {
    std::string s;
    for (std::size_t i = 0; i < k; ++i) {
      if (i) s += ", ";
      s += names[i];
    }
    s += more(k);
    if (display_width(s) <= width) return s;
  }
  const std::string tail = more(1);
  return fit(names[0], width - display_width(tail), th) + tail;
}

// ---- primitives ----------------------------------------------------------------------

Element paint(const theme& th, Element e, tone t) {
  if (!th.colour()) {
    // NO_COLOR: weight only. Dim stays dim; an error stays noticeable.
    if (t == tone::dim) return ftxui::dim(std::move(e));
    if (t == tone::error) return ftxui::bold(std::move(e));
    return e;
  }
  switch (t) {
    case tone::normal: return e;
    case tone::accent:
    case tone::info: return e | ftxui::color(th.accent);
    case tone::dim: return e | ftxui::color(th.grey);
    case tone::success: return e | ftxui::color(th.success);
    case tone::error: return e | ftxui::color(th.error);
    case tone::warning: return e | ftxui::color(th.warning);
  }
  return e;
}

Element card(const theme& th, int width, std::string_view title, Elements body, tone border) {
  const glyph_set& g = *th.g;
  if (width < 8) return vbox(std::move(body));
  if (!title.empty() && display_width(title) > width - 6) {
    // Too long for the border (a long name, a narrow terminal): the title
    // goes inside, whole and wrapped, rather than lose its end to a cut.
    Elements head;
    for (auto& l : wrap(title, width - 4)) head.push_back(text(std::move(l)) | ftxui::bold);
    body.insert(body.begin(), vbox(std::move(head)));
    title = {};
  }
  Element top;
  if (title.empty()) {
    top = paint(th, text(std::string(g.tl) + repeat(g.h, width - 2) + std::string(g.tr)), border);
  } else {
    // "╭─ Title ───╮": 1 + 1 + 1 + title + 1 + rest + 1 columns.
    std::string t(title);
    const int rest = width - 5 - display_width(t);
    top = hbox({paint(th, text(std::string(g.tl) + std::string(g.h) + " "), border), text(std::move(t)) | ftxui::bold,
                paint(th, text(" " + repeat(g.h, rest) + std::string(g.tr)), border)});
  }
  Elements rows{std::move(top)};
  if (!body.empty()) {
    rows.push_back(hbox({paint(th, ftxui::separatorCharacter(std::string(g.v)), border), text(" "),
                         vbox(std::move(body)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width - 4), text(" "),
                         paint(th, ftxui::separatorCharacter(std::string(g.v)), border)}));
  }
  rows.push_back(paint(th, text(std::string(g.bl) + repeat(g.h, width - 2) + std::string(g.br)), border));
  return vbox(std::move(rows));
}

Element big_digits(const theme& th, std::string_view digits, int width) {
  const std::string d = digits_of(digits);
  const bool grouped = d.size() == 6;
  const int need = static_cast<int>(d.size()) * 5 - 1 + (grouped ? 3 : 0);
  if (d.empty() || !th.g->big_digits || need > width) {
    std::string plain = grouped ? d.substr(0, 3) + " " + d.substr(3) : d;
    return paint(th, text(std::move(plain)) | ftxui::bold, tone::accent);
  }
  Elements rows;
  for (int r = 0; r < 3; ++r) {
    std::string row;
    for (std::size_t i = 0; i < d.size(); ++i) {
      if (i) row += grouped && i == 3 ? "    " : " ";
      row += kDigits[d[i] - '0'][r];
    }
    rows.push_back(text(std::move(row)));
  }
  return paint(th, vbox(std::move(rows)) | ftxui::bold, tone::accent);
}

Element code_boxes(const theme& th, std::string_view typed, int width) {
  const std::string d = digits_of(typed).substr(0, 6);
  const glyph_set& g = *th.g;
  const auto tone_of = [&](std::size_t i) {
    return i < d.size() ? tone::normal : i == d.size() ? tone::accent : tone::dim;
  };
  const auto inside = [&](std::size_t i) -> Element {
    if (i < d.size()) return text(std::string(1, d[i])) | ftxui::bold;
    if (i == d.size()) return paint(th, text(std::string(g.caret)), tone::accent);
    return text(" ");
  };
  Elements boxes;
  if (!g.big_digits || width < kBoxesWidth) {
    // [4][8][2] [9][_][ ]
    for (std::size_t i = 0; i < 6; ++i) {
      if (i == 3) boxes.push_back(text(" "));
      boxes.push_back(paint(th, text("["), tone_of(i)));
      boxes.push_back(inside(i));
      boxes.push_back(paint(th, text("]"), tone_of(i)));
    }
    return hbox(std::move(boxes));
  }
  const std::string h3 = repeat(g.h, 3);
  for (std::size_t i = 0; i < 6; ++i) {
    if (i) boxes.push_back(text(i == 3 ? "   " : " "));
    const tone t = tone_of(i);
    boxes.push_back(vbox({
        paint(th, text(std::string(g.btl) + h3 + std::string(g.btr)), t),
        hbox({paint(th, text(std::string(g.v) + " "), t), inside(i), paint(th, text(" " + std::string(g.v)), t)}),
        paint(th, text(std::string(g.bbl) + h3 + std::string(g.bbr)), t),
    }));
  }
  return hbox(std::move(boxes));
}

Element choices(const theme& th, int width, const std::vector<std::string>& items, int selected) {
  Elements rows;
  const std::string gap = spaces(display_width(th.g->pointer));
  for (std::size_t i = 0; i < items.size(); ++i) {
    const bool on = static_cast<int>(i) == selected;
    std::string label = fit(std::to_string(i + 1) + ". " + items[i], width - 1 - static_cast<int>(gap.size()), th);
    Element lab = text(std::move(label));
    rows.push_back(hbox({on ? paint(th, text(std::string(th.g->pointer)), tone::accent) : text(gap), text(" "),
                         on ? paint(th, std::move(lab), tone::accent) : std::move(lab)}));
  }
  return vbox(std::move(rows));
}

Element lines(const theme& th, std::string_view s, int width, tone t, int indent) {
  Elements rows;
  for (auto& l : wrap(s, std::max(1, width - indent))) {
    rows.push_back(hbox({text(spaces(indent)), paint(th, text(std::move(l)), t)}));
  }
  return vbox(std::move(rows));
}

Element spread(const theme& th, std::string_view left, tone left_tone, std::string_view right, tone right_tone,
               int width) {
  const int rw = display_width(right);
  const bool both = rw > 0 && width - rw - 2 >= std::min(12, display_width(left));
  std::string l = fit(left, both ? width - rw - 2 : width, th);
  const int lw = display_width(l);
  Elements row{paint(th, text(std::move(l)), left_tone)};
  if (both) {
    row.push_back(text(spaces(width - lw - rw)));
    row.push_back(paint(th, text(std::string(right)), right_tone));
  }
  return hbox(std::move(row));
}

// ---- the start banner ----------------------------------------------------------------

Element banner(const theme& th, const banner_model& m, int width) {
  const int cw = width - 4;
  const std::string mark = std::string(th.g->wordmark) + " Driftwire";
  Element title = gradient(th, mark, th.accent, th.gradient_end) | ftxui::bold;
  Elements body;
  body.push_back(hbox({std::move(title), text("  "), dimmed(th, m.version)}));
  body.push_back(blank());
  std::vector<part> who{{m.name, tone::normal, true}};
  if (!m.presence.empty()) who.push_back({m.presence});
  if (!m.internet.empty()) who.push_back({m.internet});
  const std::string glue = " " + sep(th) + " ";
  body.push_back(flow(th, who, glue, cw, 2));
  std::vector<part> where;
  if (!m.saves_to.empty()) where.push_back({"Saves to " + m.saves_to});
  if (!m.help.empty()) where.push_back({m.help, tone::dim});
  body.push_back(flow(th, where, glue, cw, 2));
  return card(th, width, "", std::move(body));
}

// ---- Nearby ----------------------------------------------------------------------------

Element nearby(const theme& th, const nearby_model& m, int width) {
  const glyph_set& g = *th.g;
  Elements rows;
  rows.push_back(hbox({text("  "), text("Nearby") | ftxui::bold}));
  const int total = static_cast<int>(m.peers.size() + m.known.size());
  // The rows shown: all, or a window around the selection under the height cap.
  int first = 0, last = total;
  if (m.max_rows > 0 && total > m.max_rows) {
    const int sel = std::clamp(m.selected, 0, total - 1);
    first = std::clamp(sel - m.max_rows / 2, 0, total - m.max_rows);
    last = first + m.max_rows;
  }
  const auto row_at = [&](int i) -> const nearby_row& {
    return i < static_cast<int>(m.peers.size()) ? m.peers[static_cast<std::size_t>(i)]
                                                : m.known[static_cast<std::size_t>(i) - m.peers.size()];
  };
  // Columns: "  ❯ ◉ " then the name, the detail in a column of its own (about
  // two thirds across) or none when the terminal is too narrow for both.
  int name_w = 0, detail_w = 0;
  for (int i = first; i < last; ++i) {
    name_w = std::max(name_w, display_width(row_at(i).name));
    detail_w = std::max(detail_w, display_width(row_at(i).detail));
  }
  const int lead = 6;
  int detail_x = std::max(lead + name_w + 3, width * 2 / 3);
  detail_x = std::min(detail_x, width - detail_w - 1);
  const bool details = detail_w > 0 && detail_x - lead - 2 >= 8;
  const int name_room = details ? detail_x - lead - 2 : width - lead - 1;

  const auto peer_row = [&](int i) {
    const nearby_row& r = row_at(i);
    const bool on = m.show_pointer && i == m.selected;
    std::string name = fit(r.name, name_room, th);
    const int nw = display_width(name);
    Elements cells{text("  "), on ? paint(th, text(std::string(g.pointer)), tone::accent) : text(" "), text(" "),
                   th.colour() ? text(std::string(g.peer)) | ftxui::color(th.of(r.dot)) : text(std::string(g.peer)),
                   text(" "), on ? text(std::move(name)) | ftxui::bold : text(std::move(name))};
    if (details && !r.detail.empty()) {
      cells.push_back(text(spaces(detail_x - lead - nw)));
      cells.push_back(paint(th, text(fit(r.detail, width - detail_x, th)), r.detail_tone));
    }
    return hbox(std::move(cells));
  };

  if (m.peers.empty()) {
    rows.push_back(hbox({text("  "), dimmed(th, std::string(g.nobody)), text(" "), text(m.empty_title)}));
    rows.push_back(lines(th, m.empty_how, width, tone::dim, 4));
  }
  for (int i = first; i < last; ++i) {
    if (i == static_cast<int>(m.peers.size())) {
      rows.push_back(hbox({text("  "), text("Your devices") | ftxui::bold,
                           dimmed(th, sep(th) + "over the internet")}));
    }
    rows.push_back(peer_row(i));
  }
  if (last - first < total) {
    const int hidden = total - (last - first);
    rows.push_back(dimmed(th, "    " + std::string(g.ellipsis) + " and " + std::to_string(hidden) + " more (" +
                                  std::string(g.arrows) + ")"));
  }
  if (!m.note.empty()) rows.push_back(lines(th, m.note, width, tone::dim, 2));
  return vbox(std::move(rows));
}

// ---- the prompt, its hints, the palette ----------------------------------------------------

int prompt_columns(int width) { return std::max(1, width - 6); }

Element prompt(const theme& th, const prompt_model& m, int width) {
  const int cols = prompt_columns(width);
  Elements line{text(">"), m.cut_left ? dimmed(th, std::string(th.g->ellipsis)) : text(" ")};
  const auto focus = [&](Element e) { return m.focused ? ftxui::focusCursorBar(std::move(e)) : e; };
  if (m.text.empty()) {
    std::string ph = fit(m.placeholder, cols, th);
    const auto spans = glyph_spans(ph);
    if (spans.empty()) {
      line.push_back(focus(text(" ")));
    } else {
      line.push_back(focus(dimmed(th, ph.substr(0, spans[0].len))));
      line.push_back(dimmed(th, ph.substr(spans[0].len)));
    }
  } else {
    // Before the cursor, the glyph under it (a space at the end), after it.
    std::size_t cut = m.text.size(), next = m.text.size();
    int col = 0;
    for (const auto& gs : glyph_spans(m.text)) {
      if (col >= m.cursor_col) {
        cut = gs.at;
        next = gs.at + gs.len;
        break;
      }
      col += gs.width;
    }
    line.push_back(text(m.text.substr(0, cut)));
    line.push_back(focus(text(cut < m.text.size() ? m.text.substr(cut, next - cut) : std::string(" "))));
    if (next < m.text.size()) line.push_back(text(m.text.substr(next)));
  }
  // line_editor::view leaves the last column free when the text goes on
  // past it: the ellipsis says so there, as the one after ">" does on the left.
  if (m.cut_right) line.push_back(dimmed(th, std::string(th.g->ellipsis)));
  Element row = hbox(std::move(line));
  return card(th, width, m.title, {std::move(row)}, m.border);
}

Element hints(const theme& th, const hint_model& m, int width) {
  // `right` stays while any part fits beside it (fit_parts says which).
  const int room = std::max(1, width - 4);
  if (!m.right.empty()) {
    if (const std::string left = fit_parts(th, m.parts, room - display_width(m.right) - 2);
        !left.empty() || m.parts.empty()) {
      return hbox({text("  "), spread(th, left, m.left_tone, m.right, tone::dim, room)});
    }
  }
  if (const std::string left = fit_parts(th, m.parts, room); !left.empty() || m.parts.empty()) {
    return hbox({text("  "), paint(th, text(left), m.left_tone)});
  }
  // Not even one part fits: the first, cut.
  return hbox({text("  "), paint(th, text(fit(m.parts[0], room, th)), m.left_tone)});
}

Element command_palette(const theme& th, const palette_model& m, int width) {
  const glyph_set& g = *th.g;
  Elements rows;
  if (m.rows.empty()) {
    rows.push_back(hbox({text("    "), dimmed(th, m.empty)}));
    return vbox(std::move(rows));
  }
  const int n = static_cast<int>(m.rows.size());
  const int shown = m.max_rows > 0 ? std::min(n, m.max_rows) : n;
  const int sel = std::clamp(m.selected, 0, n - 1);
  const int first = std::clamp(sel - shown + 1, 0, n - shown);
  int name_w = 13;
  for (const auto& r : m.rows) {
    name_w = std::max(name_w, display_width(r.name) + (r.args.empty() ? 0 : 1 + display_width(r.args)));
  }
  name_w = std::min(name_w + 2, std::max(8, width / 2));
  for (int i = first; i < first + shown; ++i) {
    const palette_row& r = m.rows[static_cast<std::size_t>(i)];
    const bool on = i == sel;
    std::string name = fit(r.name, name_w - 1, th);
    std::string args =
        r.args.empty() ? std::string() : fit(r.args, std::max(0, name_w - 2 - display_width(name)), th);
    const int used = display_width(name) + (args.empty() ? 0 : 1 + display_width(args));
    Elements cells{text("  "), on ? paint(th, text(std::string(g.pointer)), tone::accent) : text(" "), text(" "),
                   on ? paint(th, text(std::move(name)), tone::accent) : text(std::move(name))};
    if (!args.empty()) cells.push_back(dimmed(th, " " + args));
    cells.push_back(text(spaces(name_w - used)));
    const int room = width - 4 - name_w;
    if (room >= 8) {
      std::string d = fit(r.description, room, th);
      cells.push_back(on ? text(std::move(d)) : dimmed(th, std::move(d)));
    }
    rows.push_back(hbox(std::move(cells)));
  }
  return vbox(std::move(rows));
}

// ---- waiting and moving -------------------------------------------------------------------

Element waiting(const theme& th, const wait_model& m, int width, std::int64_t ms) {
  const glyph_set& g = *th.g;
  // · ✢ ✳ ✶ ✻ ✽ and back (ping-pong), 120 ms a step; ASCII spins - \ | /.
  const int frames = g.spinner_frames;
  const bool pingpong = frames == 6;
  const int period = pingpong ? 2 * frames - 2 : frames;
  int step = static_cast<int>((ms < 0 ? 0 : ms) / 120 % period);
  if (pingpong && step >= frames) step = period - step;
  std::string suffix;
  if (m.elapsed_s >= 0 && !m.hint.empty()) {
    suffix = "  (" + wait_text(m.elapsed_s) + sep(th) + m.hint + ")";
  } else if (m.elapsed_s >= 0) {
    suffix = "  (" + wait_text(m.elapsed_s) + ")";
  } else if (!m.hint.empty()) {
    suffix = "  (" + m.hint + ")";
  }
  const int lead = 4;  // "  ✶ "
  if (lead + display_width(m.text) + display_width(suffix) > width) suffix.clear();
  const std::string body = fit(m.text, width - lead - display_width(suffix), th);

  Elements cells{text("  "), paint(th, text(std::string(g.spinner[static_cast<std::size_t>(step)])), m.t),
                 text(" ")};
  if (th.shimmer() && (m.t == tone::accent || m.t == tone::info)) {
    // A brighter band, five glyphs wide, sweeps across every 2 s.
    const auto spans = glyph_spans(body);
    const double band = 4.0;
    const double span = static_cast<double>(spans.size()) + 2 * band;
    const double centre = static_cast<double>((ms < 0 ? 0 : ms) % 2000) / 2000.0 * span - band;
    for (std::size_t i = 0; i < spans.size(); ++i) {
      const double k = std::max(0.0, 1.0 - std::abs(static_cast<double>(i) - centre) / band);
      cells.push_back(text(body.substr(spans[i].at, spans[i].len)) |
                      ftxui::color(mix(static_cast<float>(k), th.accent, th.shine)));
    }
  } else {
    cells.push_back(paint(th, text(body), m.t));
  }
  if (!suffix.empty()) cells.push_back(dimmed(th, suffix));
  return hbox(std::move(cells));
}

Element path(const theme& th, const path_model& m, int width, std::int64_t ms) {
  const glyph_set& g = *th.g;
  const bool frozen = !m.moving || m.reconnecting;
  const ftxui::Color tint = th.of(m.via);
  std::string badge = m.reconnecting ? std::string("reconnecting") + std::string(g.ellipsis)
                      : m.badge.empty() ? bearer_label(m.via)
                                        : m.badge;
  // Columns: "  " here " " line "  " badge "  " line " " there.
  const int fixed = 2 + 1 + 4 + 1;
  int here_w = display_width(m.here), there_w = display_width(m.there);
  int badge_w = display_width(badge);
  constexpr int kMinLine = 3;  // each side
  int line = width - fixed - here_w - there_w - badge_w;
  if (line < 2 * kMinLine) {
    // The line has shrunk as far as it goes: cut the names, the other's last.
    int names = std::max(0, width - fixed - badge_w - 2 * kMinLine);
    if (names < 8) {  // too narrow for a badge too
      badge.clear();
      badge_w = 0;
      names = std::max(0, width - fixed + 4 - 2 * kMinLine);
    }
    const int half = names / 2;
    const int here_room = std::min(here_w, std::max(half, names - there_w));
    here_w = display_width(fit(m.here, here_room, th));
    there_w = display_width(fit(m.there, names - here_w, th));
    line = std::max(2 * kMinLine, width - fixed - here_w - there_w - badge_w + (badge.empty() ? 4 : 0));
  }
  const std::string here = fit(m.here, here_w, th), there = fit(m.there, there_w, th);
  const int left = line / 2, right = line - left;

  // A train of three dots five cells apart, one cell a frame at 12 frames a
  // second, toward the receiver; frozen where it is while nothing moves.
  std::vector<bool> dot(static_cast<std::size_t>(line), false);
  if (line > 0) {
    const std::int64_t cycle = line + 10;
    const std::int64_t head = (ms < 0 ? 0 : ms) * 12 / 1000 % cycle;
    for (int k = 0; k < 3; ++k) {
      const std::int64_t p = head - 5 * k;
      if (p >= 0 && p < line) dot[static_cast<std::size_t>(m.sending ? p : line - 1 - p)] = true;
    }
  }
  const auto segment = [&](int from, int count) {
    Elements cells;
    std::string run;
    const auto flush = [&] {
      if (run.empty()) return;
      Element e = text(run);
      e = frozen ? paint(th, std::move(e), tone::dim) : th.colour() ? std::move(e) | ftxui::color(tint) : std::move(e);
      cells.push_back(std::move(e));
      run.clear();
    };
    for (int i = from; i < from + count; ++i) {
      if (dot[static_cast<std::size_t>(i)]) {
        flush();
        Element e = text(std::string(g.dot)) | ftxui::bold;
        e = frozen ? paint(th, std::move(e), tone::dim) : th.colour() ? std::move(e) | ftxui::color(tint) : std::move(e);
        cells.push_back(std::move(e));
      } else {
        run += g.line;
      }
    }
    flush();
    return hbox(std::move(cells));
  };
  Elements row{text("  "), text(here), text(" "), segment(0, left)};
  if (!badge.empty()) {
    Element b = text(badge) | ftxui::bold;
    b = m.reconnecting ? paint(th, std::move(b), tone::warning)
        : th.colour()  ? std::move(b) | ftxui::color(tint)
                       : std::move(b);
    row.push_back(text("  "));
    row.push_back(std::move(b));
    row.push_back(text("  "));
  }
  row.push_back(segment(left, right));
  row.push_back(text(" "));
  row.push_back(text(there) | ftxui::bold);
  return hbox(std::move(row));
}

Element bar(const theme& th, double fraction, int width) {
  const glyph_set& g = *th.g;
  fraction = std::clamp(std::isfinite(fraction) ? fraction : 0.0, 0.0, 1.0);
  const int pct = fraction >= 1.0 ? 100 : std::min(99, static_cast<int>(std::floor(fraction * 100)));
  const int bw = std::max(1, width - 11);  // "  " bar "  " " 42 %" "  "
  const double cells = fraction * bw;
  const int full = static_cast<int>(std::floor(cells));
  const int eighth = g.partial_cells ? static_cast<int>(std::floor((cells - full) * 8)) : 0;
  Elements row{text("  ")};
  const auto colour_at = [&](int i) {
    return mix(bw > 1 ? static_cast<float>(i) / static_cast<float>(bw - 1) : 0.0f, th.accent, th.gradient_end);
  };
  for (int i = 0; i < full; ++i) {
    Element e = text(std::string(g.eighths[7]));
    row.push_back(th.colour() ? std::move(e) | ftxui::color(colour_at(i)) : std::move(e));
  }
  int used = full;
  if (eighth > 0 && used < bw) {
    Element e = text(std::string(g.eighths[static_cast<std::size_t>(eighth - 1)]));
    row.push_back(th.colour() ? std::move(e) | ftxui::color(colour_at(used)) : std::move(e));
    ++used;
  }
  if (used < bw) row.push_back(dimmed(th, repeat(g.track, bw - used)));
  std::string p = std::to_string(pct) + " %";
  row.push_back(text(spaces(2 + 5 - static_cast<int>(p.size()))));
  row.push_back(text(std::move(p)) | ftxui::bold);
  return hbox(std::move(row));
}

Element stats(const theme& th, const progress_model& m, int width) {
  // Shown in this order; dropped when narrow from the least needed: the
  // file count, the hint, the speed, the time left. The bytes always stay.
  struct item {
    std::string s;
    int drop;  // lower goes first; -1 never
  };
  std::vector<item> items;
  if (m.count > 1) items.push_back({std::to_string(m.files) + " of " + std::to_string(m.count) + " files", 0});
  items.push_back({size_text(m.bytes) + " of " + size_text(m.total), -1});
  if (!m.link.reconnecting) {
    if (m.rate > 0) items.push_back({rate_text(m.rate), 2});
    if (std::string left = time_left_text(m.left_s); !left.empty()) items.push_back({std::move(left), 3});
  }
  if (!m.hint.empty()) items.push_back({m.hint, 1});
  const std::string glue = sep(th);
  const auto joined_width = [&] {
    int w = 0;
    for (std::size_t i = 0; i < items.size(); ++i) w += (i ? display_width(glue) : 0) + display_width(items[i].s);
    return w;
  };
  for (int drop = 0; drop <= 3 && joined_width() > width - 2; ++drop) {
    items.erase(std::remove_if(items.begin(), items.end(), [&](const item& it) { return it.drop == drop; }),
                items.end());
  }
  Elements row{text("  ")};
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) row.push_back(dimmed(th, glue));
    const bool bytes = items[i].drop == -1;
    std::string s = bytes ? fit(items[i].s, width - 2, th) : items[i].s;
    row.push_back(bytes ? text(std::move(s)) : dimmed(th, std::move(s)));
  }
  return hbox(std::move(row));
}

Element progress(const theme& th, const progress_model& m, int width, std::int64_t ms) {
  const double f = m.total > 0 ? static_cast<double>(m.bytes) / static_cast<double>(m.total) : 0.0;
  return vbox({path(th, m.link, width, ms), bar(th, f, width), stats(th, m, width)});
}

// ---- the cards ------------------------------------------------------------------------------

Element code_card(const theme& th, const code_card_model& m, int width) {
  const int cw = width - 4;
  Elements body;
  std::string title;
  if (m.known) {
    // #472 PR 4: a remembered device: its key is the one remembered, no code.
    title = m.peer + " (remembered) wants to send you files";
    body.push_back(lines(th, "Its key is the one you remembered.", cw, tone::normal, 2));
  } else if (m.internet) {
    // #472 PR 2: the code already proved them; the check is to compare.
    title = m.peer + " (new device) wants to send you files";
    body.push_back(blank());
    body.push_back(check_line(th, "Make sure " + m.peer + " shows", m.code, cw));
    body.push_back(lines(th, "If the six digits differ, decline.", cw, tone::dim, 2));
  } else {
    // #442: the code shows here only; the person at the sender types it.
    title = m.peer + " wants to send you files";
    body.push_back(blank());
    body.push_back(code_row(th, m.code, cw));
    body.push_back(blank());
  }
  const std::string say = m.known || m.internet ? std::string() : "Type this code on " + m.peer + ".";
  body.push_back(say_and_keys(th, say, tone::normal, {m.hint}, cw));
  return card(th, width, title, std::move(body), tone::accent);
}

std::vector<std::string> offer_hint(const theme& th) {
  return {std::string(th.g->arrows) + " or 1" + std::string(th.g->dash) + "3", "enter to choose", "y accept",
          "n or esc decline"};
}

Element offer_card(const theme& th, const offer_model& m, int width) {
  const int cw = width - 4;
  const std::string title = m.peer + (m.known ? " (remembered)" : "") + " wants to send you " +
                            files_text(m.count) + sep(th) + size_text(m.bytes);
  Elements body;
  const std::string what =
      m.folder.empty() ? names_text(m.names, m.count, cw - 2, th) : "the folder \"" + m.folder + "\"";
  body.push_back(lines(th, what, cw, tone::dim, 2));
  if (!m.relay.empty()) body.push_back(lines(th, "Through " + m.relay + ", sealed end to end.", cw, tone::dim, 2));
  if (!m.check.empty()) body.push_back(check_line(th, "Make sure " + m.peer + " shows", m.check, cw));
  body.push_back(blank());
  body.push_back(choices(th, cw,
                         {"Accept" + sep(th) + "save to " + m.save_to,
                          "Accept and save somewhere else" + std::string(th.g->ellipsis), "Decline"},
                         m.selected));
  return card(th, width, title, std::move(body), tone::accent);
}

std::vector<std::string> code_entry_hint() { return {"digits only", "it goes by itself at the sixth", "esc to cancel"}; }

Element code_entry_card(const theme& th, const code_entry_model& m, int width) {
  const int cw = width - 4;
  Elements body;
  body.push_back(blank());
  body.push_back(boxes_row(th, m.typed, cw));
  body.push_back(blank());
  // #442: typed, not compared; this screen never shows the code.
  body.push_back(lines(th, m.note.empty() ? std::string("Nothing has been sent yet.") : m.note, cw,
                       m.wrong ? tone::warning : tone::dim, 2));
  return card(th, width, "Type the code on " + m.peer + "'s screen", std::move(body), tone::accent);
}

Element internet_code_card(const theme& th, const internet_code_model& m, int width) {
  const int cw = width - 4;
  Elements body;
  body.push_back(blank());
  if (!m.code.empty() && digits_of(m.code).size() == 6 &&
      m.code.find_first_not_of("0123456789 ") == std::string::npos) {
    body.push_back(code_row(th, m.code, cw));
  } else {
    // Four words or base32 groups: large is bold and spaced.
    Elements rows;
    for (auto& l : wrap(m.code, std::max(1, cw - 5))) {
      rows.push_back(hbox({text("     "), paint(th, text(std::move(l)) | ftxui::bold, tone::accent)}));
    }
    body.push_back(vbox(std::move(rows)));
  }
  body.push_back(blank());
  std::string intro = "Give this code to the person sending. It works once";
  if (const std::string left = life_text(m.left_s); !left.empty()) intro += ", " + left;
  intro += ".";
  body.push_back(lines(th, intro, cw, tone::normal, 2));
  if (!m.link.empty()) body.push_back(hbox({text("  "), dimmed(th, fit("Link: " + m.link, cw - 2, th))}));
  if (!m.how.empty()) body.push_back(lines(th, "Ways in: " + m.how + ".", cw, tone::dim, 2));
  if (!m.note.empty()) body.push_back(lines(th, m.note, cw, tone::warning, 2));
  std::vector<std::string> keys{"c copy code"};
  if (!m.link.empty()) keys.emplace_back("l copy link");
  keys.emplace_back("esc stop");
  body.push_back(say_and_keys(th, m.copied, tone::success, keys, cw));
  return card(th, width, "Receive from anywhere", std::move(body), tone::accent);
}

Element naming_card(const theme& th, const naming_model& m, int width) {
  const int cw = width - 4;
  Elements body;
  if (!m.relay.empty()) {
    body.push_back(lines(th,
                         "Through " + (m.relay == "your relay server" ? m.relay : m.relay + " (your computer)") +
                             ", sealed end to end: it passes the bytes on and cannot read them.",
                         cw, tone::dim, 2));
  }
  body.push_back(blank());
  body.push_back(lines(th, "Check that " + m.peer + "'s screen shows", cw, tone::normal, 2));
  body.push_back(blank());
  body.push_back(code_row(th, m.check, cw));
  body.push_back(blank());
  body.push_back(lines(th, "If they differ, someone else has the code: cancel. Nothing has been sent yet.", cw,
                       tone::dim, 2));
  body.push_back(blank());
  body.push_back(choices(th, cw, {"It matches" + sep(th) + "send", "Cancel"}, m.selected));
  return card(th, width, "Connected to " + m.peer + " (new device)", std::move(body), tone::accent);
}

Element remember_card(const theme& th, const remember_model& m, int width) {
  const int cw = width - 4;
  Elements body;
  switch (m.at) {
    case remember_model::step::ask:
      body.push_back(lines(th,
                           "Send to each other later over the internet, with no code. It works once " + m.peer +
                               " remembers this computer too.",
                           cw, tone::dim, 2));
      body.push_back(blank());
      body.push_back(choices(th, cw, {"Remember", "Not now"}, m.selected));
      return card(th, width, "Remember " + m.peer + "?", std::move(body), tone::accent);
    case remember_model::step::show_check:
      body.push_back(blank());
      body.push_back(code_row(th, m.check, cw));
      body.push_back(blank());
      body.push_back(lines(th, "Type this code on " + m.peer + " so it remembers this computer.", cw,
                           tone::normal, 2));
      break;
    case remember_model::step::type_check:
      body.push_back(blank());
      body.push_back(boxes_row(th, m.typed, cw));
      body.push_back(blank());
      // #497: the receiver remembers only once its person types the sender's code.
      body.push_back(lines(th,
                           m.note.empty() ? "To remember " + m.peer +
                                                ", type the code its window shows now. It proves the computer "
                                                "that sent is the one in front of you."
                                          : m.note,
                           cw, m.note.empty() ? tone::dim : tone::warning, 2));
      break;
  }
  return card(th, width, "Remember " + m.peer, std::move(body), tone::accent);
}

// ---- the transcript ---------------------------------------------------------------------------

Element entry(const theme& th, const entry_model& m, int width) {
  const glyph_set& g = *th.g;
  Elements rows;
  const int bw = display_width(g.bullet);
  const auto head = wrap(m.text, std::max(1, width - bw - 1));
  for (std::size_t i = 0; i < head.size(); ++i) {
    rows.push_back(hbox({i == 0 ? paint(th, text(std::string(g.bullet)), m.bullet) : text(spaces(bw)), text(" "),
                         text(head[i])}));
  }
  const int ew = display_width(g.elbow);
  for (const auto& d : m.details) {
    const auto body = wrap(d, std::max(1, width - 3 - ew));
    for (std::size_t i = 0; i < body.size(); ++i) {
      rows.push_back(hbox({text("  "), i == 0 ? dimmed(th, std::string(g.elbow)) : text(spaces(ew)), text(" "),
                           dimmed(th, body[i])}));
    }
  }
  return vbox(std::move(rows));
}

Element region(Elements blocks) {
  Elements rows;
  for (auto& b : blocks) {
    if (!b) continue;
    if (!rows.empty()) rows.push_back(blank());
    rows.push_back(std::move(b));
  }
  return vbox(std::move(rows));
}

}  // namespace mv::driftwire::cli
