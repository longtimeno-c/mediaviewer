// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/slots.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>
#include <utility>

#include "core/json.h"

namespace mv::addon {
namespace {


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

slot_registry::slot_registry() = default;

slot_registry::~slot_registry() {
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
    last_theme_ = theme;
    last_scale_ = scale;
    for (auto& [id, e] : addons_) {
      for (const info_section_contrib& s : e->c.info_sections) {
        if (s.needs_location && !loc) continue;  // never asked for a file without one
        screen_state st;
        st.addon = id;
        st.id = s.id;
        st.place = screen_place::info_section;
        st.generation = g;
        info_.emplace(std::make_pair(id, s.id), std::move(st));
        const std::string section = s.id;
        enqueue(*e, "info:" + section,
                [g, section, path, location_text, loc, theme, width_pt, scale](slot_client& c) {
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
  }
  if (had) notify(slot_changed_info);
  return g;
}

void slot_registry::clear_info() {
  bool had = false;
  {
    std::lock_guard lock(m_);
    had = !info_.empty();
    info_.clear();
    info_generation_ = 0;
    info_path_.clear();
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
    if (!trimmed.empty()) {
      const std::string folded = fold_query(trimmed);
      // A query that starts with a provider's prefix ("place:") goes to the
      // providers that declare it, prefix removed; any other query to every
      // provider of the scope.
      std::string routed_prefix;
      for (auto& [id, e] : addons_) {
        for (const search_provider_contrib& p : e->c.search_providers) {
          for (const std::string& pre : p.prefixes) {
            if (folded.rfind(pre, 0) == 0 && pre.size() > routed_prefix.size()) routed_prefix = pre;
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
            // The prefix is ASCII, so its length is the same folded or not.
            q = q.substr(std::min(q.size(), routed_prefix.size()));
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
    w.begin_object()
        .key("addon").string(st.addon)
        .key("id").string(st.id)
        .key("title").string(def->title)
        .key("place").string(place_name(def->place))
        .key("width").integer(def->width)
        .key("height").integer(def->height)
        .key("generation").integer(static_cast<std::int64_t>(st.generation))
        .key("revision").integer(static_cast<std::int64_t>(st.tree.revision()))
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
      g == search_[1].generation) {
    return false;
  }
  return std::none_of(screens_.begin(), screens_.end(),
                      [g](const auto& s) { return s.second.generation == g; });
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
    auto& slot = st->surfaces[surface_id];
    px.revision = slot.revision + 1;
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

void clear_location_cache() {
  std::lock_guard lock(cache_m());
  cache().clear();
}

}  // namespace mv::addon
