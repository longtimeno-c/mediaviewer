// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/engine.h"
#include "addons/enhance/face_restore.h"

#include "addons/enhance/memory.h"

#include <mediaviewer/mediaviewer_addon.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mv::enhance {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::optional<std::string> read_text(const std::string& path, std::size_t cap) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return std::nullopt;
  std::string text(cap + 1, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  const auto got = static_cast<std::size_t>(in.gcount());
  if (got > cap) return std::nullopt;  // not a file this add-on wrote
  text.resize(got);
  return text;
}

std::vector<std::uint8_t> head_of(const std::string& path) {
  std::vector<std::uint8_t> head(256);
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return {};
  in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<std::size_t>(in.gcount()));
  return head;
}

bool library_key(const std::string& path) { return path.rfind("photos:", 0) == 0; }

// A Photos-library item ("photos:<id>", docs/design/26) has no file name to
// show, and its key is never shown (rule 6).
std::string name_of(const std::string& path) {
  if (library_key(path)) return "Photo in your Photos library";
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* model_noun(model_kind k) {
  switch (k) {
    case model_kind::still: return "photo model";
    case model_kind::video: return "video model";
    case model_kind::audio: return "audio model";
    case model_kind::restyle: return "restyle model";
    case model_kind::remove: return "remove model";
    case model_kind::natural: return "natural model";
    case model_kind::denoise: return "noise model";
    case model_kind::deblur: return "blur model";
    case model_kind::motion: return "motion-blur model";
    case model_kind::face: return "face model";
  }
  return "model";
}

// "k.<kind>.<knob>" -> (kind, knob).
std::optional<std::pair<model_kind, std::string>> knob_id(const std::string& node) {
  if (node.rfind("k.", 0) != 0) return std::nullopt;
  const std::size_t dot = node.find('.', 2);
  if (dot == std::string::npos) return std::nullopt;
  const auto k = kind_from(std::string_view(node).substr(2, dot - 2));
  if (!k) return std::nullopt;
  return std::pair{*k, node.substr(dot + 1)};
}

// "install.<kind>" -> kind, for `what`.
std::optional<model_kind> action_kind(const std::string& node, std::string_view what) {
  if (node.size() <= what.size() + 1 || node.compare(0, what.size(), what) != 0 || node[what.size()] != '.') {
    return std::nullopt;
  }
  return kind_from(std::string_view(node).substr(what.size() + 1));
}

}  // namespace

engine::engine(engine_port port, std::string self_dir, std::string data_dir, ai::fetch_options fetch)
    : port_(std::move(port)), self_dir_(std::move(self_dir)), data_dir_(std::move(data_dir)) {
#if defined(__APPLE__) && defined(__aarch64__)
  constexpr bool kAppleSilicon = true;
#else
  constexpr bool kAppleSilicon = false;
#endif
  recommended_ = recommend_tier(std::thread::hardware_concurrency(), total_memory().value_or(0), kAppleSilicon);
  std::vector<pin> pins;
  if (auto manifest = read_text(self_dir_ + "/manifest.json", 4u << 20)) pins = read_pins(*manifest);
  for (const model_kind k : kModelKinds) {
    const auto text = read_text(self_dir_ + "/models/" + kind_name(k) + "/model.json", 256u << 10);
    if (!text) continue;
    auto s = parse_spec(*text, k);
    if (!s) {
      if (port_.log) port_.log(1, "enhance: a model.json did not parse; that kind is not offered");
      continue;
    }
    files_[static_cast<int>(k)] = files_for(*s, pins);
    specs_[static_cast<int>(k)] = std::move(*s);
  }
  if (!data_dir_.empty()) prefs_ = load_prefs(data_dir_ + "/enhance.json");
  downloads::port dp;
  dp.transport = port_.transport;
  dp.free_disk = port_.free_disk;
  dp.changed = [this](model_kind k) { on_changed(k); };
  dl_ = std::make_unique<downloads>(data_dir_ + "/models", std::move(dp), std::move(fetch));
}

engine::~engine() {
  if (resume_.joinable()) resume_.join();
  {
    std::lock_guard lock(m_);
    measure_quit_ = true;
  }
  measure_cv_.notify_all();
  if (measure_t_.joinable()) measure_t_.join();
  if (runner_) runner_->cancel();
  runner_.reset();
  dl_->stop_all();
}

void engine::set_runner(std::unique_ptr<runner> r) {
  std::lock_guard lock(m_);
  runner_ = std::move(r);
}

void engine::resume_wanted() {
  if (resume_.joinable() || data_dir_.empty()) return;
  resume_ = std::thread([this] {
    for (const model_kind k : kModelKinds) {
      bool want = false;
      {
        std::lock_guard lock(m_);
        want = prefs_.want(k) && kind_offered(k) && specs_[static_cast<int>(k)].has_value();
      }
      // Only a download this person started and a quit cut short (its part
      // is here): a kind whose pin changed since (S7: still's 4.9 MB
      // Real-ESRGAN became Real-HAT-GAN's 161 MB) has nothing here and
      // waits for a click, never fetched unasked.
      if (want && dl_->status(k, files_[static_cast<int>(k)]).state == dl_state::stopped) {
        dl_->start(k, files_[static_cast<int>(k)]);
      }
    }
  });
}

const model_spec* engine::spec(model_kind k) const {
  const auto& s = specs_[static_cast<int>(k)];
  return s ? &*s : nullptr;
}

dl_status engine::download_status(model_kind k) const { return dl_->status(k, files_[static_cast<int>(k)]); }

prefs engine::current_prefs() const {
  std::lock_guard lock(m_);
  return prefs_;
}

void engine::on_changed(model_kind k) {
  bool start = false;
  {
    std::lock_guard lock(m_);
    // One-click: a click's download, then its run.
    if (pending_tool_ && pending_kind_ == k) {
      const dl_status st = dl_->status(k, files_[static_cast<int>(k)]);
      switch (st.state) {
        case dl_state::ready: start = true; break;
        case dl_state::no_space:
          end_pending_locked(run_state::failed, "Not enough free disk for the model: it needs " +
                                                    size_text(st.need_bytes) + ", and " + size_text(st.free_bytes) +
                                                    " is free.");
          break;
        case dl_state::failed:
          end_pending_locked(run_state::failed, "The model's download did not match its checksum, so it was not kept.");
          break;
        case dl_state::refused:
        case dl_state::gone:
          end_pending_locked(run_state::failed, "The model could not be downloaded.");
          break;
        case dl_state::stopped:
        case dl_state::absent:
          // Stopped by hand (Cancel, Stop the download), or cut short.
          if (prefs_.want(k)) end_pending_locked(run_state::failed, "The download stopped. Try again.");
          else end_pending_locked(run_state::cancelled, {});
          break;
        default: break;
      }
    }
    post_settings_locked();
    if (!start) post_sheet_locked();
  }
  if (start) {
    photo_target t = photo_target::detail;
    {
      std::lock_guard lock(m_);
      if (!pending_tool_) return;
      t = *pending_tool_;
      pending_tool_.reset();
      run_state_ = run_state::idle;
      prefs_.photo = t;
    }
    run(true);
  }
}

void engine::end_pending_locked(run_state st, std::string message) {
  pending_tool_.reset();
  run_state_ = st;
  run_message_ = std::move(message);
}

void engine::save_locked() {
  if (!data_dir_.empty() && !save_prefs(data_dir_ + "/enhance.json", prefs_) && port_.log) {
    port_.log(1, "enhance: its settings could not be saved");
  }
}

void engine::post_settings_locked() {
  if (settings_gen_ == 0 || !port_.post) return;
  std::vector<model_card> cards;
  for (const model_kind k : kModelKinds) {
    model_card c;
    c.kind = k;
    c.spec = spec(k);
    c.files = files_[static_cast<int>(k)];
    if (c.spec) c.st = dl_->status(k, c.files);
    // A kind not offered (kOfferMotionBlur) shows only while its files are
    // here, so they can be removed.
    if (!kind_offered(k) && c.st.state == dl_state::absent) continue;
    // A kind this build has no model for is named only when it is one the
    // issue promises (photos): video and audio always are.
    cards.push_back(std::move(c));
  }
  tier_view tv;  // m_ is held (the _locked suffix)
  tv.chosen = prefs_.tier;
  tv.recommended = recommended_;
  if (!port_.post(kSettingsScreen, settings_gen_, settings_tree(cards, tv))) settings_gen_ = 0;
}

// S7: Enhance image's chosen model when this build offers it, else Sharpen
// & restore (the kind a photo always had).
bool engine::detail_offered(model_kind k) const {
  return still_family(k) && kind_offered(k) && specs_[static_cast<int>(k)].has_value() &&
         files_[static_cast<int>(k)].usable();
}

model_kind engine::detail_locked() const {
  return detail_offered(prefs_.detail) ? prefs_.detail : model_kind::still;
}

std::vector<model_kind> engine::kinds_for_locked(bool docked, std::optional<photo_target> as) const {
  const photo_target photo = as.value_or(prefs_.photo);
  switch (item_) {
    case item_kind::still:
      // Offered only once its weights are pinned (a model.json alone is not a
      // model anyone can download). Docked (S5), Remove is the card the
      // person chose: its knobs show before anything is brushed.
      if (photo == photo_target::remove && (brushed_ > 0 || docked) &&
          specs_[static_cast<int>(model_kind::remove)]) {
        return {model_kind::remove};
      }
      if (photo == photo_target::extend && specs_[static_cast<int>(model_kind::remove)]) {
        return {model_kind::remove};  // Remove's model, in its extend mode
      }
      if (photo == photo_target::faces && specs_[static_cast<int>(model_kind::face)]) {
        return {model_kind::face};  // S10: Restore faces
      }
      if (specs_[static_cast<int>(model_kind::restyle)] && files_[static_cast<int>(model_kind::restyle)].usable() &&
          photo == photo_target::restyle) {
        return {model_kind::restyle};
      }
      return {detail_locked()};
    case item_kind::audio: return facts_.sound ? std::vector{model_kind::audio} : std::vector<model_kind>{};
    case item_kind::media:
      if (facts_.picture && facts_.sound) {
        switch (prefs_.target) {
          case clip_target::picture: return {model_kind::video};
          case clip_target::sound: return {model_kind::audio};
          case clip_target::both: return {model_kind::video, model_kind::audio};
        }
      }
      if (facts_.picture) return {model_kind::video};
      if (facts_.sound) return {model_kind::audio};
      return {};
    default: return {};
  }
}

sheet_view engine::sheet_locked(bool docked, std::optional<photo_target> as) const {
  sheet_view v;
  v.file_name = name_of(path_);
  v.library = library_key(path_);
  v.edit = edit_text(keep_);
  v.item = item_;
  v.has_picture = facts_.picture;
  v.has_sound = facts_.sound;
  v.target = prefs_.target;
  v.offers_restyle = specs_[static_cast<int>(model_kind::restyle)].has_value() &&
                     files_[static_cast<int>(model_kind::restyle)].usable();
  // Remove is offered whenever something is brushed: a model not yet
  // downloaded shows as not installed, as any kind does.
  v.offers_remove = (brushed_ > 0 || docked) && specs_[static_cast<int>(model_kind::remove)].has_value();
  v.offers_extend = specs_[static_cast<int>(model_kind::remove)].has_value();
  v.offers_faces = specs_[static_cast<int>(model_kind::face)].has_value();
  v.framed = framed_ && item_ == item_kind::still;
  v.photo = as.value_or(prefs_.photo);
  v.level = effective_tier();
  for (const model_kind k : kStillKinds) {
    if (detail_offered(k)) v.details.push_back(k);
  }
  v.detail = detail_locked();
  if ((v.photo == photo_target::restyle && !v.offers_restyle) || (v.photo == photo_target::remove && !v.offers_remove) ||
      (v.photo == photo_target::extend && !v.offers_extend) || (v.photo == photo_target::faces && !v.offers_faces)) {
    v.photo = photo_target::detail;
  }
  if (v.item == item_kind::media && !(facts_.picture && facts_.sound)) {
    v.target = facts_.picture ? clip_target::picture : clip_target::sound;
  }
  v.running = running_;
  v.progress = progress_;
  v.eta = eta_;
  v.preview = preview_;
  v.status = status_;
  switch (item_) {
    case item_kind::animation: v.note = "Enhance has no model for animations yet."; break;
    case item_kind::document: v.note = "Enhance works on photos, clips and audio, not on documents."; break;
    case item_kind::unknown: v.note = "Enhance cannot tell what kind of file this is."; break;
    case item_kind::audio:
      if (!facts_.sound) v.note = "This audio file cannot be read (it may be protected).";
      break;
    case item_kind::media:
      if (!facts_.picture && !facts_.sound) v.note = "This file has no picture or sound Enhance can read.";
      break;
    case item_kind::still: break;
  }
  bool all_ready = true;
  for (const model_kind k : kinds_for_locked(docked, as)) {
    sheet_section s;
    s.kind = k;
    s.values = prefs_.of(k);
    const model_spec* sp = spec(k);
    const dl_status st = sp ? dl_->status(k, files_[static_cast<int>(k)]) : dl_status{};
    if (sp && st.state == dl_state::ready) {
      s.spec = sp;
      level_quality(s.values, *sp, v.level);  // Restyle: the best the level allows
    } else if (!sp) {
      s.missing = "This version of Enhance has no " + std::string(model_noun(k)) + " yet.";
    } else if (st.state == dl_state::downloading || st.state == dl_state::waiting) {
      s.missing = "The " + std::string(model_noun(k)) + " is still downloading.";
      if (still_family(k) || (docked && item_ == item_kind::still)) {
        s.downloading = true;
        if (st.total_bytes > 0) {
          s.missing = "Downloading the " + std::string(model_noun(k)) + "… " + size_text(st.done_bytes) + " of " +
                      size_text(st.total_bytes) + ".";
        }
      }
    } else if ((still_family(k) || (docked && item_ == item_kind::still)) && files_[static_cast<int>(k)].usable() &&
               st.state != dl_state::gone && st.state != dl_state::refused) {
      // S7: each of Enhance image's models is fetched only when chosen, from
      // the sheet itself.
      s.missing = std::string(still_family(k) ? detail_text(k) : "This tool") + " needs its model, downloaded from Hugging Face once (" +
                  size_text(files_[static_cast<int>(k)].bytes) + "). It runs on this computer.";
      if (st.state == dl_state::no_space) {
        s.missing = "Not enough free disk for the " + std::string(model_noun(k)) + ": it needs " +
                    size_text(st.need_bytes) + ", and " + size_text(st.free_bytes) + " is free.";
      } else if (st.state == dl_state::failed) {
        s.missing = "The download never matched the model's checksum, so it was not kept.";
      }
      s.install = st.state == dl_state::stopped ? std::string("Resume the download")
                  : st.state == dl_state::absent ? "Download (" + size_text(files_[static_cast<int>(k)].bytes) + ")"
                                                 : std::string("Try again");
    } else {
      s.missing = "The " + std::string(model_noun(k)) + " is not installed. Download it in Settings → Add-ons → Enhance.";
    }
    all_ready = all_ready && s.spec && runner_ && runner_->supports(k, item_);
    v.sections.push_back(std::move(s));
  }
  if (v.sections.empty()) all_ready = false;
  if (docked) {
    v.result_path = result_path_;
    v.result_name = result_name_;
    v.result_note = result_note_;
    // Remove with nothing brushed: the brush first (the card arms it).
    if (v.item == item_kind::still && v.photo == photo_target::remove && brushed_ == 0) {
      v.brush_needed = true;
      all_ready = false;
      if (v.note.empty()) v.note = "Brush over what to remove, then press Remove (or Return).";
    }
  }
  v.can_run = all_ready && v.note.empty() && !running_;
  if (!running_ && v.note.empty() && status_.empty() && !v.sections.empty()) {
    const bool models = std::all_of(v.sections.begin(), v.sections.end(), [](const sheet_section& s) { return s.spec; });
    if (models && !runner_) {
      v.status = "This build of Enhance cannot run its models yet.";
    } else if (models) {
      for (const sheet_section& s : v.sections) {
        if (runner_->supports(s.kind, item_)) continue;
        v.status = s.kind == model_kind::video ? "This version of Enhance cannot enhance a clip's picture yet."
                   : still_family(s.kind) ? "This version of Enhance cannot enhance photos yet."
                                                  : "This version of Enhance cannot run this model.";
        break;
      }
    }
  }
  return v;
}

void engine::post_sheet_locked() {
  if (!port_.post) return;
  if (sheet_gen_ != 0 && !port_.post(kSheetScreen, sheet_gen_, sheet_tree(sheet_locked()))) sheet_gen_ = 0;
  // One-click (owner 2026-11-05): the tool screen is the Enhance tab's cards.
  if (tool_gen_ != 0) {
    const std::vector<card_view> cards = cards_locked();
    if (!port_.post(kToolScreen, tool_gen_, cards_tree(cards, run_view_locked()))) tool_gen_ = 0;
  }
}

// ---- One-click (owner 2026-11-05) ---------------------------------------------------

namespace {

model_kind kind_of_tool(photo_target t, model_kind detail) {
  switch (t) {
    case photo_target::restyle: return model_kind::restyle;
    case photo_target::remove:
    case photo_target::extend: return model_kind::remove;
    case photo_target::faces: return model_kind::face;
    case photo_target::detail: break;
  }
  return detail;
}

std::optional<photo_target> tool_from(std::string_view word) {
  for (const photo_target t : kPhotoTargets) {
    if (word == photo_target_name(t)) return t;
  }
  return std::nullopt;
}

// "go.<word>" and the like -> the tool.
std::optional<photo_target> tool_of_node(const std::string& node, std::string_view what) {
  if (node.size() <= what.size() + 1 || node.compare(0, what.size(), what) != 0 || node[what.size()] != '.') {
    return std::nullopt;
  }
  return tool_from(std::string_view(node).substr(what.size() + 1));
}

}  // namespace

std::vector<card_view> engine::cards_locked() const {
  std::vector<card_view> out;
  if (item_ != item_kind::still) return out;
  // The order of the Enhance tab's cards.
  for (const photo_target t : {photo_target::remove, photo_target::restyle, photo_target::faces, photo_target::detail,
                               photo_target::extend}) {
    const model_kind k = kind_of_tool(t, detail_locked());
    const model_spec* sp = spec(k);
    // Offered whenever this Enhance has the tool's model (a pinned one, for
    // the downloads): never gated on a palette command (the owner's "Not
    // offered" on Restyle, 2026-11-05).
    if (!sp || !kind_offered(k) || !files_[static_cast<int>(k)].usable()) continue;
    if (t == photo_target::detail && !detail_offered(k)) continue;
    card_view c;
    c.tool = t;
    c.spec = sp;
    c.v = sheet_locked(true, t);
    const dl_status st = dl_->status(k, files_[static_cast<int>(k)]);
    c.downloading = st.state == dl_state::downloading || st.state == dl_state::waiting;
    if (c.downloading && st.total_bytes > 0) {
      c.download_fraction = static_cast<double>(st.done_bytes) / static_cast<double>(st.total_bytes);
      c.download_text = size_text(st.done_bytes) + " of " + size_text(st.total_bytes);
    }
    if (!c.v.sections.empty()) c.install = c.v.sections.front().install;
    c.eta = card_eta_locked(t, c.v);
    out.push_back(std::move(c));
  }
  return out;
}

std::string engine::card_eta_locked(photo_target t, const sheet_view& v) const {
  if (v.sections.empty()) return {};
  const sheet_section& s = v.sections.front();
  const model_spec* sp = spec(s.kind);
  if (!sp) return {};
  const tier_policy pol = policy_of(v.level);
  const auto value = [&](const char* id) -> json::value {
    const knob* kb = sp->find(id);
    if (!kb) return {};
    const auto it = s.values.find(id);
    return knob_value(*kb, it == s.values.end() ? nullptr : &it->second);
  };
  if (t == photo_target::faces) {
    const json::value m = value("model");
    const double per = seconds_per_face(m.k == json::kind::string ? m.s : "") * thread_factor(pol.threads);
    return per < 1.5 ? "About 1 s a face" : "About " + std::to_string(static_cast<int>(std::lround(per))) + " s a face";
  }
  eta_in in;
  in.kind = s.kind;
  in.spec = sp;
  in.threads = pol.threads;
  in.steps = pol.restyle_steps;
  if (t == photo_target::restyle) {
    const json::value q = value("quality");
    in.quality = q.k == json::kind::string ? q.s : "standard";
    return eta_words(estimate_seconds(in)) + (in.quality == "standard" ? "" : ", more the first time");
  }
  if (t != photo_target::detail) return {};
  const auto it = measured_.find(path_);
  if (it == measured_.end()) return "Estimating…";
  if (!it->second) return {};
  in.photo = *it->second;
  const json::value sc = value("scale");
  if (sc.k == json::kind::string) in.out_scale = static_cast<std::uint32_t>(std::max(1, std::atoi(sc.s.c_str())));
  const double secs = estimate_seconds(in);
  if (secs < 0) return {};
  std::string words = eta_words(secs);
  const std::uint64_t ow = static_cast<std::uint64_t>(in.photo.width) * in.out_scale;
  const std::uint64_t oh = static_cast<std::uint64_t>(in.photo.height) * in.out_scale;
  return words + " · " + std::to_string(ow) + " × " + std::to_string(oh);
}

run_view engine::run_view_locked() const {
  run_view r;
  r.state = run_state_;
  r.id = run_id_;
  r.tool = run_tool_;
  r.message = run_message_;
  if (run_state_ == run_state::running) {
    if (pending_tool_) {
      r.downloading = true;
      const dl_status st = dl_->status(pending_kind_, files_[static_cast<int>(pending_kind_)]);
      if (st.total_bytes > 0) r.fraction = static_cast<double>(st.done_bytes) / static_cast<double>(st.total_bytes);
      r.model_name = still_family(pending_kind_) ? detail_text(pending_kind_) : std::string("the model");
    } else {
      r.fraction = progress_;
      r.eta = run_eta_;
    }
  }
  if (run_state_ == run_state::done) {
    r.result_path = result_path_;
    r.result_name = result_name_;
  }
  return r;
}

void engine::go(photo_target t) {
  // Remove and Extend run from the tool bar under the photo, on what is
  // brushed or framed: their card only fetches the model.
  const bool bar_runs = t == photo_target::remove || t == photo_target::extend;
  std::optional<model_kind> fetch;
  {
    std::lock_guard lock(m_);
    if (running_ || pending_tool_ || tool_gen_ == 0 || item_ != item_kind::still) return;
    if (t != prefs_.photo && !bar_runs) {
      prefs_.photo = t;
      save_locked();
    }
    const model_kind k = kind_of_tool(t, detail_locked());
    const model_spec* sp = spec(k);
    if (!sp || !files_[static_cast<int>(k)].usable()) return;
    const dl_status st = dl_->status(k, files_[static_cast<int>(k)]);
    if (st.state == dl_state::downloading || st.state == dl_state::waiting) return;
    if (st.state == dl_state::ready && bar_runs) return;
    if (st.state != dl_state::ready) {
      fetch = k;
      if (!bar_runs) {
        // Download, then run: the tool bar says so, and the run starts on its own.
        pending_tool_ = t;
        pending_kind_ = k;
        run_state_ = run_state::running;
        ++run_id_;
        run_tool_ = t;
        run_message_.clear();
        source_path_ = path_;
      }
    }
  }
  if (fetch) {
    install(*fetch);  // may answer at once (no room, refused): on_changed ends it
    on_changed(*fetch);
    return;
  }
  run(true);
}

void engine::want_measure_locked() {
  // Enhance image's estimate needs the photo's size and detail; a run reads
  // the photo itself, so nothing is measured while one goes.
  if (item_ != item_kind::still || path_.empty() || running_ || !runner_) return;
  if (measured_.count(path_) != 0 || measure_want_ == path_) return;
  measure_want_ = path_;
  if (!measure_t_.joinable()) measure_t_ = std::thread([this] { measure_loop(); });
  measure_cv_.notify_all();
}

void engine::measure_loop() {
  std::unique_lock lock(m_);
  for (;;) {
    measure_cv_.wait(lock, [this] { return measure_quit_ || !measure_want_.empty(); });
    if (measure_quit_) return;
    const std::string path = measure_want_;
    runner* r = runner_.get();
    lock.unlock();
    std::optional<photo_size> got = r ? r->measure(path) : std::nullopt;
    lock.lock();
    if (measure_quit_) return;
    if (measure_want_ == path) measure_want_.clear();
    if (measured_.size() >= 8) measured_.clear();  // the newest few: a session walks on
    measured_[path] = got;
    if (path == path_) post_sheet_locked();
  }
}

engine::item_facts engine::read_item(const std::string& path) const {
  // A Photos-library item ("photos:<id>", docs/design/26) is not a file the
  // add-on can read: the host's still rows resolve it, so it is a photo here
  // (owner 2026-11-04: edits of library photos). A library video fails at
  // its read with the photo's message.
  item_facts f;
  f.item = path.empty() ? item_kind::unknown : library_key(path) ? item_kind::still : sniff(head_of(path));
  if ((f.item == item_kind::media || f.item == item_kind::audio) && port_.probe) f.facts = port_.probe(path);
  f.brushed = f.item == item_kind::still && port_.brushed ? port_.brushed(path) : 0;
  f.framed = f.item == item_kind::still && port_.framed && port_.framed(path);
  return f;
}

void engine::adopt_locked(const std::string& path, const item_facts& f, std::vector<std::int64_t> keep) {
  if (path != path_) {
    status_.clear();
    forget_result_locked();  // a result belongs to the item it was made from
  }
  preview_ = false;  // a preview belongs to the run that drew it
  path_ = path;
  keep_ = f.item == item_kind::media ? std::move(keep) : std::vector<std::int64_t>{};
  item_ = f.item;
  facts_ = f.facts;
  framed_ = f.framed;  // S6: the host's Extend frame for this photo
}

void engine::forget_result_locked() {
  result_path_.clear();
  result_name_.clear();
  result_note_.clear();
}

void engine::settings_opened(std::uint64_t generation) {
  std::lock_guard lock(m_);
  settings_gen_ = generation;
  post_settings_locked();
}

void engine::sheet_opened(std::uint64_t generation, const std::string& request_path, float width_pt, float scale) {
  std::string path;
  std::vector<std::int64_t> keep;
  {
    std::lock_guard lock(m_);
    if (!pending_path_.empty()) {
      path = pending_path_;
      keep = std::move(pending_keep_);
    } else {
      path = request_path;
      if (path == path_) keep = keep_;  // the sheet reopened on the same edit
    }
    pending_path_.clear();
    pending_keep_.clear();
    if (running_ && path != path_) path = path_;  // a run keeps its item
  }
  // Read outside the lock: a slow disk delays only this sheet.
  const item_facts f = read_item(path);
  std::lock_guard lock(m_);
  sheet_gen_ = generation;
  if (width_pt >= 100 && width_pt <= 2000) sheet_width_pt_ = width_pt;
  if (scale >= 1 && scale <= 4) sheet_scale_ = scale;
  if (!running_) {
    adopt_locked(path, f, std::move(keep));
    // Remove… in Edit opened this sheet: it starts on Remove (not saved as
    // the photo's choice: it is offered only while something is brushed).
    if (f.brushed > 0 && brushed_ == 0) prefs_.photo = photo_target::remove;
    brushed_ = f.brushed;
    // S6: Fill on Edit's Extend frame opened it: it starts on Extend (not
    // saved either; the frame overrides the preset).
    if (f.framed && specs_[static_cast<int>(model_kind::remove)]) prefs_.photo = photo_target::extend;
  }
  post_sheet_locked();
}

void engine::tool_opened(std::uint64_t generation, const std::string& request_path, float width_pt, float scale) {
  std::string path = request_path;
  {
    std::lock_guard lock(m_);
    if (running_ && path != path_) path = path_;  // a run keeps its item
  }
  const item_facts f = read_item(path);
  std::lock_guard lock(m_);
  tool_gen_ = generation;
  if (width_pt >= 100 && width_pt <= 2000) tool_width_pt_ = width_pt;
  if (scale >= 1 && scale <= 4) tool_scale_ = scale;
  if (!running_) {
    adopt_locked(path, f, {});
    brushed_ = f.brushed;
  }
  want_measure_locked();
  post_sheet_locked();
}

void engine::tool_command(const std::string& value_json) {
  const auto v = json::parse(value_json, 4);
  if (!v || v->k != json::kind::object) return;
  // One-click: the photo now on screen. The cards follow it; a run (or a
  // download waiting to run) on another photo stops, as leaving it always
  // did; the result a run just opened keeps its notice ("Saved as …").
  if (const std::string* p = v->str("path"); p && !p->empty()) {
    bool stop_run = false, stop_dl = false;
    model_kind dl_kind = model_kind::still;
    {
      std::lock_guard lock(m_);
      if (tool_gen_ == 0 || *p == path_) {
        p = nullptr;
      } else if (running_) {
        stop_run = true;
        follow_path_ = *p;  // ev.done follows once the run has stopped
      } else if (pending_tool_) {
        stop_dl = true;
        dl_kind = pending_kind_;
        end_pending_locked(run_state::cancelled, {});
      }
    }
    if (stop_run) cancel();
    if (stop_dl) stop(dl_kind);
    if (p) {
      const item_facts f = read_item(*p);
      std::lock_guard lock(m_);
      if (tool_gen_ != 0 && !running_) {
        const bool onto_result = !result_path_.empty() && *p == result_path_ && run_state_ == run_state::done;
        const std::string keep_path = result_path_, keep_name = result_name_, keep_note = result_note_;
        adopt_locked(*p, f, {});
        brushed_ = f.brushed;
        if (onto_result) {
          result_path_ = keep_path;
          result_name_ = keep_name;
          result_note_ = keep_note;
        } else if (run_state_ != run_state::running) {
          run_state_ = run_state::idle;
          run_message_.clear();
        }
        want_measure_locked();
        post_sheet_locked();
      }
    }
  }
  std::optional<photo_target> chosen;
  if (const std::string* p = v->str("photo")) {
    for (const photo_target t : kPhotoTargets) {
      if (*p == photo_target_name(t)) chosen = t;
    }
  }
  const json::value* run_flag = v->find("run");
  const bool start = run_flag && run_flag->k == json::kind::boolean && run_flag->b;
  // edit-refresh S8: Restore face from the Faces panel names the chosen
  // faces' boxes; any other command runs on every face.
  std::vector<float> boxes;
  if (const json::value* fb = v->find("faces"); fb && fb->k == json::kind::array) {
    for (const json::value& n : fb->a) {
      if (n.k != json::kind::number || boxes.size() >= 4 * kMaxChosenFaces) return;
      const double d = n.is_integer ? static_cast<double>(n.i) : n.d;
      if (!(d >= -1.0 && d <= 2.0)) return;
      boxes.push_back(static_cast<float>(d));
    }
    if (boxes.size() % 4 != 0) return;
  }
  // Remove reads the brush again: the host hands its dabs over (still_mask)
  // just before it asks for a run.
  std::string path;
  bool still = false;
  {
    std::lock_guard lock(m_);
    if (tool_gen_ == 0) return;
    path = path_;
    still = item_ == item_kind::still;
  }
  const bool reread = still && !path.empty() && port_.brushed &&
                      (chosen == photo_target::remove || (!chosen && current_prefs().photo == photo_target::remove));
  const std::uint32_t brushed = reread ? port_.brushed(path) : 0;
  // S6: Fill on the canvas hands its frame over (still_extend) just before it
  // asks for an Extend run: the panel says the frame wins over the preset.
  const bool extend = chosen == photo_target::extend || (!chosen && current_prefs().photo == photo_target::extend);
  const bool reframe = still && !path.empty() && port_.framed && extend;
  const bool framed = reframe && port_.framed(path);
  {
    std::lock_guard lock(m_);
    if (tool_gen_ == 0) return;
    if (!running_) {
      if (chosen && *chosen != prefs_.photo) {
        prefs_.photo = *chosen;
        status_.clear();
        forget_result_locked();  // the last result was another tool's
        preview_ = false;
        save_locked();
      }
      if (reread && path == path_) brushed_ = brushed;
      if (reframe && path == path_) framed_ = framed;
      face_boxes_ = std::move(boxes);
    }
    post_sheet_locked();
  }
  // One-click: Enhance image, Restyle and Restore faces fetch their model
  // first when it is not here; Remove and Extend run on what is brushed or framed.
  if (start && chosen && (*chosen == photo_target::detail || *chosen == photo_target::restyle ||
                          *chosen == photo_target::faces)) {
    go(*chosen);
  } else if (start) {
    run(true);
  }
}

void engine::closed(const std::string& screen) {
  bool cancel_run = false;
  std::optional<model_kind> stop_dl;
  {
    std::lock_guard lock(m_);
    if (screen == kToolScreen && pending_tool_) {
      stop_dl = pending_kind_;
      end_pending_locked(run_state::idle, {});
    }
    if (screen == kToolScreen && run_state_ != run_state::running) run_state_ = run_state::idle;
    if (screen == kSettingsScreen) settings_gen_ = 0;
    if (screen == kSheetScreen || screen == kToolScreen) {
      (screen == kSheetScreen ? sheet_gen_ : tool_gen_) = 0;
      status_.clear();
      if (screen == kToolScreen) forget_result_locked();
      // Leaving the sheet (or the docked panel: the host closes it when the
      // item changes or Edit closes) is leaving the item: its job stops (#297
      // "Navigation away cancels the job"), unless the other still shows it.
      // The runner reports the cancel as usual.
      cancel_run = running_ && sheet_gen_ == 0 && tool_gen_ == 0;
    }
  }
  if (cancel_run) cancel();
  if (stop_dl) stop(*stop_dl);
}

void engine::command(const std::vector<std::string>& paths, std::vector<std::int64_t> keep_ns,
                     std::optional<photo_target> start_on) {
  std::uint64_t open = 0;
  float width = 420, scale = 2;
  {
    std::lock_guard lock(m_);
    if (start_on && !running_) prefs_.photo = *start_on;
    if (!paths.empty()) {
      pending_path_ = paths.front();
      pending_keep_ = std::move(keep_ns);
    }
    open = sheet_gen_;
    width = sheet_width_pt_;
    scale = sheet_scale_;
  }
  // Already open (Enhance… again, from the editor or the viewer): it moves to
  // this file and edit now, as a fresh open would.
  if (open != 0 && !paths.empty()) sheet_opened(open, paths.front(), width, scale);
  if (port_.show) port_.show(kSheetScreen, true);
}

void engine::install(model_kind k) {
  if (!spec(k) || !kind_offered(k)) return;
  {
    std::lock_guard lock(m_);
    prefs_.want(k) = true;
    save_locked();
  }
  dl_->start(k, files_[static_cast<int>(k)]);
}

void engine::stop(model_kind k) {
  {
    std::lock_guard lock(m_);
    prefs_.want(k) = false;  // stopped by hand: not resumed at the next start
    save_locked();
  }
  dl_->stop(k);
  on_changed(k);
}

void engine::remove(model_kind k) {
  {
    std::lock_guard lock(m_);
    if (running_) return;  // a run may be reading the weights
    prefs_.want(k) = false;
    save_locked();
  }
  dl_->remove(k, files_[static_cast<int>(k)]);
}

tier engine::effective_tier() const {
  tier t = recommended_;
  if (prefs_.tier != "auto") (void)tier_from(prefs_.tier, t);
  return t;
}

void engine::run(bool docked) {
  job_request r;
  runner* run = nullptr;
  {
    std::lock_guard lock(m_);
    const sheet_view v = sheet_locked(docked);
    if (!v.can_run || !runner_) {
      // One-click: the card's button says why in the tool bar's line.
      if (docked && run_state_ != run_state::running && !v.note.empty()) {
        run_state_ = run_state::failed;
        ++run_id_;
        run_tool_ = v.photo;
        run_message_ = v.note;
        post_sheet_locked();
      }
      return;
    }
    r.path = path_;
    r.keep_ns = keep_;
    r.has_sound = facts_.sound;
    r.item = item_;
    r.target = v.target;
    r.policy = policy_of(effective_tier());
    for (const sheet_section& s : v.sections) {
      job_request::part p;
      p.kind = s.kind;
      p.spec = s.spec;
      p.models_dir = data_dir_ + "/models/" + kind_name(s.kind);
      for (const knob& kb : s.spec->knobs) {
        const auto it = s.values.find(kb.id);
        p.values[kb.id] = knob_value(kb, it == s.values.end() ? nullptr : &it->second);
      }
      if (s.kind == model_kind::face && docked && !face_boxes_.empty()) {
        json::value& faces = p.values["faces"];
        faces.k = json::kind::array;
        for (const float b : face_boxes_) {
          json::value n;
          n.k = json::kind::number;
          n.d = b;
          faces.a.push_back(std::move(n));
        }
      }
      if (s.kind == model_kind::remove) p.values["mode"].k = json::kind::string;
      if (s.kind == model_kind::remove) p.values["mode"].s = v.photo == photo_target::extend ? "extend" : "remove";
      r.parts.push_back(std::move(p));
    }
    if (std::string why = runner_->check(r); !why.empty()) {
      if (docked) {
        run_state_ = run_state::failed;
        ++run_id_;
        run_tool_ = v.photo;
        run_message_ = why;
      }
      status_ = std::move(why);
      post_sheet_locked();
      return;
    }
    running_ = true;
    follow_path_.clear();
    // One-click: the run the host's glow, tool bar and result follow.
    run_state_ = run_state::running;
    ++run_id_;
    run_tool_ = v.photo;
    run_message_.clear();
    run_eta_.clear();
    source_path_ = path_;
    pending_tool_.reset();
    progress_ = -1;
    eta_.clear();
    preview_ = false;
    run_started_ = std::chrono::steady_clock::now();
    status_.clear();
    forget_result_locked();
    post_sheet_locked();
    run = runner_.get();
  }
  job_events ev;
  ev.progress = [this](double f) {
    std::lock_guard lock(m_);
    if (!running_) return;
    // Whole percent steps, and the progress node alone: the rest of the sheet
    // stays as it is.
    if (f >= 0 && progress_ >= 0 && std::floor(f * 100) == std::floor(progress_ * 100)) return;
    progress_ = f;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started_).count();
    eta_ = time_left(f, elapsed);
    run_eta_ = time_left_short(f, elapsed);
    if (sheet_gen_ == 0 && tool_gen_ == 0) return;
    bool whole = false;
    if (sheet_gen_ != 0) {
      json::writer w;
      w.begin_array().begin_object().key("op").string("set").key("id").string("progress");
      w.key("props").begin_object().key("value").number(f < 0 ? -1.0 : std::min(1.0, f)).end_object();
      w.end_object();
      w.begin_object().key("op").string("set").key("id").string("eta");
      w.key("props").begin_object().key("text").string(eta_.empty() ? std::string("Working…") : eta_).end_object();
      w.end_object().end_array();
      // The two nodes alone; a refused diff re-posts it whole.
      if (!port_.diff || !port_.diff(kSheetScreen, sheet_gen_, w.str())) whole = true;
    }
    if (tool_gen_ != 0) {
      // One-click: the tool bar's line and its bar.
      const run_view rv = run_view_locked();
      const std::string line = run_line(rv);
      json::writer w;
      w.begin_array().begin_object().key("op").string("set").key("id").string("run.progress");
      w.key("props").begin_object().key("value").number(f < 0 ? -1.0 : std::min(1.0, f));
      w.key("label").string(line).end_object();
      w.end_object();
      w.begin_object().key("op").string("set").key("id").string("run.line");
      w.key("props").begin_object().key("text").string(line).end_object();
      w.end_object().end_array();
      if (!port_.diff || !port_.diff(kToolScreen, tool_gen_, w.str())) whole = true;
    }
    if (whole) post_sheet_locked();
  };
  ev.preview = [this](preview_image before, preview_image after) {
    // Composed off the lock: a letterbox at the docked panel's pixel size
    // when it shows, else the sheet's.
    float w_pt = 420, sc = 2;
    {
      std::lock_guard lock(m_);
      // One-click: the cards show no preview (the glow is over the photo).
      if (!running_ || sheet_gen_ == 0) return;
      w_pt = sheet_width_pt_;
      sc = sheet_scale_;
    }
    const auto ow = static_cast<std::uint32_t>(std::min(2048.0f, w_pt * sc));
    const auto oh = static_cast<std::uint32_t>(std::min(1024.0f, static_cast<float>(kPreviewHeight) * sc));
    const std::vector<std::uint8_t> bgra = compose_preview(before, after, ow, oh);
    std::lock_guard lock(m_);
    if (!running_ || sheet_gen_ == 0 || !port_.surface) return;
    if (!preview_) {
      // The node first: pixels for a surface the tree does not show are dropped.
      preview_ = true;
      post_sheet_locked();
    }
    (void)port_.surface(kSheetScreen, "preview", sheet_gen_, bgra, ow, oh);
  };
  ev.done = [this](bool ok, const std::string& output, const std::string& message) {
    std::lock_guard lock(m_);
    running_ = false;
    progress_ = -1;
    // S0: a library photo's result is a new photo in the library (the host
    // lists it beside the one it was made from).
    status_ = !ok                  ? message
              : library_key(output) ? std::string("Added to your Photos library as a new photo.")
                                    : "Saved as " + output + ".";
    // S5: the docked panel's result row. Every writer puts the new file
    // beside the original (doc 29 naming), so its path is the original's
    // folder and the name the runner reports.
    forget_result_locked();
    if (ok && !output.empty()) {
      if (library_key(output)) {
        result_path_ = output;
      } else if (!library_key(path_)) {
        const std::size_t slash = path_.find_last_of("/\\");
        if (slash != std::string::npos) result_path_ = path_.substr(0, slash + 1) + output;
      }
      result_name_ = library_key(output) ? std::string() : output;
      // status_ keeps "Saved as …" for the sheet; the docked panel's row
      // says it in its own words (tool_tree hides the status while it shows).
    }
    if (ok && !message.empty()) status_ += " " + message;  // S7: parts a restorer left as they were
    // The docked panel hides the status under its result row: the same words go there.
    if (ok && !message.empty() && !result_path_.empty()) result_note_ = message;
    // One-click: done opens the result (the host's), else the reason; a run
    // that wrote nothing (Restore faces with every face kept) says why.
    if (ok && !result_path_.empty()) {
      run_state_ = run_state::done;
      run_message_ = message;
    } else if (ok) {
      run_state_ = run_state::failed;
      run_message_ = message.empty() ? std::string("Nothing changed, so nothing was written.") : message;
    } else {
      run_state_ = message.empty() ? run_state::cancelled : run_state::failed;
      run_message_ = message;
    }
    run_eta_.clear();
    post_sheet_locked();
  };
  // The cards were pointed at another photo while it ran: they follow now.
  ev.done = [this, finish = std::move(ev.done)](bool ok, const std::string& output, const std::string& message) {
    finish(ok, output, message);
    std::string follow;
    {
      std::lock_guard lock(m_);
      follow.swap(follow_path_);
    }
    if (!follow.empty()) {
      json::writer w;
      w.begin_object().key("path").string(follow).end_object();
      tool_command(w.str());
    }
  };
  run->start(std::move(r), std::move(ev));
}

void engine::cancel() {
  runner* r = nullptr;
  {
    std::lock_guard lock(m_);
    if (!running_ || !runner_) return;
    r = runner_.get();
  }
  r->cancel();
}

void engine::discard() {
  // Only the file this run wrote, never the original nor a library photo
  // (Photos asks before it deletes; the person does that in Photos).
  std::string target;
  {
    std::lock_guard lock(m_);
    // One-click opens the result, so the photo on screen may be the result
    // itself: never the photo the run was made from.
    if (running_ || result_path_.empty() || library_key(result_path_) || result_path_ == source_path_ ||
        (source_path_.empty() && result_path_ == path_)) {
      return;
    }
    target = result_path_;
  }
  // The host table's recycle_file: to the Bin / Trash, never a permanent
  // delete. Off the lock: it is a disk operation.
  const int r = port_.recycle ? port_.recycle(target) : -1;
#if defined(_WIN32)
  const char* bin = "the Recycle Bin";
#else
  const char* bin = "the Trash";
#endif
  std::lock_guard lock(m_);
  if (result_path_ != target) return;  // another run since
  const std::string name = result_name_.empty() ? std::string("the copy") : result_name_;
  if (r == 0) {
    forget_result_locked();
    preview_ = false;
    status_ = "Moved " + name + " to " + bin + ".";
    if (run_state_ == run_state::done) run_state_ = run_state::idle;
  } else if (r == 1) {
    result_note_ = "This folder has no " + std::string(bin + 4) + ", so " + name + " was kept.";
  } else {
    result_note_ = name + " could not be moved to " + bin + ".";
  }
  post_sheet_locked();
}

void engine::event(const std::string& screen, const std::string& node, std::uint32_t kind,
                   const std::string& value_json) {
  if (screen == kSettingsScreen) {
    // The performance level (owner 2026-11-04).
    if (kind == MV_SLOT_EVENT_CHANGE && node == "tier") {
      const auto v = json::parse(value_json, 4);
      tier t{};
      std::lock_guard lock(m_);
      if (v && v->k == json::kind::string && (v->s == "auto" || tier_from(v->s, t))) {
        prefs_.tier = v->s;
        save_locked();
      }
      post_settings_locked();
      // An open sheet's Restyle quality follows the level (S9): the options it
      // gates and the quality it shows and runs.
      post_sheet_locked();
      return;
    }
    if (kind != MV_SLOT_EVENT_CLICK) return;
    if (const auto k = action_kind(node, "install")) {
      install(*k);
    } else if (const auto k2 = action_kind(node, "stop")) {
      stop(*k2);
    } else if (const auto k3 = action_kind(node, "remove")) {
      remove(*k3);
    }
    return;
  }
  if (screen != kSheetScreen && screen != kToolScreen) return;
  const bool docked = screen == kToolScreen;
  if (docked && kind == MV_SLOT_EVENT_CLICK) {
    // One-click (owner 2026-11-05): a card's one button, its download's Stop,
    // and the tool bar's Cancel and Dismiss.
    if (const auto t = tool_of_node(node, "go")) {
      {
        // The card's Restore faces is every face (the Faces panel's per-face
        // Restore face names its boxes in its own command).
        std::lock_guard lock(m_);
        if (!running_) face_boxes_.clear();
      }
      go(*t);
      return;
    }
    if (const auto t = tool_of_node(node, "dlstop")) {
      model_kind k = model_kind::still;
      {
        std::lock_guard lock(m_);
        k = kind_of_tool(*t, detail_locked());
      }
      stop(k);
      return;
    }
    if (node == "run.dismiss") {
      std::lock_guard lock(m_);
      if (run_state_ != run_state::running) {
        run_state_ = run_state::idle;
        run_message_.clear();
        post_sheet_locked();
      }
      return;
    }
    if (node == "cancel") {
      std::optional<model_kind> dl;
      {
        std::lock_guard lock(m_);
        if (pending_tool_) dl = pending_kind_;
      }
      if (dl) stop(*dl);  // on_changed ends the wait as cancelled
      else cancel();
      return;
    }
  }
  // Return in Restyle's words runs it (one-click).
  if (docked && kind == MV_SLOT_EVENT_SUBMIT && node == "k.restyle.prompt") {
    const auto v = json::parse(value_json, 4);
    const model_spec* sp = spec(model_kind::restyle);
    const knob* kb = sp ? sp->find("prompt") : nullptr;
    if (v && kb) {
      if (auto ok = check_knob_value(*kb, *v)) {
        std::lock_guard lock(m_);
        if (running_) return;
        prefs_.of(model_kind::restyle)[kb->id] = std::move(*ok);
        save_locked();
      }
    }
    go(photo_target::restyle);
    return;
  }
  if (kind == MV_SLOT_EVENT_CLICK) {
    // S7: Enhance image's model, fetched from the sheet when chosen.
    if (const auto k = action_kind(node, "install"); k && (still_family(*k) || docked)) {
      {
        std::lock_guard lock(m_);
        if (running_) return;
      }
      install(*k);
      on_changed(*k);
      return;
    }
    if (const auto k = action_kind(node, "stop"); k && (still_family(*k) || docked)) {
      stop(*k);
      return;
    }
    if (node == "close" && !docked) {
      if (port_.show) port_.show(kSheetScreen, false);
    } else if (node == "run") {
      run(docked);
    } else if (node == "cancel") {
      cancel();
    } else if (node == "result.discard" && docked) {
      discard();
    }
    // "result.show" / "result.keep" are the host's (it navigates).
    return;
  }
  if (kind != MV_SLOT_EVENT_CHANGE) return;
  const auto v = json::parse(value_json, 4);
  if (!v) return;
  std::lock_guard lock(m_);
  if (running_) return;
  if (node == "photo") {
    if (v->k != json::kind::string) return;
    for (const photo_target t : kPhotoTargets) {
      if (v->s == photo_target_name(t)) prefs_.photo = t;
    }
    save_locked();
    post_sheet_locked();
    return;
  }
  if (node == "detail") {
    if (v->k != json::kind::string) return;
    if (const auto k = kind_from(v->s); k && detail_offered(*k)) prefs_.detail = *k;
    save_locked();
    post_sheet_locked();
    return;
  }
  if (node == "target") {
    if (v->k != json::kind::string) return;
    for (const clip_target t : {clip_target::picture, clip_target::sound, clip_target::both}) {
      if (v->s == target_name(t)) prefs_.target = t;
    }
    save_locked();
    post_sheet_locked();  // other sections now
    return;
  }
  const auto id = knob_id(node);
  if (!id) return;
  const model_spec* sp = spec(id->first);
  const knob* kb = sp ? sp->find(id->second) : nullptr;
  if (!kb) return;
  if (id->first == model_kind::restyle && kb->id == "quality" && v->k == json::kind::string &&
      quality_rank(v->s) > policy_of(effective_tier()).max_quality) {
    post_sheet_locked();  // above the level: not kept, and the sheet shows what is
    return;
  }
  if (auto ok = check_knob_value(*kb, *v)) {
    // Remembered for that kind of target; the host already shows the value.
    prefs_.of(id->first)[kb->id] = std::move(*ok);
    save_locked();
    // One-click: the card's estimate follows its Size and Quality (never
    // re-posted under typed words, which would take the field's focus).
    if (docked && kb->t != knob::type::text) post_sheet_locked();
  }
}

}  // namespace mv::enhance
