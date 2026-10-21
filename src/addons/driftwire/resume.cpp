// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/resume.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <tuple>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire {

namespace {

// A record lists at most an offer's files (kMaxFiles), each a few hundred bytes.
constexpr std::uint64_t kMaxResumeBytes = 32u * 1024 * 1024;

std::optional<std::string> read_file(const std::string& path) {
  io::file_reader in;
  if (!in.open(path, io::read_mode::sequential) || in.size() > kMaxResumeBytes) return std::nullopt;
  std::string text(static_cast<std::size_t>(in.size()), '\0');
  std::size_t got = 0;
  while (got < text.size()) {
    auto n = in.read({reinterpret_cast<std::uint8_t*>(text.data()) + got, text.size() - got});
    if (!n || *n == 0) return std::nullopt;
    got += *n;
  }
  return text;
}

std::uint64_t u64(const json::value& v, const char* key) {
  const auto n = v.integer(key);
  return n && *n > 0 ? static_cast<std::uint64_t>(*n) : 0;
}

std::vector<resume_record> parse(const std::string& text) {
  std::vector<resume_record> out;
  const auto doc = json::parse(text, 6);
  const json::value* list = doc ? doc->find("records") : nullptr;
  if (!list || list->k != json::kind::array) return out;
  for (const json::value& r : list->a) {
    if (out.size() == kMaxResumeRecords) break;
    const std::string* sender = r.str("sender");
    const std::string* dest = r.str("dest");
    const json::value* files = r.find("files");
    if (!sender || sender->size() != 64 || !dest || !files || files->k != json::kind::array) continue;
    resume_record rec;
    rec.sender = *sender;
    rec.dest = *dest;
    if (const std::string* p = r.str("peer")) rec.peer = *p;
    if (const std::string* l = r.str("landed")) rec.landed = *l;
    rec.saved = r.integer("saved").value_or(0);
    rec.count = u64(r, "count");
    rec.bytes = u64(r, "bytes");
    for (const json::value& f : files->a) {
      const std::string* rel = f.str("rel");
      const std::string* fin = f.str("final");
      if (!rel || !fin) continue;
      resume_file rf;
      rf.rel = *rel;
      rf.final_path = *fin;
      rf.size = u64(f, "size");
      rf.mtime = f.integer("mtime").value_or(0);
      rf.final_mtime = f.integer("final_mtime").value_or(0);
      rf.whole = f.boolean("whole").value_or(false);
      if (const std::string* t = f.str("temp")) rf.temp_path = *t;
      rf.have = u64(f, "have");
      if (!rf.whole && (rf.temp_path.empty() || rf.have == 0 || rf.have >= rf.size)) continue;
      rec.files.push_back(std::move(rf));
    }
    out.push_back(std::move(rec));
  }
  return out;
}

}  // namespace

std::uint64_t resume_record::bytes_here() const noexcept {
  std::uint64_t n = 0;
  for (const auto& f : files) n += f.whole ? f.size : f.have;
  return n;
}

std::vector<resume_record> load_resume(const std::string& path) {
  if (path.empty()) return {};
  if (auto text = read_file(path)) return parse(*text);
  // A write stopped between removing the old file and renaming the new one.
  if (auto text = read_file(path + ".new")) return parse(*text);
  return {};
}

bool save_resume(const std::string& path, const std::vector<resume_record>& records) {
  if (path.empty()) return false;
  if (records.empty()) {
    (void)io::remove_file(path + ".new");
    (void)io::remove_file(path);
    return true;
  }
  json::writer w;
  w.begin_object().key("v").integer(1).key("records").begin_array();
  for (std::size_t i = 0; i < records.size() && i < kMaxResumeRecords; ++i) {
    const resume_record& r = records[i];
    w.begin_object().key("sender").string(r.sender).key("peer").string(r.peer).key("dest").string(r.dest);
    w.key("landed").string(r.landed).key("saved").integer(r.saved);
    w.key("count").integer(static_cast<std::int64_t>(r.count)).key("bytes").integer(static_cast<std::int64_t>(r.bytes));
    w.key("files").begin_array();
    for (const resume_file& f : r.files) {
      w.begin_object().key("rel").string(f.rel).key("size").integer(static_cast<std::int64_t>(f.size));
      w.key("mtime").integer(f.mtime).key("final").string(f.final_path);
      if (f.whole) {
        w.key("whole").boolean(true).key("final_mtime").integer(f.final_mtime);
      } else {
        w.key("temp").string(f.temp_path).key("have").integer(static_cast<std::int64_t>(f.have));
      }
      w.end_object();
    }
    w.end_array().end_object();
  }
  w.end_array().end_object();
  const std::string text = w.take();
  const std::string fresh = path + ".new";
  (void)io::remove_file(fresh);  // a leftover from a write that stopped
  {
    io::file_writer out;
    auto made = out.create_new(fresh);
    if (!made || *made != io::rename_outcome::renamed) return false;
    const bool ok = out.write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}) &&
                    out.flush_durable() && out.close();
    if (!ok) {
      (void)io::remove_file(fresh);
      return false;
    }
  }
  (void)io::remove_file(path);
  auto r = io::rename_no_replace(fresh, path);
  return r && *r == io::rename_outcome::renamed;
}

void discard_partials(const resume_record& r) {
  for (const resume_file& f : r.files) {
    // Only a temporary of ours: never a file that has its final name.
    if (!f.whole && !f.temp_path.empty() && f.temp_path.find(".mvtmp") != std::string::npos) {
      (void)io::remove_file(f.temp_path);
    }
  }
}

resume_plan plan_resume(const resume_record& r, const incoming_offer& offer, const std::string& dest) {
  resume_plan plan;
  if (!offer.resume || r.dest != dest || offer.files.empty()) return plan;
  // The record's files by what the sender offered: its path, size and time.
  std::map<std::tuple<std::string_view, std::uint64_t, std::int64_t>, const resume_file*> by;
  for (const resume_file& f : r.files) by.emplace(std::make_tuple(std::string_view(f.rel), f.size, f.mtime), &f);
  std::size_t runs = 0;
  for (std::uint32_t i = 0; i < offer.files.size(); ++i) {
    const incoming_file& o = offer.files[i];
    if (o.mtime == 0) continue;  // a file whose time was not said is not known to be the same
    const auto it = by.find(std::make_tuple(std::string_view(o.rel), o.size, o.mtime));
    if (it == by.end()) continue;
    const resume_file& f = *it->second;
    const auto st = io::stat_path(f.whole ? f.final_path : f.temp_path);
    if (!st || st->is_directory) continue;
    if (f.whole) {
      // Still there as it was written: its size and the time it was stamped with.
      if (st->size != f.size || (f.final_mtime != 0 && st->mtime_unix != f.final_mtime)) continue;
      const bool joins = !plan.have.empty() && plan.have.back().index + 1 == i;
      if (!joins && runs == kMaxHaveRuns) continue;  // the accept has no room: sent again
      if (!joins) ++runs;
      plan.have.push_back({i, f.final_path});
    } else if (!plan.partial && st->size >= f.have) {
      plan.partial = resume_plan::partial_file{i, f.have, f.temp_path, f.final_path};
    }
  }
  if (!plan.empty() || !offer.folder.empty()) plan.landed = r.landed;
  return plan;
}

resume_record record_of(const kept_state& k, const incoming_offer& offer, const resume_plan& plan,
                        std::string sender, std::string peer, std::string dest, std::int64_t now) {
  resume_record rec;
  rec.sender = std::move(sender);
  rec.peer = std::move(peer);
  rec.dest = std::move(dest);
  rec.landed = k.landed;
  rec.saved = now;
  rec.count = offer.files.size();
  rec.bytes = offer.bytes;
  for (const auto& w : k.whole) {
    if (w.index >= offer.files.size()) continue;
    const incoming_file& f = offer.files[w.index];
    if (f.mtime == 0) continue;
    rec.files.push_back({f.rel, f.size, f.mtime, w.final_path, f.mtime, {}, 0, true});
  }
  auto part = k.partial;
  if (!part && plan.partial &&
      std::none_of(k.whole.begin(), k.whole.end(), [&](const auto& w) { return w.index == plan.partial->index; })) {
    part = plan.partial;
  }
  if (part && part->index < offer.files.size() && offer.files[part->index].mtime != 0) {
    const incoming_file& f = offer.files[part->index];
    rec.files.push_back({f.rel, f.size, f.mtime, part->final_path, 0, part->temp_path, part->at, false});
  }
  return rec;
}

}  // namespace mv::driftwire
