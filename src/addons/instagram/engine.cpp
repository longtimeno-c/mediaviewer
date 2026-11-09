// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/instagram/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

#include "core/json.h"

namespace mv::instagram {
namespace {

constexpr std::uint32_t kPreviewEdge = 1200;

bool is_key(std::string_view path) noexcept { return path.rfind("photos:", 0) == 0; }

std::string name_of(const std::string& path) {
  const std::size_t sep = path.find_last_of("/\\");
  return sep == std::string::npos ? path : path.substr(sep + 1);
}

std::string folder_of(const std::string& path) {
  if (path.empty() || is_key(path)) return {};
  const std::size_t sep = path.find_last_of("/\\");
  return sep == std::string::npos ? std::string() : path.substr(0, sep);
}

std::string two_digits(std::size_t n) {
  return (n < 10 ? "0" : "") + std::to_string(n);
}

std::string json_string(const std::string& value_json) {
  const auto doc = json::parse(value_json, 2);
  return doc && doc->k == json::kind::string ? doc->s : std::string();
}

}  // namespace

// ---- lane ------------------------------------------------------------------

engine::lane::lane() : t_([this] { run(); }) {}
engine::lane::~lane() { stop(); }

void engine::lane::push(std::string key, std::function<void()> job) {
  std::lock_guard lock(m_);
  if (quit_) return;
  if (!key.empty()) {
    for (auto& queued : jobs_) {
      if (queued.first == key) {
        queued.second = std::move(job);
        return;
      }
    }
  }
  jobs_.emplace_back(std::move(key), std::move(job));
  cv_.notify_all();
}

void engine::lane::stop() {
  {
    std::lock_guard lock(m_);
    quit_ = true;
    jobs_.clear();
    cv_.notify_all();
  }
  if (t_.joinable()) t_.join();
}

void engine::lane::wait_idle() {
  std::unique_lock lock(m_);
  cv_.wait(lock, [this] { return quit_ || (jobs_.empty() && !busy_); });
}

void engine::lane::run() {
  std::unique_lock lock(m_);
  for (;;) {
    cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
    if (quit_) return;
    auto job = std::move(jobs_.front().second);
    jobs_.pop_front();
    busy_ = true;
    lock.unlock();
    try {
      job();
    } catch (...) {
      // A job that threw (out of memory on a picture) is one tile left empty.
    }
    lock.lock();
    busy_ = false;
    cv_.notify_all();
  }
}

// ---- engine ----------------------------------------------------------------

engine::engine(engine_port port) : port_(std::move(port)) {}

engine::~engine() {
  cancel_.store(true, std::memory_order_relaxed);
  if (export_.joinable()) export_.join();
  finder_.stop();
  work_.stop();
}

void engine::wait_idle() {
  std::thread finishing;
  {
    std::lock_guard lock(m_);
    if (!exporting_ && export_.joinable()) finishing = std::move(export_);
  }
  if (finishing.joinable()) finishing.join();
  for (;;) {
    {
      std::lock_guard lock(m_);
      if (!exporting_) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  finder_.wait_idle();
  work_.wait_idle();
}

std::vector<std::string> engine::written() const {
  std::lock_guard lock(m_);
  return written_;
}

const std::vector<gallery_row>& engine::active_locked() const {
  return from_ == source::library ? library_rows_ : folder_rows_;
}

view_state engine::view_locked() {
  view_state v;
  v.p = &post_;
  v.from = from_;
  v.has_folder = !folder_.empty();
  v.has_library = has_library_;
  v.query = query_;
  v.rows = &shown_rows_;
  v.shown = std::min(page_, shown_rows_.size());
  v.loading = loading_;
  v.exporting = exporting_;
  v.progress = progress_;
  v.progress_words = progress_words_;
  v.note = note_;
  v.can_show = can_show_;
  v.mode = mode_;
  v.enhance_missing = enhance_missing_;
  v.unreadable = unreadable_;
  return v;
}

void engine::publish_strip_locked() {
  if (open_ && port_.diff) (void)port_.diff(gen_, strip_diff(view_locked()));
}

void engine::publish_gallery_locked() {
  if (open_ && port_.diff) (void)port_.diff(gen_, gallery_diff(view_locked()));
}

void engine::publish_progress_locked() {
  if (open_ && port_.diff) (void)port_.diff(gen_, progress_diff(view_locked()));
}

void engine::refilter_locked() {
  shown_rows_.clear();
  const std::vector<gallery_row>& rows = active_locked();
  std::set<const gallery_row*> taken;
  if (!query_.empty() && !hits_.empty()) {
    // Local search's order first: what the picture shows beats a name's digits.
    std::map<std::string_view, const gallery_row*> by_path;
    for (const gallery_row& r : rows) by_path.emplace(r.path, &r);
    for (const std::string& hit : hits_) {
      const auto it = by_path.find(hit);
      if (it != by_path.end() && taken.insert(it->second).second) shown_rows_.push_back(it->second);
    }
  }
  for (const gallery_row& r : rows) {
    if (name_matches(r.name, query_) && !taken.count(&r)) shown_rows_.push_back(&r);
  }
}

void engine::opened(const screen_facts& facts) {
  std::lock_guard lock(m_);
  open_ = true;
  gen_ = facts.generation;
  accent_ = facts.accent;
  scale_ = facts.scale > 0 ? facts.scale : 2;
  const std::string& where = facts.path.empty() ? hint_ : facts.path;
  const std::string folder = folder_of(where);
  folder_ = folder;
  folder_rows_.clear();
  library_rows_.clear();
  shown_rows_.clear();
  query_.clear();
  hits_.clear();
  page_ = kGalleryPage;
  has_library_ = is_key(where);  // a library item on screen proves one until the list says
  from_ = (is_key(where) || folder.empty()) ? source::library : source::folder;
  loading_ = true;
  // The window first: the strip and an empty grid, then what fills them.
  if (port_.post) (void)port_.post(gen_, tree_json(view_locked()));
  const bool wants_library = static_cast<bool>(port_.library);
  work_.push("", [this, gen = gen_, folder, wants_library] { load_lists(gen, folder, wants_library); });
  for (const chosen& c : post_.items()) {
    if (pictures_.count(c.uid)) {
      request_render_locked(c.uid);
    } else {
      request_preview_locked(c);
    }
  }
}

void engine::closed() {
  std::lock_guard lock(m_);
  open_ = false;
  cancel_.store(true, std::memory_order_relaxed);
  mode_ = tool_mode::choose;
  sync_enhance_locked();
  // The post itself stays: closing by accident loses nothing. The lists go;
  // they are read again at the next open.
  shown_rows_.clear();
  folder_rows_.clear();
  library_rows_.clear();
}

void engine::load_lists(std::uint64_t gen, std::string folder, bool wants_library) {
  std::vector<gallery_row> files;
  if (!folder.empty() && port_.walk) {
    (void)port_.walk(folder, [&](gallery_row r) {
      if (listable(r.name)) {
        r.video = looks_like_video(r.name);
        files.push_back(std::move(r));
      }
    });
    std::sort(files.begin(), files.end(), [](const gallery_row& a, const gallery_row& b) { return a.when > b.when; });
  }
  {
    std::lock_guard lock(m_);
    if (!open_ || gen != gen_) return;
    folder_rows_ = std::move(files);
    if (from_ == source::folder) {
      loading_ = wants_library;
      refilter_locked();
      add_pending_locked();
      publish_gallery_locked();
    }
  }
  if (!wants_library && port_.library) return;  // a refresh: the library is as it was
  std::vector<gallery_row> items;
  bool library = false;
  if (wants_library) library = port_.library([&](gallery_row r) { items.push_back(std::move(r)); });
  std::lock_guard lock(m_);
  if (!open_ || gen != gen_) return;
  has_library_ = library;
  library_rows_ = std::move(items);
  loading_ = false;
  // No library after all, and the window had settled on it: the folder.
  if (!library && from_ == source::library) from_ = source::folder;
  refilter_locked();
  add_pending_locked();
  publish_gallery_locked();
}

void engine::add_pending_locked() {
  if (pending_.empty()) return;
  bool any = false;
  for (const std::string& path : pending_) {
    const gallery_row* row = nullptr;
    for (const std::vector<gallery_row>* rows : {&folder_rows_, &library_rows_}) {
      for (const gallery_row& r : *rows) {
        if (r.path == path) row = &r;
      }
    }
    if (is_key(path) && !row) continue;  // the library is still coming
    const std::string name = row ? row->name : name_of(path);
    const bool video = row ? row->video : looks_like_video(name);
    if (post_.add(path, name, video)) {
      any = true;
      request_preview_locked(post_.items().back());
    }
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const std::string& p) { return post_.has(p); }),
                 pending_.end());
  if (any) publish_strip_locked();
}

void engine::add_and_show(const std::vector<std::string>& paths) {
  {
    std::lock_guard lock(m_);
    pending_.insert(pending_.end(), paths.begin(), paths.end());
    if (open_) add_pending_locked();
  }
  if (port_.show) port_.show();
}

void engine::open_command(const std::vector<std::string>& paths) {
  if (paths.size() != 1) {
    add_and_show(paths);
    return;
  }
  {
    std::lock_guard lock(m_);
    hint_ = paths.front();
  }
  if (port_.show) port_.show();
}

void engine::request_preview_locked(const chosen& c) {
  work_.push("p" + std::to_string(c.uid), [this, uid = c.uid, path = c.path] {
    auto pic = std::make_shared<image>();
    const bool ok = port_.preview && port_.preview(path, kPreviewEdge, *pic) && pic->w > 0 && pic->h > 0;
    std::lock_guard lock(m_);
    if (!post_.find(uid)) return;
    if (!ok) {
      if (std::find(unreadable_.begin(), unreadable_.end(), uid) == unreadable_.end()) unreadable_.push_back(uid);
      publish_strip_locked();
      return;
    }
    pictures_[uid] = {std::move(pic)};
    request_render_locked(uid);
  });
}

void engine::request_render_locked(std::uint64_t uid) {
  work_.push("r" + std::to_string(uid), [this, uid] { render(uid); });
}

void engine::render(std::uint64_t uid) {
  std::shared_ptr<const image> pic;
  frame f;
  adjustments look{};
  shape s;
  bool focused = false;
  bool tools = false;
  std::uint32_t accent = 0;
  float scale = 2;
  std::uint64_t gen = 0;
  {
    std::lock_guard lock(m_);
    const chosen* c = post_.find(uid);
    const auto it = pictures_.find(uid);
    if (!open_ || !c || it == pictures_.end()) return;
    pic = it->second.pic;
    f = c->f;
    look = c->adjust;
    s = post_.current();
    focused = post_.focus() == uid;
    tools = mode_ != tool_mode::choose;
    accent = accent_;
    scale = scale_;
    gen = gen_;
  }
  const rect crop = crop_of(pic->w, pic->h, aspect_of(s), f);
  const auto draw = [&](tile_size size, const std::string& id, bool ring) {
    const auto w = static_cast<std::uint32_t>(std::lround(size.w * scale));
    const auto h = static_cast<std::uint32_t>(std::lround(size.h * scale));
    std::vector<std::uint8_t> rgb = crop_rgb(*pic, crop, w, h);
    if (adjusted(look) && port_.adjust) (void)port_.adjust(rgb.data(), w, h, look);
    const auto ring_px = static_cast<std::uint32_t>(std::lround(2 * scale));
    if (port_.surface) port_.surface(gen, id, bgra_of(rgb, w, h, accent, ring ? ring_px : 0), w, h);
  };
  draw(tile_of(s), surface_id(uid), focused);
  if (focused && tools) draw(stage_of(s), std::string(kStageSurface), false);
}

void engine::select(const std::string& value_json) {
  const auto doc = json::parse(value_json, 3);
  if (!doc || doc->k != json::kind::array) return;
  std::lock_guard lock(m_);
  if (exporting_) {
    publish_strip_locked();  // the selection on screen goes back to the post
    return;
  }
  std::map<std::string_view, const gallery_row*> by_path;
  for (const std::vector<gallery_row>* rows : {&folder_rows_, &library_rows_}) {
    for (const gallery_row& r : *rows) by_path.emplace(r.path, &r);
  }
  std::vector<post::described> wanted;
  wanted.reserve(doc->a.size());
  for (const json::value& v : doc->a) {
    if (v.k != json::kind::string) continue;
    post::described d;
    d.path = v.s;
    const auto it = by_path.find(d.path);
    if (it != by_path.end()) {
      d.name = it->second->name;
      d.video = it->second->video;
    } else {
      d.name = name_of(d.path);
      d.video = looks_like_video(d.name);
    }
    wanted.push_back(std::move(d));
  }
  // The newest choice is what the person is looking at. The host sends the
  // selection sorted, so without this a pick lands wherever its path sorts.
  const std::uint64_t before = post_.focus();
  const auto change = post_.apply_selection(wanted);
  if (change.refused > 0) {
    note_ = "A post holds " + std::to_string(kMaxChosen);
    can_show_ = false;
  } else if (change.added > 0) {
    note_.clear();
  }
  if (change.last_added != 0) post_.set_focus(change.last_added);
  if (post_.focus() != before) focus_changed_locked();
  for (const chosen& c : post_.items()) {
    if (!pictures_.count(c.uid)) {
      request_preview_locked(c);
    } else if (c.uid == before || c.uid == post_.focus()) {
      request_render_locked(c.uid);  // its ring came or went
    }
  }
  for (auto it = pictures_.begin(); it != pictures_.end();) {
    it = post_.find(it->first) ? std::next(it) : pictures_.erase(it);
  }
  publish_strip_locked();
  publish_progress_locked();
}

void engine::click(const std::string& node) {
  std::lock_guard lock(m_);
  if (node == "export") {
    if (exporting_) {
      cancel_.store(true, std::memory_order_relaxed);
      return;
    }
    if (post_.items().empty()) return;
    if (export_.joinable()) export_.join();
    cancel_.store(false, std::memory_order_relaxed);
    exporting_ = true;
    progress_ = 0;
    progress_words_ = "0 of " + std::to_string(post_.items().size());
    note_.clear();
    can_show_ = false;
    written_.clear();
    publish_progress_locked();
    publish_strip_locked();
    if (port_.busy) port_.busy("an Instagram post export");
    export_ = std::thread([this, items = post_.items(), s = post_.current()] { export_all(items, s); });
    return;
  }
  if (node == "show") {
    if (port_.open_list && !written_.empty()) port_.open_list(gen_, written_);
    return;
  }
  if (node == "more") {
    page_ += kGalleryPage;
    publish_gallery_locked();
    return;
  }
  if (exporting_) return;
  const std::uint64_t focus = post_.focus();
  if (node == "reset") {
    if (chosen* c = post_.find(focus)) {
      c->adjust = adjustments{};
      publish_panel_locked();
      request_render_locked(focus);
    }
    return;
  }
  if (node == "auto") {
    const auto it = pictures_.find(focus);
    if (it == pictures_.end() || !port_.auto_adjust) return;
    work_.push("a" + std::to_string(focus), [this, focus, pic = it->second.pic] {
      adjustments values{};
      if (!port_.auto_adjust(pic->rgb.data(), pic->w, pic->h, values)) return;
      std::lock_guard lock(m_);
      chosen* c = post_.find(focus);
      if (!c) return;
      c->adjust = values;
      publish_panel_locked();
      request_render_locked(focus);
    });
    return;
  }
  bool changed = false;
  if (node == "earlier") changed = post_.move(focus, -1);
  if (node == "later") changed = post_.move(focus, 1);
  if (node == "remove") {
    changed = post_.remove(focus);
    pictures_.erase(focus);
    if (changed && post_.focus() != 0) request_render_locked(post_.focus());
    if (changed) focus_changed_locked();
  }
  if (changed) publish_strip_locked();
}

void engine::publish_panel_locked() {
  if (open_ && port_.diff) (void)port_.diff(gen_, panel_diff(view_locked()));
}

void engine::sync_enhance_locked() {
  const chosen* c = mode_ == tool_mode::enhance ? post_.find(post_.focus()) : nullptr;
  if (!c || c->video) {
    if (!embed_path_.empty() && port_.embed_close) port_.embed_close();
    embed_path_.clear();
    return;
  }
  if (c->path == embed_path_) return;
  enhance_missing_ = !port_.embed_open || !port_.embed_open(gen_, c->path);
  embed_path_ = enhance_missing_ ? std::string() : c->path;
}

void engine::focus_changed_locked() {
  if (mode_ == tool_mode::choose) return;
  sync_enhance_locked();
  publish_panel_locked();
  if (post_.focus() != 0) request_render_locked(post_.focus());
}

void engine::change(const std::string& node, const std::string& value_json) {
  if (node.rfind("a.", 0) == 0) {
    const auto doc = json::parse(value_json, 2);
    if (!doc || doc->k != json::kind::number) return;
    const double value = doc->is_integer ? static_cast<double>(doc->i) : doc->d;
    std::uint32_t wire = kAdjustValues;
    float scale = 1.0f;
    for (const slider_info& s : sliders()) {
      if (node == "a." + std::to_string(s.wire)) {
        wire = s.wire;
        scale = s.scale;
      }
    }
    std::lock_guard lock(m_);
    chosen* c = post_.find(post_.focus());
    if (!c || wire >= kAdjustValues || exporting_) return;
    const bool was = adjusted(c->adjust);
    c->adjust[wire] = static_cast<float>(value) * scale;
    if (adjusted(c->adjust) != was && open_ && port_.diff) (void)port_.diff(gen_, reset_diff(was));
    request_render_locked(c->uid);
    return;
  }
  const std::string value = json_string(value_json);
  std::lock_guard lock(m_);
  if (node == "mode") {
    const tool_mode to = value == "adjust" ? tool_mode::adjust : value == "enhance" ? tool_mode::enhance : tool_mode::choose;
    if (to == mode_) return;
    mode_ = to;
    // The tools need a photo: the first one, when none has focus yet.
    if (mode_ != tool_mode::choose && post_.focus() == 0 && !post_.items().empty()) {
      post_.set_focus(post_.items().front().uid);
      publish_strip_locked();
    }
    sync_enhance_locked();
    publish_panel_locked();
    if (post_.focus() != 0) request_render_locked(post_.focus());
    return;
  }
  if (node == "shape") {
    const auto s = shape_from(value);
    if (!s || *s == post_.current()) return;
    post_.set_shape(*s);
    publish_strip_locked();  // the strip eases to the new shape while its pixels are made
    for (const chosen& c : post_.items()) request_render_locked(c.uid);
    return;
  }
  if (node == "source") {
    const source to = value == "library" ? source::library : source::folder;
    if (to == from_) return;
    from_ = to;
    hits_.clear();
    page_ = kGalleryPage;
    ++search_ticket_;
    refilter_locked();
    publish_gallery_locked();
    if (!query_.empty()) {
      finder_.push("search", [this, t = search_ticket_, q = query_, scope = from_ == source::library ? std::string("photos:") : folder_] {
        search_later(t, q, scope);
      });
    }
    return;
  }
  if (node == "search") {
    if (value == query_) return;
    query_ = value;
    hits_.clear();
    page_ = kGalleryPage;
    const std::uint64_t ticket = ++search_ticket_;
    refilter_locked();  // names answer as the person types
    publish_gallery_locked();
    if (!value.empty() && port_.search) {
      finder_.push("search", [this, ticket, value, scope = from_ == source::library ? std::string("photos:") : folder_] {
        search_later(ticket, value, scope);
      });
    }
  }
}

void engine::search_later(std::uint64_t ticket, std::string query, std::string scope) {
  // Typing pauses before the models are asked.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  {
    std::lock_guard lock(m_);
    if (ticket != search_ticket_) return;
  }
  std::vector<std::string> hits;
  if (!port_.search || !port_.search(query, scope, hits)) return;
  std::lock_guard lock(m_);
  if (ticket != search_ticket_ || !open_) return;
  hits_ = std::move(hits);
  refilter_locked();
  publish_gallery_locked();
}

void engine::item_saved(const std::string& source, const std::string& result) {
  std::lock_guard lock(m_);
  std::string reload;
  for (const chosen& c : post_.items()) {
    if (c.path != source) continue;
    const std::uint64_t uid = c.uid;
    // A new library photo keeps the name it was shot under.
    const std::string name = is_key(result) ? c.name : name_of(result);
    if (!post_.replace(uid, result, name)) return;
    pictures_.erase(uid);
    unreadable_.erase(std::remove(unreadable_.begin(), unreadable_.end(), uid), unreadable_.end());
    if (is_key(result)) {
      gallery_row r;
      r.path = result;
      r.name = name;
      library_rows_.insert(library_rows_.begin(), std::move(r));
      refilter_locked();
    } else {
      reload = folder_of(result) == folder_ ? folder_ : std::string();
    }
    note_ = "Now using the enhanced copy";
    can_show_ = false;
    request_preview_locked(*post_.find(uid));
    publish_strip_locked();
    publish_gallery_locked();
    publish_progress_locked();
    if (post_.focus() == uid) focus_changed_locked();
    break;
  }
  // Only the folder: the new file should be in the gallery too.
  if (!reload.empty()) {
    work_.push("", [this, gen = gen_, reload] { load_lists(gen, reload, false); });
  }
}

void engine::tap(const std::string& node) {
  const std::uint64_t uid = uid_of_tile_node(node);
  std::lock_guard lock(m_);
  if (uid == 0 || !post_.find(uid) || post_.focus() == uid) return;
  const std::uint64_t before = post_.focus();
  post_.set_focus(uid);
  if (before != 0) request_render_locked(before);
  request_render_locked(uid);
  publish_strip_locked();
  focus_changed_locked();
}

void engine::pan(const std::string& node, double dx, double dy) {
  std::lock_guard lock(m_);
  const std::uint64_t uid = node == kStageNode ? post_.focus() : uid_of_tile_node(node);
  chosen* c = post_.find(uid);
  const auto it = pictures_.find(uid);
  if (!c || it == pictures_.end() || exporting_) return;
  c->f = dragged(c->f, it->second.pic->w, it->second.pic->h, aspect_of(post_.current()), dx, dy);
  request_render_locked(uid);
}

void engine::zoom(const std::string& node, double scale) {
  std::lock_guard lock(m_);
  const std::uint64_t uid = node == kStageNode ? post_.focus() : uid_of_tile_node(node);
  chosen* c = post_.find(uid);
  if (!c || exporting_) return;
  c->f = zoomed(c->f, scale);
  request_render_locked(uid);
}

void engine::reset_frame(const std::string& node) {
  std::lock_guard lock(m_);
  const std::uint64_t uid = node == kStageNode ? post_.focus() : uid_of_tile_node(node);
  chosen* c = post_.find(uid);
  if (!c || exporting_) return;
  c->f = frame{};
  request_render_locked(uid);
}

// ---- export ----------------------------------------------------------------

void engine::note_progress(std::size_t index, std::size_t total, double within) {
  std::lock_guard lock(m_);
  progress_ = (static_cast<double>(index) + std::clamp(within, 0.0, 1.0)) / static_cast<double>(total);
  progress_words_ = std::to_string(index + 1) + " of " + std::to_string(total);
  publish_progress_locked();
}

bool engine::export_clip(const chosen& c, shape s, const std::string& dir, const std::string& stem, std::size_t index,
                         std::size_t total, std::string& out) {
  if (!port_.open_clip || !port_.write_clip) return false;
  const std::string file = port_.library_file ? port_.library_file(c.path) : c.path;
  if (file.empty()) return false;
  std::unique_ptr<clip_reader> reader = port_.open_clip(file);
  if (!reader) return false;
  const shape_info& info = info_of(s);
  // Even sizes for the encoder; every shape's already are.
  std::unique_ptr<clip_writer> writer = port_.write_clip(file, info.out_w, info.out_h, dir, stem);
  if (!writer) return false;
  const std::uint32_t rot = reader->rotation();
  const bool swaps = rot == 90 || rot == 270;
  const std::uint32_t dw = swaps ? reader->height() : reader->width();
  const std::uint32_t dh = swaps ? reader->width() : reader->height();
  scaler scale(dw, dh, crop_of(dw, dh, aspect_of(s), c.f), info.out_w, info.out_h);
  std::vector<std::uint8_t> rgb;
  std::vector<std::uint8_t> output(static_cast<std::size_t>(info.out_w) * info.out_h * 3);
  image upright;
  std::int64_t pts = 0;
  std::int64_t done = 0;
  const std::int64_t estimate = std::max<std::int64_t>(1, reader->frame_count());
  auto last = std::chrono::steady_clock::now();
  bool failed = false;
  while (reader->next(rgb, pts, failed)) {
    if (cancel_.load(std::memory_order_relaxed)) return false;
    const std::uint8_t* picture = rgb.data();
    if (rot != 0) {
      turned(rgb.data(), reader->width(), reader->height(), rot, upright);
      picture = upright.rgb.data();
    }
    scale.run(picture, output.data());
    if (!writer->frame(output.data(), info.out_w * 3, pts)) return false;
    ++done;
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(200)) {
      last = now;
      note_progress(index, total, static_cast<double>(done) / static_cast<double>(estimate));
    }
  }
  if (failed || done == 0) return false;
  out = writer->finish();
  return !out.empty();
}

void engine::export_all(std::vector<chosen> items, shape s) {
  const std::string stamp = port_.stamp ? port_.stamp() : std::string("post");
  const std::string dir = port_.library_dir + "/Instagram/" + stamp;
  std::vector<std::string> made;
  std::size_t failed = 0;
  bool have_dir = !port_.library_dir.empty() && port_.make_dirs && port_.make_dirs(dir);
  const shape_info& info = info_of(s);
  for (std::size_t i = 0; have_dir && i < items.size(); ++i) {
    if (cancel_.load(std::memory_order_relaxed)) break;
    note_progress(i, items.size(), 0);
    const chosen& c = items[i];
    const std::string stem = two_digits(i + 1);
    std::string out;
    if (c.video) {
      if (!export_clip(c, s, dir, stem, i, items.size(), out)) ++failed;
    } else {
      image pic;
      if (port_.still && port_.still(c.path, pic) && pic.w > 0 && pic.h > 0) {
        image cut;
        cut.w = info.out_w;
        cut.h = info.out_h;
        cut.rgb.resize(static_cast<std::size_t>(cut.w) * cut.h * 3);
        scaler(pic.w, pic.h, crop_of(pic.w, pic.h, aspect_of(s), c.f), cut.w, cut.h).run(pic.rgb.data(), cut.rgb.data());
        if (adjusted(c.adjust) && port_.adjust) (void)port_.adjust(cut.rgb.data(), cut.w, cut.h, c.adjust);
        if (port_.write_still) out = port_.write_still(cut, dir, stem);
      }
      if (out.empty()) ++failed;
    }
    if (!out.empty()) made.push_back(std::move(out));
  }
  const bool stopped = cancel_.load(std::memory_order_relaxed);
  std::lock_guard lock(m_);
  exporting_ = false;
  progress_ = 0;
  progress_words_.clear();
  written_ = std::move(made);
  can_show_ = !written_.empty();
  if (!have_dir) {
    note_ = "Could not make the export folder";
  } else if (stopped) {
    note_ = "Export cancelled";
  } else if (failed > 0) {
    note_ = "Saved " + std::to_string(written_.size()) + ", " + std::to_string(failed) + " could not be made";
  } else {
    note_ = "Saved " + std::to_string(written_.size()) + " to Pictures/MediaViewer/Instagram";
  }
  if (port_.busy) port_.busy("");
  publish_progress_locked();
  publish_strip_locked();
}

}  // namespace mv::instagram
