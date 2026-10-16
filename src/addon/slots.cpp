// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/slots.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>
#include <utility>

#include "addon/settings_values.h"
#include "core/json.h"

namespace mv::addon {
namespace {


constexpr std::size_t kMaxSurfacesPerScreen = 16;

const char* place_name(screen_place p) noexcept {
  switch (p) {
    case screen_place::info_section: return "info";
    case screen_place::window: return "window";
    case screen_place::pane: return "pane";
    case screen_place::sheet: return "sheet";
  }
  return "window";
}

std::uint32_t place_code(screen_place p) noexcept {
  switch (p) {
    case screen_place::info_section: return MV_SLOT_PLACE_INFO;
    case screen_place::window: return MV_SLOT_PLACE_WINDOW;
    case screen_place::pane: return MV_SLOT_PLACE_PANE;
    case screen_place::sheet: return MV_SLOT_PLACE_SHEET;
  }
  return MV_SLOT_PLACE_WINDOW;
}

const char* slot_name(item_slot s) noexcept {
  switch (s) {
    case item_slot::bar: return "bar";
    case item_slot::path: return "path";
    case item_slot::context: return "context";
    case item_slot::status: return "status";
  }
  return "bar";
}

const char* target_name(item_target t) noexcept {
  switch (t) {
    case item_target::none: return "none";
    case item_target::file: return "file";
    case item_target::files: return "files";
    case item_target::folder: return "folder";
  }
  return "none";
}

class native_client final : public slot_client {
 public:
  native_client(void* addon, const mv_addon_slots* t) : addon_(addon), t_(t) {}
  status info_section(const mv_slot_info_request& r) override {
    return has(offsetof(mv_addon_slots, info_section), t_->info_section)
               ? static_cast<status>(t_->info_section(addon_, &r))
               : status::unsupported_format;
  }
  status search(const mv_slot_search_request& r) override {
    return has(offsetof(mv_addon_slots, search), t_->search) ? static_cast<status>(t_->search(addon_, &r))
                                                             : status::unsupported_format;
  }
  status screen_open(const mv_slot_screen_request& r) override {
    return has(offsetof(mv_addon_slots, screen_open), t_->screen_open)
               ? static_cast<status>(t_->screen_open(addon_, &r))
               : status::unsupported_format;
  }
  status badges(const mv_slot_badges_request& r) override {
    return has(offsetof(mv_addon_slots, badges), t_->badges) ? static_cast<status>(t_->badges(addon_, &r))
                                                             : status::unsupported_format;
  }
  void event(const mv_slot_event& e) override {
    if (has(offsetof(mv_addon_slots, event), t_->event)) t_->event(addon_, &e);
  }

 private:
  template <typename F>
  bool has(std::size_t offset, F fn) const noexcept {
    return t_ && t_->struct_size >= offset + sizeof(fn) && fn != nullptr;
  }
  void* addon_;
  const mv_addon_slots* t_;
};

// Latin-1 Supplement and Latin Extended-A letters to their base letter, for
// U+00C0..U+017F. '\0' = keep the character as it is.
constexpr char kFoldLatin[] =
    "aaaaaaaceeeeiiiidnooooo\0ouuuuytsaaaaaaaceeeeiiiidnooooo\0ouuuuyty"
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllll"
    "nnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
static_assert(sizeof(kFoldLatin) == 0x180 - 0xC0 + 1, "one letter per code point U+00C0..U+017F");

}  // namespace

std::unique_ptr<slot_client> native_slot_client(void* addon, const mv_addon_slots* table) {
  return std::make_unique<native_client>(addon, table);
}

std::optional<mv_addon_location> parse_location_text(std::string_view text) noexcept {
  // "48.85837° N, 2.29448° E": number, degree sign (UTF-8 C2 B0), space, N/S,
  // comma, space, number, degree sign, space, E/W.
  auto parse_one = [](std::string_view& s, char pos, char neg, double limit, double& out) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ')) ++i;
    const std::size_t start = i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
    if (i == start) return false;
    double v = 0;
    double scale = 0;
    for (std::size_t k = start; k < i; ++k) {
      if (s[k] == '.') {
        if (scale != 0) return false;
        scale = 1;
        continue;
      }
      if (scale != 0) {
        scale *= 10;
        v += (s[k] - '0') / scale;
      } else {
        v = v * 10 + (s[k] - '0');
      }
    }
    if (s.substr(i, 2) != "\xC2\xB0") return false;
    i += 2;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) return false;
    if (s[i] == neg) {
      v = -v;
    } else if (s[i] != pos) {
      return false;
    }
    if (v > limit || v < -limit) return false;
    out = v;
    s.remove_prefix(i + 1);
    return true;
  };
  mv_addon_location loc{};
  std::string_view s = text;
  if (!parse_one(s, 'N', 'S', 90.0, loc.lat)) return std::nullopt;
  if (s.empty() || s.front() != ',') return std::nullopt;
  s.remove_prefix(1);
  if (!parse_one(s, 'E', 'W', 180.0, loc.lon)) return std::nullopt;
  if (!s.empty()) return std::nullopt;
  loc.has = 1;
  return loc;
}

std::string fold_query(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
      ++i;
      continue;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      const std::uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
      if (cp >= 0xC0 && cp <= 0x17F) {
        const char base = kFoldLatin[cp - 0xC0];
        if (base != '\0') {
          out.push_back(base);
          if (cp == 0xDF) out.push_back('s');  // ß -> ss
          i += 2;
          continue;
        }
      }
      // A combining mark (U+0300..U+036F) is dropped: decomposed names.
      if (cp >= 0x300 && cp <= 0x36F) {
        i += 2;
        continue;
      }
      out.append(text.substr(i, 2));
      i += 2;
      continue;
    }
    // Longer sequences pass through.
    std::size_t len = (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (i + len > text.size()) len = text.size() - i;
    out.append(text.substr(i, len));
    i += len;
  }
  return out;
}

bool has_place_terms(std::string_view s) {
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  // ASCII ", or the typographic “ ” a Mac field types (query.cpp quote_at).
  const auto quote_at = [&](std::size_t i) -> std::size_t {
    if (i >= s.size()) return 0;
    if (s[i] == '"') return 1;
    if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2 && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        (static_cast<unsigned char>(s[i + 2]) == 0x9C || static_cast<unsigned char>(s[i + 2]) == 0x9D)) {
      return 3;
    }
    return 0;
  };
  const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  std::size_t i = 0;
  while (i < s.size()) {
    if (space(s[i])) {
      ++i;
      continue;
    }
    if (s[i] == '-' && i + 1 < s.size() && !space(s[i + 1]) && s[i + 1] != '-') ++i;
    std::string op;
    if (i < s.size() && s[i] == '@') {
      ++i;
    } else {
      std::size_t j = i;
      while (j < s.size() && letter(s[j])) ++j;
      if (j > i && j < s.size() && s[j] == ':') {
        op = fold_query(s.substr(i, j - i));
        i = j + 1;
        // Name:"words": any other word before a colon is a word, and what
        // follows is a term of its own.
        if (op != "place" && op != "in" && op != "before" && op != "after" && op != "since" && op != "until" &&
            op != "is" && op != "type" && op != "kind" && op != "person" && op != "said" && op != "file" &&
            op != "camera" && op != "lens" && op != "ai" && op != "made" && op != "rating" && op != "label" &&
            op != "iso" && op != "f" && op != "mm" && op != "shutter") {
          continue;
        }
      }
    }
    // The term's text: quoted (to the closing quote, or the end), or a word.
    std::size_t begin = i, end = i;
    if (const std::size_t q = quote_at(i)) {
      begin = i + q;
      end = begin;
      std::size_t ql = 0;
      while (end < s.size() && (ql = quote_at(end)) == 0) ++end;
      i = end >= s.size() ? s.size() : end + ql;
    } else {
      while (end < s.size() && !space(s[end]) && quote_at(end) == 0) ++end;
      i = end;
    }
    if (op != "place" && op != "in") continue;
    std::size_t k = begin;
    while (k < end && space(s[k])) ++k;
    if (k == end) continue;  // "place:" still being typed
    // in: is a date when its word starts with a digit (in:2024); never a place.
    if (op == "in" && s[k] >= '0' && s[k] <= '9') continue;
    return true;
  }
  return false;
}

// ---- workers ------------------------------------------------------------------

struct slot_registry::worker {
  struct task {
    std::string key;  // non-empty: a newer task with the same key replaces this one
    std::function<void(slot_client&)> run;
  };
  std::mutex m;
  std::condition_variable cv;
  std::condition_variable idle_cv;
  std::deque<task> queue;
  bool stopping = false;
  bool busy = false;
  std::thread thread;
};

struct slot_registry::addon_entry {
  std::string id;
  std::string name;
  std::string dir;
  contributions c;
  std::unique_ptr<slot_client> client;
  std::unique_ptr<worker> w;
};

slot_registry::slot_registry() {
  // A changed setting re-asks the add-on it belongs to (docs/design/25 "A
  // changed setting re-asks"). Generic: whichever add-on's page it was.
  settings_listener_ = add_settings_listener(
      [this](std::string_view addon_id) { settings_changed(std::string(addon_id)); });
}

slot_registry::~slot_registry() {
  // First: on return no settings write is inside settings_changed, and none
  // will reach this registry again.
  remove_settings_listener(settings_listener_);
  // The process's registry dies at exit (a static), after the chromes: no
  // host callback may run from here (on Windows that would be managed code
  // under the loader lock).
  set_listener({});
  for (const std::string& id : attached_ids()) detach(id);
}

void slot_registry::set_listener(std::function<void(std::uint32_t)> listener) {
  std::lock_guard lock(listener_m_);
  listener_ = std::move(listener);
}

void slot_registry::notify(std::uint32_t bits) {
  std::lock_guard lock(listener_m_);
  if (listener_) listener_(bits);
}

status slot_registry::attach(const std::string& addon_id, const std::string& addon_name,
                             contributions c, std::unique_ptr<slot_client> client,
                             std::string package_dir) {
  if (addon_id.empty() || !client || c.empty()) return status::invalid_arg;
  auto e = std::make_unique<addon_entry>();
  e->id = addon_id;
  e->name = addon_name;
  e->dir = std::move(package_dir);
  // Resolved once: sections in display order.
  std::stable_sort(c.info_sections.begin(), c.info_sections.end(),
                   [](const info_section_contrib& a, const info_section_contrib& b) {
                     return a.order < b.order;
                   });
  e->c = std::move(c);
  e->client = std::move(client);
  e->w = std::make_unique<worker>();
  {
    std::lock_guard lock(m_);
    if (addons_.count(addon_id)) return status::invalid_arg;
    worker* w = e->w.get();
    slot_client* cl = e->client.get();
    std::atomic<std::uint64_t>* calls = &calls_;
    w->thread = std::thread([w, cl, calls] {
      std::unique_lock lk(w->m);
      for (;;) {
        w->cv.wait(lk, [w] { return w->stopping || !w->queue.empty(); });
        if (w->stopping) break;
        worker::task t = std::move(w->queue.front());
        w->queue.pop_front();
        w->busy = true;
        lk.unlock();
        calls->fetch_add(1);
        t.run(*cl);
        lk.lock();
        w->busy = false;
        if (w->queue.empty()) w->idle_cv.notify_all();
      }
      w->busy = false;
      w->idle_cv.notify_all();
    });
    addons_.emplace(addon_id, std::move(e));
  }
  notify(slot_changed_contributions | slot_changed_status);
  return status::ok;
}

void slot_registry::detach(const std::string& addon_id) {
  std::unique_ptr<addon_entry> e;
  {
    std::lock_guard lock(m_);
    auto it = addons_.find(addon_id);
    if (it == addons_.end()) return;
    e = std::move(it->second);
    addons_.erase(it);
    // Every item it contributed and everything it posted goes at once.
    for (auto i = info_.begin(); i != info_.end();) i = i->first.first == addon_id ? info_.erase(i) : std::next(i);
    for (auto i = screens_.begin(); i != screens_.end();) {
      i = i->first.first == addon_id ? screens_.erase(i) : std::next(i);
    }
    for (auto& s : search_) {
      s.providers.erase(std::remove_if(s.providers.begin(), s.providers.end(),
                                       [&](const provider_state& p) { return p.addon == addon_id; }),
                        s.providers.end());
      s.rows.erase(std::remove_if(s.rows.begin(), s.rows.end(),
                                  [&](const auto& r) { return r.second.first == addon_id; }),
                   s.rows.end());
    }
    const std::string prefix = addon_id + "/";
    for (auto i = badge_sets_.begin(); i != badge_sets_.end();) {
      i = i->first.rfind(prefix, 0) == 0 ? badge_sets_.erase(i) : std::next(i);
    }
    for (auto i = status_.begin(); i != status_.end();) {
      i = i->first.first == addon_id ? status_.erase(i) : std::next(i);
    }
    private_cv_.notify_all();  // a provider_paths waiting on it returns not_found
  }
  {
    std::lock_guard lk(e->w->m);
    e->w->stopping = true;
    e->w->queue.clear();
  }
  e->w->cv.notify_all();
  if (e->w->thread.joinable()) {
    if (e->w->thread.get_id() == std::this_thread::get_id()) {
      // Detached from inside its own call (a script stopping itself): the
      // thread finishes that call and exits; the entry must outlive it.
      e->w->thread.detach();
      (void)e.release();  // a few hundred bytes, once, rather than a use after free
    } else {
      e->w->thread.join();
    }
  }
  notify(slot_changed_contributions | slot_changed_info | slot_changed_screens | slot_changed_search |
         slot_changed_badges | slot_changed_status);
}

bool slot_registry::attached(const std::string& addon_id) const {
  std::lock_guard lock(m_);
  return addons_.count(addon_id) != 0;
}

std::vector<std::string> slot_registry::attached_ids() const {
  std::lock_guard lock(m_);
  std::vector<std::string> out;
  for (const auto& [id, e] : addons_) out.push_back(id);
  return out;
}

void slot_registry::enqueue(addon_entry& e, std::string key, std::function<void(slot_client&)> task) {
  worker& w = *e.w;
  {
    std::lock_guard lk(w.m);
    if (w.stopping) return;
    if (!key.empty()) {
      for (auto& t : w.queue) {
        if (t.key == key) {
          t.run = std::move(task);  // latest wins, keeps its place
          return;
        }
      }
    }
    w.queue.push_back(worker::task{std::move(key), std::move(task)});
  }
  w.cv.notify_one();
}

void slot_registry::drain() {
  std::vector<worker*> ws;
  {
    std::lock_guard lock(m_);
    for (auto& [id, e] : addons_) ws.push_back(e->w.get());
  }
  for (worker* w : ws) {
    std::unique_lock lk(w->m);
    w->idle_cv.wait(lk, [w] { return w->stopping || (w->queue.empty() && !w->busy); });
  }
}

// ---- requests ---------------------------------------------------------------------

std::uint64_t slot_registry::request_info(const std::string& path, const std::string& location_text,
                                          const mv_slot_theme& theme, float width_pt, float scale) {
  const auto loc = parse_location_text(location_text);
  std::uint64_t g = 0;
  bool had = false;
  {
    std::lock_guard lock(m_);
    g = next_generation();
    had = !info_.empty();
    info_.clear();
    info_generation_ = g;
    info_path_ = path;
    info_location_text_ = location_text;
    info_theme_ = theme;
    info_width_pt_ = width_pt;
    info_scale_ = scale;
    last_theme_ = theme;
    last_scale_ = scale;
    for (auto& [id, e] : addons_) ask_info_locked(*e, g, loc);
  }
  if (had) notify(slot_changed_info);
  return g;
}

void slot_registry::ask_info_locked(addon_entry& e, std::uint64_t g,
                                    const std::optional<mv_addon_location>& loc) {
  for (const info_section_contrib& s : e.c.info_sections) {
    if (s.needs_location && !loc) continue;  // never asked for a file without one
    // A section asked again (settings_changed) keeps the tree it showed until
    // the answer replaces it; only its generation moves, so a late answer to
    // the old request is dropped.
    screen_state& st = info_[std::make_pair(e.id, s.id)];
    st.addon = e.id;
    st.id = s.id;
    st.place = screen_place::info_section;
    st.generation = g;
    const std::string section = s.id;
    enqueue(e, "info:" + section,
            [g, section, path = info_path_, location_text = info_location_text_, loc, theme = info_theme_,
             width_pt = info_width_pt_, scale = info_scale_](slot_client& c) {
              mv_slot_info_request r{};
              r.struct_size = sizeof r;
              r.generation = g;
              r.section_id = section.c_str();
              r.path_utf8 = path.c_str();
              if (loc) r.location = *loc;
              r.location_text = location_text.c_str();
              r.theme = theme;
              r.width_pt = width_pt;
              r.scale = scale;
              (void)c.info_section(r);
            });
  }
}

void slot_registry::settings_changed(const std::string& addon_id) {
  bool badges_dropped = false;
  {
    std::lock_guard lock(m_);
    const auto it = addons_.find(addon_id);
    if (it == addons_.end()) return;
    addon_entry& e = *it->second;
    // The file on screen: this add-on's sections, under a generation of their own.
    if (info_generation_ != 0 && !e.c.info_sections.empty()) {
      ask_info_locked(e, next_generation(), parse_location_text(info_location_text_));
    }
    // The folder shown: this add-on's badge sets go (a setting may change which
    // files carry them) and it is asked again under the folder's generation.
    if (badges_generation_ != 0 && !e.c.badges.empty()) {
      const std::string prefix = addon_id + "/";
      badges_dropped =
          std::erase_if(badge_sets_, [&](const auto& b) { return b.first.rfind(prefix, 0) == 0; }) > 0;
      enqueue(e, "badges", [g = badges_generation_, folder = badges_folder_](slot_client& c) {
        mv_slot_badges_request r{};
        r.struct_size = sizeof r;
        r.generation = g;
        r.folder_utf8 = folder.c_str();
        (void)c.badges(r);
      });
    }
  }
  if (badges_dropped) notify(slot_changed_badges);
}

void slot_registry::clear_info() {
  bool had = false;
  {
    std::lock_guard lock(m_);
    had = !info_.empty();
    info_.clear();
    info_generation_ = 0;
    info_path_.clear();
    info_location_text_.clear();
  }
  if (had) notify(slot_changed_info);
}

std::uint64_t slot_registry::search(std::uint32_t scope, const std::string& query,
                                    const std::string& folder) {
  if (scope != MV_SLOT_SCOPE_FILES && scope != MV_SLOT_SCOPE_LOCAL) return 0;
  search_state& s = search_[scope == MV_SLOT_SCOPE_FILES ? 0 : 1];
  std::string trimmed = query;
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(0, 1);
  std::uint64_t g = 0;
  {
    std::lock_guard lock(m_);
    g = next_generation();
    s.generation = g;
    s.query = trimmed;
    s.providers.clear();
    s.rows.clear();
    // #441: the AI pack answers a Local search query's place terms itself
    // (provider_paths, intersected with the rest of the query), so its own
    // results are the whole answer and nobody else is asked.
    if (!trimmed.empty() && !(scope == MV_SLOT_SCOPE_LOCAL && has_place_terms(trimmed))) {
      const std::string folded = fold_query(trimmed);
      // A query that starts with a provider's prefix ("place:") goes to the
      // providers that declare it, prefix removed; any other query to every
      // provider of the scope.
      std::string routed_prefix;
      for (auto& [id, e] : addons_) {
        for (const search_provider_contrib& p : e->c.search_providers) {
          for (const std::string& pre : p.prefixes) {
            if (folded.rfind(pre, 0) != 0 || pre.size() <= routed_prefix.size()) continue;
            // A digit after the prefix is a date term, not a place: Local search's own
            // `in:2024` (docs/design/17) stays the AI parser's, and goes to everyone.
            std::size_t after = pre.size();
            while (after < folded.size() && folded[after] == ' ') ++after;
            if (after < folded.size() && folded[after] >= '0' && folded[after] <= '9') continue;
            routed_prefix = pre;
          }
        }
      }
      for (auto& [id, e] : addons_) {
        for (const search_provider_contrib& p : e->c.search_providers) {
          if (!(scope == MV_SLOT_SCOPE_FILES ? p.files : p.local)) continue;
          if (!routed_prefix.empty() &&
              std::find(p.prefixes.begin(), p.prefixes.end(), routed_prefix) == p.prefixes.end()) {
            continue;
          }
          std::string q = trimmed;
          std::string f = folded;
          if (!routed_prefix.empty()) {
            // The typed prefix can be longer than its fold ("PLÀCE:" -> "place:",
            // a decomposed accent): cut where the typed text folds to it.
            std::size_t cut = std::min(q.size(), routed_prefix.size());
            for (std::size_t i = routed_prefix.size(); i <= q.size() && i <= routed_prefix.size() * 4; ++i) {
              if (fold_query(std::string_view(q).substr(0, i)) == routed_prefix) {
                cut = i;
                break;
              }
            }
            q = q.substr(cut);
            f = f.substr(routed_prefix.size());
            while (!q.empty() && q.front() == ' ') q.erase(0, 1);
            while (!f.empty() && f.front() == ' ') f.erase(0, 1);
          }
          s.providers.push_back(provider_state{id, p.id, p.name, false, 0, 0});
          const std::string provider = p.id;
          enqueue(*e, "search:" + std::to_string(scope) + ":" + provider,
                  [g, scope, provider, q, f, folder](slot_client& c) {
                    mv_slot_search_request r{};
                    r.struct_size = sizeof r;
                    r.scope = scope;
                    r.generation = g;
                    r.provider_id = provider.c_str();
                    r.query_utf8 = q.c_str();
                    r.folded_utf8 = f.c_str();
                    r.folder_utf8 = folder.c_str();
                    (void)c.search(r);
                  });
        }
      }
    }
  }
  notify(slot_changed_search);
  return g;
}

bool slot_registry::answers_prefix(std::uint32_t scope, std::string_view prefix) const {
  std::lock_guard lock(m_);
  for (const auto& [id, e] : addons_) {
    for (const search_provider_contrib& p : e->c.search_providers) {
      if (!(scope == MV_SLOT_SCOPE_FILES ? p.files : p.local)) continue;
      if (std::find(p.prefixes.begin(), p.prefixes.end(), prefix) != p.prefixes.end()) return true;
    }
  }
  return false;
}

result<std::vector<std::string>> slot_registry::provider_paths(const std::string& prefix, const std::string& query,
                                                               const std::function<bool()>& keep_going) {
  std::string q = query;
  while (!q.empty() && (q.back() == ' ' || q.back() == '\t')) q.pop_back();
  while (!q.empty() && (q.front() == ' ' || q.front() == '\t')) q.erase(0, 1);
  std::uint64_t g = 0;
  std::string addon;
  {
    std::lock_guard lock(m_);
    addon_entry* found = nullptr;
    const search_provider_contrib* provider = nullptr;
    for (auto& [id, e] : addons_) {
      for (const search_provider_contrib& p : e->c.search_providers) {
        if (!p.local || std::find(p.prefixes.begin(), p.prefixes.end(), prefix) == p.prefixes.end()) continue;
        found = e.get();
        provider = &p;
        break;
      }
      if (found) break;
    }
    if (!found) return err(status::not_found);
    g = next_generation();
    addon = found->id;
    private_[g] = private_search{addon, provider->id, {}, false};
    const std::string provider_id = provider->id;
    const std::string folded = fold_query(q);
    // Its own key: a chrome's search of the same provider never replaces it.
    enqueue(*found, "paths:" + std::to_string(g), [this, g, provider_id, q, folded](slot_client& c) {
      mv_slot_search_request r{};
      r.struct_size = sizeof r;
      r.scope = MV_SLOT_SCOPE_LOCAL;
      r.generation = g;
      r.provider_id = provider_id.c_str();
      r.query_utf8 = q.c_str();
      r.folded_utf8 = folded.c_str();
      r.folder_utf8 = "";  // the whole index (locations_of_index)
      (void)c.search(r);
      // Returned: done, whether or not it said so (a failed call posts nothing).
      std::lock_guard done_lock(m_);
      if (auto it = private_.find(g); it != private_.end()) it->second.done = true;
      private_cv_.notify_all();
    });
  }
  std::unique_lock lock(m_);
  for (;;) {
    private_cv_.wait_for(lock, std::chrono::milliseconds(20), [&] {
      const auto it = private_.find(g);
      return it == private_.end() || it->second.done || addons_.count(addon) == 0;
    });
    const auto it = private_.find(g);
    if (it == private_.end() || addons_.count(addon) == 0) {
      if (it != private_.end()) private_.erase(it);
      return err(status::not_found);
    }
    const bool done = it->second.done;
    lock.unlock();
    const bool wanted = !keep_going || keep_going();
    lock.lock();
    const auto again = private_.find(g);
    if (!wanted || again == private_.end()) {
      // Erased: is_stale says so, and the provider's walk stops.
      if (again != private_.end()) private_.erase(again);
      return err(wanted ? status::not_found : status::cancelled);
    }
    if (done) {
      std::vector<std::string> paths = std::move(again->second.paths);
      private_.erase(again);
      return paths;
    }
  }
}

std::uint64_t slot_registry::request_badges(const std::string& folder) {
  std::uint64_t g = 0;
  bool had = false;
  {
    std::lock_guard lock(m_);
    g = next_generation();
    badges_generation_ = g;
    badges_folder_ = folder;
    had = !badge_sets_.empty();
    badge_sets_.clear();
    for (auto& [id, e] : addons_) {
      if (e->c.badges.empty()) continue;
      enqueue(*e, "badges", [g, folder](slot_client& c) {
        mv_slot_badges_request r{};
        r.struct_size = sizeof r;
        r.generation = g;
        r.folder_utf8 = folder.c_str();
        (void)c.badges(r);
      });
    }
  }
  if (had) notify(slot_changed_badges);
  return g;
}

status slot_registry::open_screen_locked(addon_entry& e, const screen_contrib& s,
                                         const std::string& path, const mv_slot_theme& theme,
                                         float width_pt, float height_pt, float scale) {
  const auto key = std::make_pair(e.id, s.id);
  if (screens_.count(key)) return status::ok;  // already open: the chrome brings it forward
  screen_state st;
  st.addon = e.id;
  st.id = s.id;
  st.place = s.place;
  st.generation = next_generation();
  const std::uint64_t g = st.generation;
  screens_.emplace(key, std::move(st));
  const std::string id = s.id;
  const std::uint32_t place = place_code(s.place);
  const float w = width_pt > 0 ? width_pt : static_cast<float>(s.width);
  const float h = height_pt > 0 ? height_pt : static_cast<float>(s.height);
  enqueue(e, "screen:" + id, [g, id, place, path, theme, w, h, scale](slot_client& c) {
    mv_slot_screen_request r{};
    r.struct_size = sizeof r;
    r.place = place;
    r.generation = g;
    r.screen_id = id.c_str();
    r.path_utf8 = path.c_str();
    r.theme = theme;
    r.width_pt = w;
    r.height_pt = h;
    r.scale = scale;
    (void)c.screen_open(r);
  });
  return status::ok;
}

status slot_registry::open_screen(const std::string& addon_id, const std::string& screen_id,
                                  const std::string& path, const mv_slot_theme& theme, float width_pt,
                                  float height_pt, float scale) {
  status st = status::not_found;
  {
    std::lock_guard lock(m_);
    auto it = addons_.find(addon_id);
    if (it == addons_.end()) return status::not_found;
    last_theme_ = theme;
    last_scale_ = scale;
    for (const screen_contrib& s : it->second->c.screens) {
      if (s.id == screen_id) {
        st = open_screen_locked(*it->second, s, path, theme, width_pt, height_pt, scale);
        break;
      }
    }
  }
  if (st == status::ok) notify(slot_changed_screens);
  return st;
}

status slot_registry::close_screen(const std::string& addon_id, const std::string& screen_id) {
  {
    std::lock_guard lock(m_);
    auto s = screens_.find(std::make_pair(addon_id, screen_id));
    if (s == screens_.end()) return status::not_found;
    const std::uint64_t g = s->second.generation;
    screens_.erase(s);
    auto it = addons_.find(addon_id);
    if (it != addons_.end()) {
      enqueue(*it->second, {}, [g, screen_id](slot_client& c) {
        mv_slot_event e{};
        e.struct_size = sizeof e;
        e.kind = MV_SLOT_EVENT_CLOSE;
        e.generation = g;
        e.screen_id = screen_id.c_str();
        e.node_id = "";
        e.value_json = "";
        c.event(e);
      });
    }
  }
  notify(slot_changed_screens);
  return status::ok;
}

status slot_registry::send_event(const std::string& addon_id, const std::string& screen_id,
                                 const std::string& node_id, std::uint32_t kind,
                                 const std::string& value_json) {
  std::lock_guard lock(m_);
  auto it = addons_.find(addon_id);
  if (it == addons_.end()) return status::not_found;
  screen_state* st = nullptr;
  if (auto s = screens_.find(std::make_pair(addon_id, screen_id)); s != screens_.end()) {
    st = &s->second;
  } else if (auto i = info_.find(std::make_pair(addon_id, screen_id)); i != info_.end()) {
    st = &i->second;
  }
  if (!st) return status::not_found;
  if (st->tree.check_event(node_id, kind, value_json) != screen::error::none) return status::invalid_arg;
  const std::uint64_t g = st->generation;
  enqueue(*it->second, {}, [g, screen_id, node_id, kind, value_json](slot_client& c) {
    mv_slot_event e{};
    e.struct_size = sizeof e;
    e.kind = kind;
    e.generation = g;
    e.screen_id = screen_id.c_str();
    e.node_id = node_id.c_str();
    e.value_json = value_json.c_str();
    c.event(e);
  });
  return status::ok;
}

status slot_registry::run_command(const std::string& addon_id, const std::string& command_id,
                                  const std::string& paths_json) {
  const auto paths = json::parse(paths_json, 4);
  if (!paths || paths->k != json::kind::array || paths->a.size() > screen::kMaxItems ||
      !std::all_of(paths->a.begin(), paths->a.end(),
                   [](const json::value& v) { return v.k == json::kind::string; })) {
    return status::invalid_arg;
  }
  std::lock_guard lock(m_);
  auto it = addons_.find(addon_id);
  if (it == addons_.end()) return status::not_found;
  addon_entry& e = *it->second;
  if (std::find(e.c.commands.begin(), e.c.commands.end(), command_id) == e.c.commands.end()) {
    return status::not_found;
  }
  enqueue(e, {}, [command_id, paths_json](slot_client& c) {
    mv_slot_event ev{};
    ev.struct_size = sizeof ev;
    ev.kind = MV_SLOT_EVENT_COMMAND;
    ev.screen_id = "";
    ev.node_id = command_id.c_str();
    ev.value_json = paths_json.c_str();
    c.event(ev);
  });
  return status::ok;
}

status slot_registry::invoke_item(const std::string& addon_id, const std::string& item_id,
                                  const std::string& paths_json) {
  // The paths are the chrome's; check the shape before an add-on sees them.
  const auto paths = json::parse(paths_json, 4);
  if (!paths || paths->k != json::kind::array || paths->a.size() > screen::kMaxItems ||
      !std::all_of(paths->a.begin(), paths->a.end(),
                   [](const json::value& v) { return v.k == json::kind::string; })) {
    return status::invalid_arg;
  }
  status st = status::not_found;
  bool opened = false;
  {
    std::lock_guard lock(m_);
    auto it = addons_.find(addon_id);
    if (it == addons_.end()) return status::not_found;
    addon_entry& e = *it->second;
    for (const item_contrib& item : e.c.items) {
      if (item.id != item_id) continue;
      if (!item.opens.empty()) {
        for (const screen_contrib& s : e.c.screens) {
          if (s.id == item.opens) {
            const std::string first = paths->a.empty() ? std::string() : paths->a.front().s;
            st = open_screen_locked(e, s, first, last_theme_, 0, 0, last_scale_);
            opened = st == status::ok;
          }
        }
        break;
      }
      enqueue(e, {}, [item_id, paths_json](slot_client& c) {
        mv_slot_event ev{};
        ev.struct_size = sizeof ev;
        ev.kind = MV_SLOT_EVENT_COMMAND;
        ev.screen_id = "";
        ev.node_id = item_id.c_str();
        ev.value_json = paths_json.c_str();
        c.event(ev);
      });
      st = status::ok;
      break;
    }
  }
  if (opened) notify(slot_changed_screens);
  return st;
}

// ---- reads ----------------------------------------------------------------------

std::string slot_registry::contributions_json() const {
  std::lock_guard lock(m_);
  json::writer w;
  w.begin_object();
  w.key("addons").begin_array();
  for (const auto& [id, e] : addons_) {
    w.begin_object().key("id").string(id).key("name").string(e->name).key("dir").string(e->dir).end_object();
  }
  w.end_array();
  // Sections in display order across add-ons: order, then add-on, then id.
  struct sec {
    const std::string* addon;
    const info_section_contrib* s;
  };
  std::vector<sec> secs;
  for (const auto& [id, e] : addons_) {
    for (const auto& s : e->c.info_sections) secs.push_back({&id, &s});
  }
  std::stable_sort(secs.begin(), secs.end(), [](const sec& a, const sec& b) {
    if (a.s->order != b.s->order) return a.s->order < b.s->order;
    if (*a.addon != *b.addon) return *a.addon < *b.addon;
    return a.s->id < b.s->id;
  });
  w.key("sections").begin_array();
  for (const sec& s : secs) {
    w.begin_object()
        .key("addon").string(*s.addon)
        .key("id").string(s.s->id)
        .key("title").string(s.s->title)
        .key("location").boolean(s.s->needs_location)
        .end_object();
  }
  w.end_array();
  w.key("screens").begin_array();
  for (const auto& [id, e] : addons_) {
    for (const auto& s : e->c.screens) {
      w.begin_object()
          .key("addon").string(id)
          .key("id").string(s.id)
          .key("title").string(s.title)
          .key("place").string(place_name(s.place))
          .key("width").integer(s.width)
          .key("height").integer(s.height)
          .end_object();
    }
  }
  w.end_array();
  bool local = false;
  w.key("providers").begin_array();
  for (const auto& [id, e] : addons_) {
    for (const auto& p : e->c.search_providers) {
      local = local || p.local;
      w.begin_object().key("addon").string(id).key("id").string(p.id).key("name").string(p.name);
      w.key("prefixes").begin_array();
      for (const auto& pre : p.prefixes) w.string(pre);
      w.end_array();
      w.key("files").boolean(p.files).key("local").boolean(p.local).end_object();
    }
  }
  w.end_array();
  w.key("items").begin_array();
  for (const auto& [id, e] : addons_) {
    for (const auto& i : e->c.items) {
      w.begin_object()
          .key("addon").string(id)
          .key("id").string(i.id)
          .key("slot").string(slot_name(i.slot))
          .key("label").string(i.label)
          .key("symbol").string(i.symbol)
          .key("opens").string(i.opens)
          .key("target").string(target_name(i.target))
          .end_object();
    }
  }
  w.end_array();
  w.key("badges").begin_array();
  for (const auto& [id, e] : addons_) {
    for (const auto& b : e->c.badges) {
      w.begin_object()
          .key("addon").string(id)
          .key("id").string(b.id)
          .key("key").string(id + "/" + b.id)
          .key("label").string(b.label)
          .key("symbol").string(b.symbol)
          .end_object();
    }
  }
  w.end_array();
  w.key("local_search").boolean(local);
  w.end_object();
  return w.take();
}

std::string slot_registry::info_json() const {
  std::lock_guard lock(m_);
  json::writer w;
  w.begin_object();
  w.key("generation").integer(static_cast<std::int64_t>(info_generation_));
  w.key("path").string(info_path_);
  w.key("sections").begin_array();
  struct sec {
    std::int32_t order;
    const screen_state* st;
    const std::string* title;
  };
  std::vector<sec> ready;
  for (const auto& [key, st] : info_) {
    if (st.tree.empty()) continue;
    auto it = addons_.find(key.first);
    if (it == addons_.end()) continue;
    for (const auto& s : it->second->c.info_sections) {
      if (s.id == key.second) ready.push_back({s.order, &st, &s.title});
    }
  }
  std::stable_sort(ready.begin(), ready.end(), [](const sec& a, const sec& b) {
    if (a.order != b.order) return a.order < b.order;
    if (a.st->addon != b.st->addon) return a.st->addon < b.st->addon;
    return a.st->id < b.st->id;
  });
  for (const sec& s : ready) {
    w.begin_object()
        .key("addon").string(s.st->addon)
        .key("id").string(s.st->id)
        .key("title").string(*s.title)
        .key("revision").integer(static_cast<std::int64_t>(s.st->tree.revision()))
        .key("tree").raw(s.st->tree.json())
        .end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

std::string slot_registry::screens_json() const {
  std::lock_guard lock(m_);
  json::writer w;
  w.begin_array();
  for (const auto& [key, st] : screens_) {
    auto it = addons_.find(key.first);
    if (it == addons_.end()) continue;
    const screen_contrib* def = nullptr;
    for (const auto& s : it->second->c.screens) {
      if (s.id == key.second) def = &s;
    }
    if (!def) continue;
    // The newest surface revision (registry-unique): a post_surface moves this,
    // not the tree's revision, so a chrome that rebuilds on revision alone
    // would never show new pixels.
    std::uint64_t pixels = 0;
    for (const auto& [sid, px] : st.surfaces) pixels = std::max(pixels, px.revision);
    w.begin_object()
        .key("addon").string(st.addon)
        .key("id").string(st.id)
        .key("title").string(def->title)
        .key("place").string(place_name(def->place))
        .key("width").integer(def->width)
        .key("height").integer(def->height)
        .key("generation").integer(static_cast<std::int64_t>(st.generation))
        .key("revision").integer(static_cast<std::int64_t>(st.tree.revision()))
        .key("pixels").integer(static_cast<std::int64_t>(pixels))
        .key("tree").raw(st.tree.json())
        .end_object();
  }
  w.end_array();
  return w.take();
}

bool slot_registry::surface(const std::string& addon_id, const std::string& screen_id,
                            const std::string& surface_id, surface_pixels& out) const {
  std::lock_guard lock(m_);
  const screen_state* st = nullptr;
  if (auto s = screens_.find(std::make_pair(addon_id, screen_id)); s != screens_.end()) {
    st = &s->second;
  } else if (auto i = info_.find(std::make_pair(addon_id, screen_id)); i != info_.end()) {
    st = &i->second;
  }
  if (!st) return false;
  auto it = st->surfaces.find(surface_id);
  if (it == st->surfaces.end()) return false;
  out = it->second;
  return true;
}

bool slot_registry::surface_size(const std::string& addon_id, const std::string& screen_id,
                                 const std::string& surface_id, std::uint32_t& width, std::uint32_t& height,
                                 std::uint64_t& revision) const {
  std::lock_guard lock(m_);
  const screen_state* st = nullptr;
  if (auto s = screens_.find(std::make_pair(addon_id, screen_id)); s != screens_.end()) {
    st = &s->second;
  } else if (auto i = info_.find(std::make_pair(addon_id, screen_id)); i != info_.end()) {
    st = &i->second;
  }
  if (!st) return false;
  auto it = st->surfaces.find(surface_id);
  if (it == st->surfaces.end()) return false;
  width = it->second.width;
  height = it->second.height;
  revision = it->second.revision;
  return true;
}

std::string slot_registry::search_json(std::uint32_t scope) const {
  std::lock_guard lock(m_);
  if (scope != MV_SLOT_SCOPE_FILES && scope != MV_SLOT_SCOPE_LOCAL) return "{}";
  const search_state& s = search_[scope == MV_SLOT_SCOPE_FILES ? 0 : 1];
  json::writer w;
  w.begin_object();
  w.key("generation").integer(static_cast<std::int64_t>(s.generation));
  w.key("scope").integer(scope);
  w.key("query").string(s.query);
  const bool searching = std::any_of(s.providers.begin(), s.providers.end(),
                                     [](const provider_state& p) { return !p.done; });
  w.key("searching").boolean(searching);
  w.key("providers").begin_array();
  for (const provider_state& p : s.providers) {
    w.begin_object()
        .key("addon").string(p.addon)
        .key("id").string(p.id)
        .key("name").string(p.name)
        .key("done").boolean(p.done)
        .key("read").integer(p.read)
        .key("total").integer(p.total)
        .end_object();
  }
  w.end_array();
  w.key("rows").begin_array();
  for (const auto& [row, who] : s.rows) {
    w.begin_object()
        .key("path").string(row.path)
        .key("why").string(row.why)
        .key("addon").string(who.first)
        .key("provider").string(who.second)
        .end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

std::string slot_registry::badges_json() const {
  std::lock_guard lock(m_);
  json::writer w;
  w.begin_object();
  w.key("generation").integer(static_cast<std::int64_t>(badges_generation_));
  w.key("folder").string(badges_folder_);
  w.key("defs").begin_array();
  for (const auto& [id, e] : addons_) {
    for (const auto& b : e->c.badges) {
      w.begin_object()
          .key("key").string(id + "/" + b.id)
          .key("label").string(b.label)
          .key("symbol").string(b.symbol)
          .end_object();
    }
  }
  w.end_array();
  // Inverted once here, so a tile is one dictionary lookup in the chrome.
  std::map<std::string, std::vector<const std::string*>> tiles;
  for (const auto& [key, paths] : badge_sets_) {
    for (const std::string& p : paths) tiles[p].push_back(&key);
  }
  w.key("tiles").begin_object();
  for (const auto& [path, keys] : tiles) {
    w.key(path).begin_array();
    for (const std::string* k : keys) w.string(*k);
    w.end_array();
  }
  w.end_object();
  w.end_object();
  return w.take();
}

std::vector<std::string> slot_registry::badges_for(const std::string& path) const {
  std::lock_guard lock(m_);
  std::vector<std::string> out;
  for (const auto& [key, paths] : badge_sets_) {
    if (paths.count(path)) out.push_back(key);
  }
  return out;
}

std::string slot_registry::status_json() const {
  std::lock_guard lock(m_);
  json::writer w;
  w.begin_array();
  for (const auto& [key, text] : status_) {
    if (text.empty()) continue;
    auto it = addons_.find(key.first);
    if (it == addons_.end()) continue;
    for (const auto& i : it->second->c.items) {
      if (i.slot == item_slot::status && i.id == key.second) {
        w.begin_object()
            .key("addon").string(key.first)
            .key("id").string(key.second)
            .key("label").string(i.label)
            .key("text").string(text)
            .end_object();
      }
    }
  }
  w.end_array();
  return w.take();
}

// ---- answers --------------------------------------------------------------------

bool slot_registry::is_stale(std::uint64_t g) const {
  std::lock_guard lock(m_);
  if (g == 0) return true;
  if (g == info_generation_ || g == badges_generation_ || g == search_[0].generation ||
      g == search_[1].generation || private_.count(g) != 0) {
    return false;
  }
  // A section asked again after a settings change has a generation of its own.
  const auto has = [g](const auto& s) { return s.second.generation == g; };
  return std::none_of(screens_.begin(), screens_.end(), has) && std::none_of(info_.begin(), info_.end(), has);
}

bool slot_registry::is_stale(const std::string& addon_id, std::uint64_t g) const {
  {
    std::lock_guard lock(m_);
    // detach erases the entry under m_ before it joins the worker.
    if (addons_.count(addon_id) == 0) return true;
  }
  return is_stale(g);
}

slot_registry::screen_state* slot_registry::addressed(const std::string& addon_id,
                                                      const std::string& screen_id,
                                                      std::uint64_t generation, status& why) {
  if (auto s = screens_.find(std::make_pair(addon_id, screen_id)); s != screens_.end()) {
    if (s->second.generation != generation) {
      why = status::cancelled;
      return nullptr;
    }
    return &s->second;
  }
  if (auto i = info_.find(std::make_pair(addon_id, screen_id)); i != info_.end()) {
    if (i->second.generation != generation) {
      why = status::cancelled;
      return nullptr;
    }
    return &i->second;
  }
  // A section of the previous file, or a screen that has closed.
  why = addons_.count(addon_id) ? status::cancelled : status::not_found;
  return nullptr;
}

status slot_registry::post_screen(const std::string& addon_id, const std::string& screen_id,
                                  std::uint64_t generation, std::string_view tree_json) {
  std::uint32_t bits = 0;
  {
    std::lock_guard lock(m_);
    status why = status::ok;
    screen_state* st = addressed(addon_id, screen_id, generation, why);
    if (!st) return why;
    if (st->tree.set(tree_json) != screen::error::none) return status::invalid_arg;
    // Pixels no node shows any more go with the tree they belonged to.
    const auto ids = st->tree.surface_ids();
    for (auto it = st->surfaces.begin(); it != st->surfaces.end();) {
      it = std::find(ids.begin(), ids.end(), it->first) == ids.end() ? st->surfaces.erase(it) : std::next(it);
    }
    bits = st->place == screen_place::info_section ? slot_changed_info : slot_changed_screens;
  }
  notify(bits);
  return status::ok;
}

status slot_registry::post_diff(const std::string& addon_id, const std::string& screen_id,
                                std::uint64_t generation, std::string_view diff_json) {
  std::uint32_t bits = 0;
  {
    std::lock_guard lock(m_);
    status why = status::ok;
    screen_state* st = addressed(addon_id, screen_id, generation, why);
    if (!st) return why;
    if (st->tree.apply(diff_json) != screen::error::none) return status::invalid_arg;
    bits = st->place == screen_place::info_section ? slot_changed_info : slot_changed_screens;
  }
  notify(bits);
  return status::ok;
}

status slot_registry::post_surface(const std::string& addon_id, const std::string& screen_id,
                                   const std::string& surface_id, std::uint64_t generation,
                                   const std::uint8_t* bgra, std::uint32_t width,
                                   std::uint32_t height, std::uint32_t stride) {
  if (!bgra || width == 0 || height == 0 || width > 4096 || height > 4096 ||
      stride < width * 4u || surface_id.empty() || surface_id.size() > 64) {
    return status::invalid_arg;
  }
  // Copied outside the lock: up to 64 MB.
  surface_pixels px;
  px.width = width;
  px.height = height;
  px.bgra.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(px.bgra.data() + static_cast<std::size_t>(y) * width * 4,
                bgra + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(width) * 4);
  }
  std::uint32_t bits = 0;
  {
    std::lock_guard lock(m_);
    status why = status::ok;
    screen_state* st = addressed(addon_id, screen_id, generation, why);
    if (!st) return why;
    // Bounded by count as well as size: an id no node names is kept (pixels
    // may come before their tree), but not without limit (16 x 64 MB at most).
    if (st->surfaces.size() >= kMaxSurfacesPerScreen && st->surfaces.count(surface_id) == 0) {
      return status::invalid_arg;
    }
    auto& slot = st->surfaces[surface_id];
    // Unique across the registry, not per slot: a section for the next file
    // starts a new slot, and a chrome caching by revision must not take its
    // first pixels for the previous file's.
    px.revision = ++surface_revision_;
    slot = std::move(px);
    bits = st->place == screen_place::info_section ? slot_changed_info : slot_changed_screens;
  }
  notify(bits);
  return status::ok;
}

status slot_registry::post_search(const std::string& addon_id, const std::string& provider_id,
                                  std::uint64_t generation, std::vector<search_row> rows, bool done) {
  {
    std::lock_guard lock(m_);
    if (auto it = private_.find(generation); it != private_.end()) {
      private_search& ps = it->second;
      if (ps.addon != addon_id || ps.provider != provider_id) return status::not_found;
      for (search_row& r : rows) {
        if (r.path.empty() || r.path.size() > 4096 || !json::detail::valid_utf8(r.path)) continue;
        ps.paths.push_back(std::move(r.path));
      }
      if (done) ps.done = true;
      private_cv_.notify_all();
      return status::ok;
    }
    search_state* s = nullptr;
    for (auto& candidate : search_) {
      if (candidate.generation == generation) s = &candidate;
    }
    if (!s || s->query.empty()) return status::cancelled;
    auto p = std::find_if(s->providers.begin(), s->providers.end(), [&](const provider_state& ps) {
      return ps.addon == addon_id && ps.id == provider_id;
    });
    if (p == s->providers.end()) return status::not_found;
    if (s->rows.size() + rows.size() > screen::kMaxItems) return status::invalid_arg;
    for (search_row& r : rows) {
      if (r.path.empty() || r.path.size() > 4096 || !json::detail::valid_utf8(r.path) || r.why.size() > 200 ||
          !json::detail::valid_utf8(r.why)) {
        continue;
      }
      s->rows.emplace_back(std::move(r), std::make_pair(addon_id, provider_id));
    }
    if (done) p->done = true;
  }
  notify(slot_changed_search);
  return status::ok;
}

void slot_registry::search_progress(const std::string& addon_id, const std::string& provider_id,
                                    std::uint64_t generation, std::uint32_t done, std::uint32_t total) {
  {
    std::lock_guard lock(m_);
    for (auto& s : search_) {
      if (s.generation != generation) continue;
      for (auto& p : s.providers) {
        if (p.addon == addon_id && p.id == provider_id) {
          p.read = std::min(done, total);
          p.total = total;
        }
      }
    }
  }
  notify(slot_changed_search);
}

status slot_registry::post_badges(const std::string& addon_id, const std::string& badge_id,
                                  std::uint64_t generation, std::vector<std::string> paths,
                                  bool replace) {
  {
    std::lock_guard lock(m_);
    auto it = addons_.find(addon_id);
    if (it == addons_.end()) return status::not_found;
    if (generation != badges_generation_) return status::cancelled;
    const auto& defs = it->second->c.badges;
    if (std::none_of(defs.begin(), defs.end(), [&](const badge_contrib& b) { return b.id == badge_id; })) {
      return status::not_found;
    }
    auto& set = badge_sets_[addon_id + "/" + badge_id];
    if (replace) set.clear();
    for (std::string& p : paths) {
      if (!p.empty() && p.size() <= 4096 && set.size() < 1'000'000) set.insert(std::move(p));
    }
  }
  notify(slot_changed_badges);
  return status::ok;
}

status slot_registry::post_status(const std::string& addon_id, const std::string& item_id,
                                  std::string text) {
  if (text.size() > 120 || !json::detail::valid_utf8(text)) return status::invalid_arg;
  {
    std::lock_guard lock(m_);
    auto it = addons_.find(addon_id);
    if (it == addons_.end()) return status::not_found;
    const auto& items = it->second->c.items;
    if (std::none_of(items.begin(), items.end(), [&](const item_contrib& i) {
          return i.slot == item_slot::status && i.id == item_id;
        })) {
      return status::not_found;
    }
    status_[std::make_pair(addon_id, item_id)] = std::move(text);
  }
  notify(slot_changed_status);
  return status::ok;
}

status slot_registry::show_screen(const std::string& addon_id, const std::string& screen_id, bool show) {
  if (!show) return close_screen(addon_id, screen_id);
  status st = status::not_found;
  {
    std::lock_guard lock(m_);
    auto it = addons_.find(addon_id);
    if (it == addons_.end()) return status::not_found;
    for (const screen_contrib& s : it->second->c.screens) {
      if (s.id == screen_id) {
        st = open_screen_locked(*it->second, s, info_path_, last_theme_, 0, 0, last_scale_);
      }
    }
  }
  if (st == status::ok) notify(slot_changed_screens);
  return st;
}

slot_registry& default_slots() {
  static slot_registry r;
  return r;
}

namespace {
class local_search_client final : public slot_client {
 public:
  explicit local_search_client(slot_registry& r) : r_(r) {}
  status info_section(const mv_slot_info_request&) override { return status::unsupported_format; }
  status search(const mv_slot_search_request& q) override {
    return r_.post_search("ai", q.provider_id, q.generation, {}, true);
  }
  status screen_open(const mv_slot_screen_request&) override { return status::unsupported_format; }
  status badges(const mv_slot_badges_request&) override { return status::unsupported_format; }
  void event(const mv_slot_event&) override {}

 private:
  slot_registry& r_;
};
}  // namespace

std::uint32_t place_terms(const slot_registry& registry, std::string_view query) {
  std::uint32_t out = 0;
  if (has_place_terms(query)) out |= MV_SLOTS_PLACE_TERMS;
  if (registry.answers_prefix(MV_SLOT_SCOPE_LOCAL, "place:")) out |= MV_SLOTS_PLACE_PROVIDER;
  return out;
}

void set_local_search_provider(slot_registry& registry, bool on) {
  if (!on) {
    registry.detach("ai");
    return;
  }
  if (registry.attached("ai")) return;
  contributions c;
  c.api_min = kSlotsApi;
  c.api_max = kSlotsApi;
  c.search_providers.push_back({"local", "Local search", {}, false, true});
  (void)registry.attach("ai", "Local search", std::move(c), std::make_unique<local_search_client>(registry));
}

// ---- the location cache -------------------------------------------------------

namespace {
struct location_key_hash {
  std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string>{}(s); }
};
struct cached {
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  mv_addon_location loc{};
};
std::mutex& cache_m() {
  static std::mutex m;
  return m;
}
std::unordered_map<std::string, cached, location_key_hash>& cache() {
  static std::unordered_map<std::string, cached, location_key_hash> c;
  return c;
}
}  // namespace

mv_addon_location cached_location(const std::string& path, std::uint64_t size, std::int64_t mtime,
                                  const location_reader& read) {
  {
    std::lock_guard lock(cache_m());
    auto it = cache().find(path);
    if (it != cache().end() && it->second.size == size && it->second.mtime == mtime) return it->second.loc;
  }
  mv_addon_location loc{};
  if (read) {
    if (auto got = read(path)) loc = *got;
  }
  std::lock_guard lock(cache_m());
  // Bounded: a 200k-file walk keeps ~40 bytes plus the path per file.
  if (cache().size() > 500'000) cache().clear();
  cache()[path] = cached{size, mtime, loc};
  return loc;
}

bool peek_cached_location(const std::string& path, std::uint64_t size, std::int64_t mtime,
                          mv_addon_location& out) {
  std::lock_guard lock(cache_m());
  auto it = cache().find(path);
  if (it == cache().end() || it->second.size != size || it->second.mtime != mtime) return false;
  out = it->second.loc;
  return true;
}

void clear_location_cache() {
  std::lock_guard lock(cache_m());
  cache().clear();
}

}  // namespace mv::addon
