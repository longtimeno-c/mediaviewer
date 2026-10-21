// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #530: a receive that stops keeps what arrived, and the next send of the
// same files picks up where it stopped (transfer.h's resume_plan, resume.h).
#include "driftwire_fixture.h"

#include "addons/driftwire/resume.h"

namespace {

constexpr std::int64_t kWhen = 1'700'000'000;
const std::string kSender(64, 'a');

struct run {
  sp::transfer_result sent, got;
  sp::incoming_offer offer;
  sp::resume_plan plan;
  std::optional<sp::kept_state> kept;
  bool kept_partial_seen = false;
  std::uint64_t first_moved = 0, last_moved = 0;
  bool offered = false;
};

std::vector<sp::outgoing_file> make_files(const fs::path& src) {
  const std::vector<std::pair<std::string, std::size_t>> spec = {
      {"a.bin", 300 * 1024}, {"b.bin", 24 * 1024 * 1024}, {"c.bin", 200 * 1024}};
  std::vector<sp::outgoing_file> files;
  std::uint32_t seed = 1;
  for (const auto& [name, size] : spec) {
    const fs::path p = src / name;
    write_bytes(p, pattern(size, seed++));
    set_mtime(p, kWhen + seed);
    files.push_back({utf8(p), name, size, kWhen + seed});
  }
  return files;
}

// One transfer over a channel pair. `record`: what the receiver kept of an
// earlier one. `cut_after`: the sender drops the link once it has moved that
// much. `cancel_after`: the receiver's person cancels once that much is in.
run transfer(const std::vector<sp::outgoing_file>& files, const fs::path& dest, const sp::resume_record* record,
             std::uint64_t cut_after = 0, std::uint64_t cancel_after = 0) {
  auto [a, b] = channel_pair();
  run out;
  std::mutex m;
  std::atomic<bool> cancel{false};
  std::thread receiver([&, &b = b] {
    auto offer = sp::read_offer(*b, 10000);
    if (!offer) return;
    out.offered = true;
    out.offer = *offer;
    if (record) out.plan = sp::plan_resume(*record, *offer, utf8(dest));
    if (!sp::send_accept(*b, std::nullopt, &out.plan)) return;
    sp::hooks h;
    h.cancel = &cancel;
    h.keep = [&](const sp::kept_state& k) {
      std::lock_guard lock(m);
      out.kept = k;
      if (k.partial) out.kept_partial_seen = true;
    };
    h.progress = [&](std::uint64_t moved, std::uint32_t) {
      if (cancel_after && moved >= cancel_after) cancel.store(true);
    };
    out.got = sp::receive_files(*b, *offer, utf8(dest), h, &out.plan);
  });
  sp::hooks hs;
  bool first = true;
  std::atomic<bool> cut{false};
  hs.progress = [&](std::uint64_t moved, std::uint32_t) {
    if (first) out.first_moved = moved;
    first = false;
    out.last_moved = moved;
    if (cut_after && moved >= cut_after && !cut.exchange(true)) a->shutdown();
  };
  out.sent = sp::send_files(*a, files, hs, {});
  receiver.join();
  return out;
}

bool same_bytes(const fs::path& x, const fs::path& y) { return read_bytes(x) == read_bytes(y); }

}  // namespace

TEST_CASE("driftwire resume: a receive that stops keeps the file it was writing, and the next send picks it up",
          "[driftwire]") {
  scratch_dir s("dw_resume");
  const fs::path src = s / "src", dest = s / "dest";
  fs::create_directories(src);
  fs::create_directories(dest);
  const auto files = make_files(src);
  const std::uint64_t total = 300 * 1024 + 24 * 1024 * 1024 + 200 * 1024;

  // The link goes 8 MB into b.bin.
  run first = transfer(files, dest, nullptr, 300 * 1024 + 8 * 1024 * 1024);
  REQUIRE(first.offered);
  CHECK(first.offer.resume);
  CHECK(first.offer.files[1].mtime == files[1].mtime);
  CHECK(first.sent.how == sp::outcome::failed);
  CHECK(first.got.how == sp::outcome::failed);
  REQUIRE(first.kept);
  REQUIRE(first.kept->whole.size() == 1);
  CHECK(first.kept->whole[0].index == 0);
  REQUIRE(first.kept->partial);
  const auto part = *first.kept->partial;
  CHECK(part.index == 1);
  CHECK(part.at > 0);
  CHECK(part.at < files[1].size);
  CHECK(fs::file_size(part.temp_path) == part.at);
  // Never under its final name: what is there is the finished a.bin and a temporary.
  CHECK(files_in(dest) == std::vector<std::string>{"a.bin", "b.bin.mvtmp"});

  const auto record = sp::record_of(*first.kept, first.offer, first.plan, kSender, "Ann", utf8(dest), kWhen);
  REQUIRE(record.files.size() == 2);
  CHECK(record.bytes_here() == 300 * 1024 + part.at);

  run second = transfer(files, dest, &record);
  REQUIRE(second.offered);
  REQUIRE(second.plan.have.size() == 1);
  REQUIRE(second.plan.partial);
  CHECK(second.plan.partial->at == part.at);
  CHECK(second.sent.how == sp::outcome::delivered);
  CHECK(second.got.how == sp::outcome::delivered);
  CHECK(second.sent.done == 3);
  CHECK(second.got.done == 3);
  CHECK(second.sent.resumed_files == 1);
  CHECK(second.sent.resumed_bytes == 300 * 1024 + part.at);
  // Only what was missing went over the link.
  CHECK(second.first_moved == second.sent.resumed_bytes);
  CHECK(second.last_moved - second.first_moved == total - second.sent.resumed_bytes);
  // Whole, once each, no "(2)" and no temporary.
  CHECK(files_in(dest) == std::vector<std::string>{"a.bin", "b.bin", "c.bin"});
  for (const auto& f : files) CHECK(same_bytes(f.path, dest / f.rel));
  CHECK(second.got.written.size() == 3);
}

TEST_CASE("driftwire resume: a changed file, or a corrupt kept one, is sent again whole and never kept wrong",
          "[driftwire]") {
  scratch_dir s("dw_resume_changed");
  const fs::path src = s / "src", dest = s / "dest";
  fs::create_directories(src);
  fs::create_directories(dest);
  auto files = make_files(src);
  run first = transfer(files, dest, nullptr, 300 * 1024 + 6 * 1024 * 1024);
  REQUIRE(first.kept);
  REQUIRE(first.kept->partial);
  const auto record = sp::record_of(*first.kept, first.offer, first.plan, kSender, "Ann", utf8(dest), kWhen);

  SECTION("the source changed since: its time differs, so nothing of it is taken up") {
    files[1].mtime += 60;
    set_mtime(files[1].path, files[1].mtime);
    run second = transfer(files, dest, &record);
    CHECK_FALSE(second.plan.partial);
    CHECK(second.plan.have.size() == 1);  // a.bin is the same
    CHECK(second.got.how == sp::outcome::delivered);
    CHECK(same_bytes(files[1].path, dest / "b.bin"));
  }
  SECTION("the kept bytes were damaged: the read-back refuses the file, nothing wrong is renamed into place") {
    {
      std::fstream f(first.kept->partial->temp_path, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(1000);
      f.put('\x5a');
    }
    run second = transfer(files, dest, &record);
    REQUIRE(second.plan.partial);
    CHECK(second.got.how == sp::outcome::failed);
    CHECK_FALSE(fs::exists(dest / "b.bin"));
    CHECK_FALSE(any_temporary(dest));
    CHECK(same_bytes(files[2].path, dest / "c.bin"));
  }
  SECTION("a sender before #530 says nothing of resuming: nothing is planned") {
    sp::incoming_offer old = first.offer;
    old.resume = false;
    CHECK(sp::plan_resume(record, old, utf8(dest)).empty());
    CHECK(sp::plan_resume(record, first.offer, utf8(s / "elsewhere")).empty());
  }
}

TEST_CASE("driftwire resume: a cancel keeps nothing partial, as before", "[driftwire]") {
  scratch_dir s("dw_resume_cancel");
  const fs::path src = s / "src", dest = s / "dest";
  fs::create_directories(src);
  fs::create_directories(dest);
  const auto files = make_files(src);
  run r = transfer(files, dest, nullptr, 0, 300 * 1024 + 4 * 1024 * 1024);
  CHECK(r.got.how == sp::outcome::cancelled);
  CHECK_FALSE(any_temporary(dest));
  CHECK_FALSE(r.kept_partial_seen);
}

TEST_CASE("driftwire resume: records are kept across launches, and discarding one removes its temporary",
          "[driftwire]") {
  scratch_dir s("dw_resume_store");
  const std::string path = utf8(s / sp::kResumeFile);
  const fs::path temp = s / "b.bin.mvtmp";
  write_bytes(temp, pattern(4096, 7));
  sp::resume_record r;
  r.sender = kSender;
  r.peer = "Ann";
  r.dest = utf8(s.root());
  r.landed = "Trip (2)";
  r.saved = kWhen;
  r.count = 3;
  r.bytes = 1 << 20;
  r.files.push_back({"Trip/a.bin", 100, kWhen, utf8(s / "a.bin"), kWhen, {}, 0, true});
  r.files.push_back({"Trip/b.bin", 9000, kWhen + 1, utf8(s / "b.bin"), 0, utf8(temp), 4096, false});
  REQUIRE(sp::save_resume(path, {r}));
  const auto back = sp::load_resume(path);
  REQUIRE(back.size() == 1);
  CHECK(back[0].sender == r.sender);
  CHECK(back[0].landed == "Trip (2)");
  REQUIRE(back[0].files.size() == 2);
  CHECK(back[0].files[0].whole);
  CHECK(back[0].files[1].have == 4096);
  CHECK(back[0].bytes_here() == 100 + 4096);
  sp::discard_partials(back[0]);
  CHECK_FALSE(fs::exists(temp));
  REQUIRE(sp::save_resume(path, {}));
  CHECK(sp::load_resume(path).empty());
}

TEST_CASE("driftwire resume: the receiver quits mid-way; started again, the next send picks up where it stopped",
          "[driftwire]") {
  scratch_dir s("dw_resume_engine");
  bus b;
  client ann(b, s / "ann_received", "Ann", s / "ann_secrets");
  auto bea = std::make_unique<client>(b, s / "bea_received", "Bea", s / "bea_secrets", s / "bea_data");
  const fs::path dest = s / "dest";
  fs::create_directories(dest);
  const auto big = pattern(48 * sp::kChunkBytes, 9);
  write_bytes(s / "out" / "a_first.jpg", pattern(5000, 1));
  write_bytes(s / "out" / "b_big.mov", big);
  set_mtime(s / "out" / "a_first.jpg", kWhen);
  set_mtime(s / "out" / "b_big.mov", kWhen + 1);
  ann.pick({s / "out" / "a_first.jpg", s / "out" / "b_big.mov"});
  bea->save_into(dest);
  REQUIRE(ann.eng.snapshot().can_remember);

  bea->host.slow.store(true);  // every chunk waits on the "viewer": the copy is slow
  ann.send_to(*bea);
  REQUIRE(eventually([&] { return bea->eng.snapshot().in == sp::view::in_phase::asking; }));
  bea->eng.on_event("accept", 1, "");
  REQUIRE(eventually([&] { return bea->eng.snapshot().in_bytes > 8 * sp::kChunkBytes; }));
  bea.reset();  // MediaViewer quits there
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(files_in(dest) == std::vector<std::string>{"a_first.jpg", "b_big.mov.mvtmp"});

  // Started again: the unfinished receive is listed, and can be discarded.
  bea = std::make_unique<client>(b, s / "bea_received", "Bea", s / "bea_secrets", s / "bea_data");
  bea->save_into(dest);
  REQUIRE(eventually([&] { return bea->eng.snapshot().unfinished.size() == 1; }));
  CHECK(bea->eng.snapshot().unfinished[0].peer == "Ann");
  CHECK(bea->eng.snapshot().unfinished[0].bytes_here > 8 * sp::kChunkBytes);
  REQUIRE(eventually([&] { return bea->tree().find("Unfinished transfers") != std::string::npos; }));

  SECTION("Discard: the record and the hidden temporary go") {
    bea->eng.on_event("discard" + bea->eng.snapshot().unfinished[0].id, 1, "");
    REQUIRE(eventually([&] { return bea->eng.snapshot().unfinished.empty(); }));
    CHECK(files_in(dest) == std::vector<std::string>{"a_first.jpg"});
    CHECK_FALSE(fs::exists(s / "bea_data" / sp::kResumeFile));
  }
  SECTION("Ann sends the same files again") {
    ann.send_to(*bea);
    REQUIRE(eventually([&] { return bea->eng.snapshot().in == sp::view::in_phase::asking; }));
    bea->eng.on_event("accept", 1, "");
    ann.eng.wait_idle();
    bea->eng.wait_idle();
    INFO(bea->eng.snapshot().in_note);
    CHECK(bea->eng.snapshot().in == sp::view::in_phase::done);
    CHECK(bea->eng.snapshot().in_note.find("picked up where the last transfer stopped") != std::string::npos);
    CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
    CHECK(files_in(dest) == std::vector<std::string>{"a_first.jpg", "b_big.mov"});
    CHECK(read_bytes(dest / "b_big.mov") == big);
    CHECK(bea->eng.snapshot().unfinished.empty());
  }
}
