// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire (docs/design/28): the wire's rules, the sealed channel, and two
// or three engines sending to each other over loopback TCP, found through an
// in-memory discovery bus instead of the LAN beacon. The issue's verify, as
// far as one machine can run it: both sides derive the same code (the
// receiver shows it, the sender types it, #442), Accept
// delivers the files with matching bytes, Decline and a third client receive
// nothing, Cancel mid-way leaves nothing partial, a machine in the middle
// shows two different codes, and no sender path climbs out of the folder.
#include <bit>
#include <functional>

#include "driftwire_fixture.h"
#include "addons/driftwire/power.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

// #498: a link to the folder `target` at `link`: a symlink on POSIX, a
// junction on Windows (which needs no privilege, so it is what an attacker
// there would use).
bool link_folder(const fs::path& link, const fs::path& target) {
#if defined(_WIN32)
  if (!::CreateDirectoryW(link.c_str(), nullptr)) return false;
  HANDLE h = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  // REPARSE_DATA_BUFFER's mount point form (the struct is a DDK one).
  struct mount_point {
    DWORD tag;
    WORD length, reserved, subst_offset, subst_length, print_offset, print_length;
  };
  const std::wstring print = fs::absolute(target).wstring();
  const std::wstring subst = L"\\??\\" + print;
  std::vector<std::uint8_t> buf(sizeof(mount_point) + (subst.size() + print.size() + 2) * sizeof(wchar_t));
  auto* head = reinterpret_cast<mount_point*>(buf.data());
  head->tag = IO_REPARSE_TAG_MOUNT_POINT;
  head->length = static_cast<WORD>(buf.size() - 8);
  head->reserved = 0;
  head->subst_offset = 0;
  head->subst_length = static_cast<WORD>(subst.size() * sizeof(wchar_t));
  head->print_offset = static_cast<WORD>((subst.size() + 1) * sizeof(wchar_t));
  head->print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
  auto* names = reinterpret_cast<wchar_t*>(buf.data() + sizeof(mount_point));
  std::memcpy(names, subst.c_str(), (subst.size() + 1) * sizeof(wchar_t));
  std::memcpy(names + subst.size() + 1, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
  DWORD n = 0;
  const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                                    &n, nullptr);
  ::CloseHandle(h);
  return ok != FALSE;
#else
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);
  return !ec;
#endif
}

// Takes a link_folder link away again, never what it points at.
void unlink_folder(const fs::path& link) {
#if defined(_WIN32)
  ::RemoveDirectoryW(link.c_str());
#else
  std::error_code ec;
  fs::remove(link, ec);
#endif
}

}  // namespace

// ---- the wire -------------------------------------------------------------------------------

TEST_CASE("driftwire: a sender's path never leaves the receiver's folder", "[driftwire]") {
  // ZWJ (an emoji family) and ZWNJ (Persian) are letters' business, not spoofing.
  for (const char* ok : {"IMG_1.JPG", "Trip/IMG_1.JPG", "Trip/Day 2/clip.mov", "caf\xC3\xA9.heic",
                         "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7.jpg", "\xD9\x85\xE2\x80\x8C\xD9\x8A.jpg"}) {
    INFO(ok);
    CHECK(sp::safe_relative(ok));
  }
  CHECK(sp::safe_relative(std::string(sp::kMaxPartBytes, 'a')));
  CHECK_FALSE(sp::safe_relative(std::string(sp::kMaxPartBytes + 1, 'a')));
  for (const char* bad : {"", "/etc/passwd", "../x.jpg", "a/../../b", "a/./b", "a//b", "a/", "C:x.jpg", "a\\b.jpg",
                          "CON", "con.txt", "Trip/NUL.jpg", "COM1", "lpt9.log", "a.", "a ", "x?y", "x*y", "a\x01"
                          "b",
                          "\xFF\xFE",
                          // #498: hidden files and folders, and text that reads as something else.
                          ".hidden", ".ssh/authorized_keys", "Trip/.zshenv", "invoice\xE2\x80\xAEgpj.exe",
                          "a\xE2\x80\xA8" "b.jpg", "a\xC2\x85" "b.jpg", "a\xE2\x81\xA6" "b\xE2\x81\xA9.jpg",
                          "a\xE2\x80\x8B" "b.jpg"}) {
    INFO(bad);
    CHECK_FALSE(sp::safe_relative(bad));
  }
  CHECK_FALSE(sp::safe_relative(std::string(1100, 'a')));
}

TEST_CASE("driftwire: an offer is refused whole for one bad path or wrong sums", "[driftwire]") {
  CHECK(sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"../b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":6,"files":[{"rel":"a.jpg","size":2},{"rel":"b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":3,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":1,"bytes":-1,"files":[{"rel":"a.jpg","size":-1}]})"));
  CHECK_FALSE(sp::parse_offer("not json"));
  // Sizes that would wrap the total, and one past the cap.
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":1,"files":[{"rel":"a.mov","size":100000000000},)"
                              R"({"rel":"b.mov","size":9223372036854775807}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":1,"bytes":1125899906842625,"files":[{"rel":"a.mov","size":1125899906842625}]})"));
}

TEST_CASE("driftwire: an offer is files picked one by one or one folder, nothing else (#498)", "[driftwire]") {
  const auto bare = sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"b.jpg","size":3}]})");
  REQUIRE(bare);
  CHECK(bare->folder.empty());
  const auto folder =
      sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"Trip/a.jpg","size":2},{"rel":"Trip/D/b.jpg","size":3}]})");
  REQUIRE(folder);
  CHECK(folder->folder == "Trip");
  // Photos beside a login item, two folders, a folder beside a file: refused whole.
  CHECK_FALSE(sp::parse_offer(
      R"({"count":2,"bytes":5,"files":[{"rel":"a.jpg","size":2},{"rel":"Library/LaunchAgents/x.plist","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"A/a.jpg","size":2},{"rel":"B/b.jpg","size":3}]})"));
  CHECK_FALSE(sp::parse_offer(R"({"count":2,"bytes":5,"files":[{"rel":"A/a.jpg","size":2},{"rel":"A","size":3}]})"));
  // 16384 files of 2^50 bytes total 2^64: wrapped, that is 0, which must not pass for the sum.
  std::string wrap = R"({"count":16384,"bytes":0,"files":[)";
  for (int i = 0; i < 16384; ++i) {
    wrap += (i ? ",{" : "{");
    wrap += R"("rel":"f)" + std::to_string(i) + R"(","size":1125899906842624})";
  }
  wrap += "]}";
  CHECK_FALSE(sp::parse_offer(wrap));
}

TEST_CASE("driftwire: names lose the controls that would make them read as something else (#498)", "[driftwire]") {
  CHECK(sp::clean_name("Ann\xE2\x80\xAE koobcaM") == "Ann koobcaM");
  CHECK(sp::clean_name("Ann\xE2\x80\xA8(remembered)") == "Ann(remembered)");
  CHECK(sp::clean_name("\xE2\x81\xA6" "Bea\xE2\x81\xA9\xC2\x85") == "Bea");
  CHECK(sp::clean_name("Studio \xF0\x9F\x8E\xA8 Mac") == "Studio \xF0\x9F\x8E\xA8 Mac");
}

TEST_CASE("driftwire: received folders are made beneath Save to, never through a link (#498)", "[driftwire]") {
  scratch_dir s("driftwire_beneath");
  fs::create_directories(s / "dest");
  fs::create_directories(s / "elsewhere");
  REQUIRE(link_folder(s / "dest" / "Trip", s / "elsewhere"));
  const auto made = mv::io::make_directories_beneath(utf8(s / "dest"), "New/Day 2");
  REQUIRE(made);
  CHECK(fs::is_directory(s / "dest" / "New" / "Day 2"));
  const auto through = mv::io::make_directories_beneath(utf8(s / "dest"), "Trip/Day 2");
  REQUIRE_FALSE(through);
  CHECK(through.error() == mv::status::permission_denied);
  CHECK(fs::is_empty(s / "elsewhere"));
  unlink_folder(s / "dest" / "Trip");
}

TEST_CASE("driftwire: the sender reads only the file it picked, never one put in its place (#498)", "[driftwire]") {
  scratch_dir s("driftwire_same_file");
  write_text(s / "pick" / "Trip" / "a.jpg", "photo one");
  write_text(s / "secret" / "Trip" / "a.jpg", "SECRET!!!");  // the same size
  const std::string a = utf8(s / "pick" / "Trip" / "a.jpg");
  const auto picked = mv::io::identify(a);
  REQUIRE(picked);
  CHECK(picked->known());
  mv::io::file_reader in;
  REQUIRE(in.open_same(a, picked.value()));
  CHECK(in.size() == 9);
  in.close();

  // Another file put at the path, the same size: not the one picked.
  write_text(s / "pick" / "other", "photo two");
  fs::rename(s / "pick" / "other", s / "pick" / "Trip" / "a.jpg");
  auto r = in.open_same(a, picked.value());
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::corrupt);

  // The folder above swapped for a link to where a same-named file waits:
  // the path still opens, but onto another file.
  const auto now = mv::io::identify(a);
  REQUIRE(now);
  fs::rename(s / "pick" / "Trip", s / "pick" / "Trip moved");
  REQUIRE(link_folder(s / "pick" / "Trip", s / "secret" / "Trip"));
  r = in.open_same(a, now.value());
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::corrupt);
  // The link itself is never taken for a file to send.
  const auto link_id = mv::io::identify(utf8(s / "pick" / "Trip"));
  REQUIRE_FALSE(link_id);
  CHECK(link_id.error() == mv::status::permission_denied);
  unlink_folder(s / "pick" / "Trip");

  // A link at the file's own name is never read through, even with no id to
  // compare (Windows makes a file symlink only with Developer Mode on).
  std::error_code ec;
  fs::create_symlink(s / "secret" / "Trip" / "a.jpg", s / "pick" / "b.jpg", ec);
  if (!ec) {
    const auto b_id = mv::io::identify(utf8(s / "pick" / "b.jpg"));
    REQUIRE_FALSE(b_id);
    CHECK(b_id.error() == mv::status::permission_denied);
    r = in.open_same(utf8(s / "pick" / "b.jpg"), {});
    REQUIRE_FALSE(r);
    CHECK(r.error() == mv::status::permission_denied);
  } else {
    WARN("no file symlink here: the last-part link check was not run");
  }
}

TEST_CASE("driftwire: a file swapped after the pick is said skipped, and its stand-in never sent (#498)",
          "[driftwire]") {
  scratch_dir s("driftwire_swap_send");
  const fs::path src = s / "src", dest = s / "dest";
  fs::create_directories(dest);
  write_text(src / "a.jpg", "photo one");
  write_text(src / "b.jpg", "photo two");
  std::vector<sp::outgoing_file> files;
  for (const char* name : {"a.jpg", "b.jpg"}) {
    const auto id = mv::io::identify(utf8(src / name));
    REQUIRE(id);
    files.push_back({utf8(src / name), name, 9, 0, id.value()});
  }
  // Between the pick and the send, a.jpg becomes another file of its size.
  write_text(s / "stand-in", "SECRET!!!");
  fs::rename(s / "stand-in", src / "a.jpg");

  auto [a, b] = channel_pair();
  sp::transfer_result got;
  std::thread receiver([&, &b = b] {
    auto offer = sp::read_offer(*b, 10000);
    if (!offer || !sp::send_accept(*b, std::nullopt)) return;
    got = sp::receive_files(*b, *offer, utf8(dest), {});
  });
  const auto sent = sp::send_files(*a, files, {}, {});
  receiver.join();
  CHECK(sent.how == sp::outcome::delivered);
  CHECK(sent.skipped == 1);
  CHECK(sent.done == 1);
  CHECK(files_in(dest) == std::vector<std::string>{"b.jpg"});
}

TEST_CASE("driftwire: an offer too big for one message goes as several (#498)", "[driftwire]") {
  // Thousands of files with long names: well past one message (~260 KB).
  std::vector<sp::outgoing_file> files;
  std::uint64_t bytes = 0;
  for (int i = 0; i < 6000; ++i) {
    files.push_back({"", "Trip/" + std::string(150, 'x') + "/" + std::to_string(i) + ".jpg",
                     static_cast<std::uint64_t>(i), 1'700'000'000, {}});
    bytes += static_cast<std::uint64_t>(i);
  }
  const auto parts = sp::offer_messages(files, true);
  REQUIRE(parts.size() > 2);
  for (const auto& p : parts) CHECK(p.size() < sp::kMaxMessageBytes - 17);
  const std::span<const std::string> more = std::span<const std::string>(parts).subspan(1);
  const auto back = sp::parse_offer(parts[0], more);
  REQUIRE(back);
  REQUIRE(back->files.size() == files.size());
  for (std::size_t i = 0; i < files.size(); ++i) {
    INFO(i);
    REQUIRE(back->files[i].rel == files[i].rel);
    REQUIRE(back->files[i].size == files[i].size);
  }
  CHECK(back->bytes == bytes);
  CHECK(back->folder == "Trip");
  CHECK(back->handover);
  // A part missing, one more than said, or the first alone: refused whole.
  CHECK_FALSE(sp::parse_offer(parts[0], more.first(more.size() - 1)));
  std::vector<std::string> extra(more.begin(), more.end());
  extra.push_back(parts.back());
  CHECK_FALSE(sp::parse_offer(parts[0], extra));
  CHECK_FALSE(sp::parse_offer(parts[0]));
  // A small offer is still the one message every receiver reads.
  const std::vector<sp::outgoing_file> few(files.begin(), files.begin() + 3);
  CHECK(sp::offer_messages(few) == std::vector<std::string>{sp::offer_json(few)});

  // Over a real channel: the receiver reads every part, then declines.
  auto [a, b] = channel_pair();
  std::optional<sp::incoming_offer> offered;
  std::thread receiver([&, &b = b] {
    if (auto o = sp::read_offer(*b, 10000)) {
      offered = std::move(*o);
      (void)sp::send_decline(*b, sp::outcome::declined);
    }
  });
  const auto sent = sp::send_files(*a, files, {}, {});
  receiver.join();
  CHECK(sent.how == sp::outcome::declined);
  REQUIRE(offered);
  CHECK(offered->files.size() == files.size());
  CHECK(offered->bytes == bytes);
}

TEST_CASE("driftwire: an offer past the most messages one may take is not sent (#498)", "[driftwire]") {
  // kMaxFiles files of 600-byte names: some 65 MB of offer, past kMaxOfferParts.
  const std::string deep = "Trip/" + std::string(190, 'a') + "/" + std::string(190, 'b') + "/" +
                           std::string(190, 'c') + "/";
  std::vector<sp::outgoing_file> files;
  files.reserve(sp::kMaxFiles);
  for (std::size_t i = 0; i < sp::kMaxFiles; ++i) files.push_back({"", deep + std::to_string(i), 1, 0, {}});
  CHECK(sp::offer_messages(files).empty());
}

TEST_CASE("driftwire: the beacon carries an id, a name and a port, nothing else", "[driftwire]") {
  sp::beacon b{"0123456789abcdef", "Tristan\xE2\x80\x99s MacBook", 50123};
  const std::string text = sp::encode_beacon(b);
  CHECK(text.rfind("MVSP1{", 0) == 0);
  auto back = sp::decode_beacon({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
  REQUIRE(back);
  CHECK(back->id == b.id);
  CHECK(back->name == b.name);
  CHECK(back->port == 50123);
  for (const std::string& bad : {std::string("MVSP1{}"), std::string("MVSP1{\"v\":2,\"id\":\"0123456789abcdef\",\"name\":\"x\",\"port\":1}"),
                                std::string("MVSP1{\"v\":1,\"id\":\"../\",\"name\":\"x\",\"port\":1}"),
                                std::string("MVSP1{\"v\":1,\"id\":\"0123456789abcdef\",\"name\":\"x\",\"port\":0}"),
                                std::string("hello")}) {
    CHECK_FALSE(sp::decode_beacon({reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}));
  }
  CHECK(sp::clean_name(std::string(200, 'x')).size() == sp::kMaxNameBytes);
  CHECK(sp::clean_name("") == "MediaViewer");
  CHECK(sp::clean_name("a\nb\tc") == "abc");
}

TEST_CASE("driftwire: the code is six digits from both keys, the client's first", "[driftwire]") {
  std::array<std::uint8_t, 32> a{}, b{};
  a.fill(1);
  b.fill(2);
  const std::string ab = sp::pairing_code(a, b);
  CHECK(ab.size() == 7);
  CHECK(ab[3] == ' ');
  CHECK(sp::pairing_code(a, b) == ab);
  CHECK(sp::pairing_code(b, a) != ab);
}

// ---- the channel ------------------------------------------------------------------------------

TEST_CASE("driftwire: both ends of a channel show the same code and each other's name", "[driftwire]") {
  auto [a, b] = channel_pair();
  CHECK(a->code() == b->code());
  CHECK(a->peer_name() == "Bea");
  CHECK(b->peer_name() == "Ann");
  CHECK(a->peer_id() == "bbbbbbbbbbbbbbbb");
  REQUIRE(a->send(sp::msg::offer, std::string_view("{\"x\":1}")));
  sp::message m;
  REQUIRE(b->receive(m, 2000));
  CHECK(m.type == sp::msg::offer);
  CHECK(m.text() == "{\"x\":1}");
  // A big message (a chunk), the other way.
  std::vector<std::uint8_t> chunk = pattern(sp::kChunkBytes, 7);
  REQUIRE(b->send(sp::msg::data, chunk));
  REQUIRE(a->receive(m, 2000));
  CHECK(m.type == sp::msg::data);
  CHECK(m.body == chunk);
}

TEST_CASE("driftwire: a machine in the middle shows each side a different code", "[driftwire]") {
  // Ann -> Mallory -> Bea: Mallory answers Ann as a server and calls Bea as a
  // client, so it can read both streams; the codes differ, and the people
  // comparing them cancel.
  auto [ann_link, mal_in] = loopback_pair();
  auto [mal_out, bea_link] = loopback_pair();
  mv::result<std::unique_ptr<sp::channel>> ann = mv::err(mv::status::internal), bea = mv::err(mv::status::internal),
                                           mal_a = mv::err(mv::status::internal),
                                           mal_b = mv::err(mv::status::internal);
  std::thread t1([&, l = std::move(ann_link)]() mutable { ann = sp::channel::open(std::move(l), true, "aaaaaaaaaaaaaaaa", "Ann"); });
  std::thread t2([&, l = std::move(mal_in)]() mutable { mal_a = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  std::thread t3([&, l = std::move(mal_out)]() mutable { mal_b = sp::channel::open(std::move(l), true, "aaaaaaaaaaaaaaaa", "Ann"); });
  std::thread t4([&, l = std::move(bea_link)]() mutable { bea = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  t1.join();
  t2.join();
  t3.join();
  t4.join();
  REQUIRE(ann);
  REQUIRE(bea);
  CHECK((*ann)->code() != (*bea)->code());  // a 1-in-a-million coincidence aside
}

TEST_CASE("driftwire: an altered byte ends the channel (protocol 1)", "[driftwire]") {
  auto [a, b] = loopback_pair();
  // Past the hello (37) and the stream header (24): inside the first sealed message.
  auto tampered = std::make_unique<tamper_link>(std::move(a), 37 + 24 + 10);
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&, l = std::move(b)]() mutable { server = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  sp::channel::options v1;
  v1.version = sp::kProtocol1;
  auto opened = sp::channel::open(std::move(tampered), true, "aaaaaaaaaaaaaaaa", "Ann", v1);
  t.join();
  (void)opened;
  CHECK_FALSE(server);  // its first sealed message (the name) did not open
  CHECK(server.error() == mv::status::corrupt);
}

TEST_CASE("driftwire: an altered byte in protocol 2's handshake fails it on both sides", "[driftwire]") {
  // Inside the client's ML-KEM key (in the clear, but hashed into everything
  // after it), and inside the server's sealed reply.
  for (const bool client_side : {true, false}) {
    auto [a, b] = loopback_pair();
    std::unique_ptr<sp::link> ca = std::move(a), sb = std::move(b);
    if (client_side) {
      ca = std::make_unique<tamper_link>(std::move(ca), 6 + 2 + 32 + 500);
    } else {
      sb = std::make_unique<tamper_link>(std::move(sb), 2 + 32 + 300);
    }
    mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
    std::thread t([&, l = std::move(sb)]() mutable { server = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
    auto opened = sp::channel::open(std::move(ca), true, "aaaaaaaaaaaaaaaa", "Ann");
    t.join();
    INFO("tampered on the " << (client_side ? "client's" : "server's") << " side");
    REQUIRE_FALSE(opened);
    REQUIRE_FALSE(server);
    if (client_side) {
      // The server's transcript differs, so its reply does not open here; or
      // the flipped key bit left a key ML-KEM refuses, and the server hung up.
      CHECK((opened.error() == mv::status::corrupt || opened.error() == mv::status::io));
    } else {
      CHECK(opened.error() == mv::status::corrupt);  // the server's reply did not open
    }
  }
}

TEST_CASE("driftwire: a key that does not match its commitment is refused", "[driftwire]") {
  // A client that commits to one key and reveals another (what a machine in
  // the middle would need, to choose its key after seeing the server's).
  auto [a, b] = loopback_pair();
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&, l = std::move(b)]() mutable { server = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  std::uint8_t hello[37] = {'M', 'V', 'S', 'P', 1};
  for (int i = 5; i < 37; ++i) hello[i] = static_cast<std::uint8_t>(i);  // a commitment to nothing
  REQUIRE(a->write_all(hello));
  std::uint8_t theirs[37];
  REQUIRE(sp::read_exact(*a, theirs, 5000));
  std::uint8_t other_key[32] = {9};
  REQUIRE(a->write_all(other_key));
  t.join();
  REQUIRE_FALSE(server);
  CHECK(server.error() == mv::status::corrupt);
}

TEST_CASE("driftwire: pings keep a link alive but never stretch a deadline", "[driftwire]") {
  auto [a, b] = channel_pair();
  // b's pinger sends at about 2 s; a's receive with 2.5 s still ends then.
  const auto t0 = std::chrono::steady_clock::now();
  sp::message m;
  auto r = a->receive(m, 2500);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::timeout);
  CHECK(ms < 3500);
  // And with no deadline the link is still up afterwards.
  REQUIRE(b->send(sp::msg::offer, std::string_view("{}")));
  REQUIRE(a->receive(m, 2000));
  CHECK(m.type == sp::msg::offer);
}

TEST_CASE("driftwire: stage timers count the seal, the send, the receive and the open (#540)", "[driftwire]") {
  auto [a, b] = channel_pair();
  sp::stage_times tx, rx;
  a->set_stages(&tx);
  b->set_stages(&rx);
  const std::vector<std::uint8_t> chunk(sp::kChunkBytes, 0x5a);
  REQUIRE(a->send(sp::msg::data, chunk));
  sp::message m;
  REQUIRE(b->receive(m, 2000));
  REQUIRE(m.type == sp::msg::data);
  auto at = [](sp::stage s, const std::atomic<std::uint64_t>* field) {
    return field[static_cast<int>(s)].load();
  };
  CHECK(at(sp::stage::seal, tx.calls) == 1);
  CHECK(at(sp::stage::seal, tx.bytes) == sp::kChunkBytes);
  CHECK(at(sp::stage::send, tx.calls) == 1);
  CHECK(at(sp::stage::send, tx.bytes) > sp::kChunkBytes);  // the sealed frame, its header and tag
  CHECK(at(sp::stage::open, rx.bytes) >= sp::kChunkBytes);
  CHECK(at(sp::stage::recv, rx.calls) >= 1);
  const std::string j = rx.json(1.0);
  CHECK(j.find("\"open\":{") != std::string::npos);
  CHECK(j.find("\"share\":") != std::string::npos);
  CHECK(j.find("\"read\"") == std::string::npos);  // a stage that never ran is left out
  // Unset, nothing more is counted.
  a->set_stages(nullptr);
  REQUIRE(a->send(sp::msg::data, chunk));
  REQUIRE(b->receive(m, 2000));
  CHECK(at(sp::stage::seal, tx.calls) == 1);
  tx.reset();
  CHECK(tx.json() == "{}");
}

TEST_CASE("driftwire: a receive that does not wait still reads what has arrived", "[driftwire]") {
  // The sender polls with receive(m, 0) while its person types the code; a
  // deadline under 1 ms away must still read, or a busy refusal waits out kAskMs.
  auto [a, b] = channel_pair();
  REQUIRE(b->send(sp::msg::decline, std::string_view("{\"why\":\"busy\"}")));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // on the wire, in a's buffer
  sp::message m;
  REQUIRE(a->receive(m, 0));
  CHECK(m.type == sp::msg::decline);
}

TEST_CASE("driftwire: another protocol version is refused at the hello", "[driftwire]") {
  auto [a, b] = loopback_pair();
  std::uint8_t hello[37] = {'M', 'V', 'S', 'P', 9};
  REQUIRE(a->write_all(hello));
  auto server = sp::channel::open(std::move(b), false, "bbbbbbbbbbbbbbbb", "Bea");
  REQUIRE_FALSE(server);
  CHECK(server.error() == mv::status::unsupported_format);
}

// ---- the window -------------------------------------------------------------------------------

TEST_CASE("driftwire: every state of the window is a tree the hosts accept", "[driftwire]") {
  sp::view v;
  v.my_name = "Studio";
  std::vector<sp::view> states;
  states.push_back(v);
  v.file_names = {"IMG_1.JPG", "IMG_1.CR2"};
  v.file_count = 2;
  v.file_bytes = 30'000'000;
  v.peers = {{"0123456789abcdef", "Laptop", "Wi-Fi"}, {"fedcba9876543210", "Desk", "Bluetooth"}};
  v.selected_peer = "0123456789abcdef";
  v.wifi_up = true;
  states.push_back(v);
  for (auto phase : {sp::view::out_phase::waiting_wifi, sp::view::out_phase::connecting,
                     sp::view::out_phase::confirming, sp::view::out_phase::waiting,
                     sp::view::out_phase::sending,
                     sp::view::out_phase::done, sp::view::out_phase::ended}) {
    v.out = phase;
    v.out_peer = "Laptop";
    v.out_entry_note = "That is not the code on Laptop's screen. 2 tries left.";
    v.out_note = "Sent 2 files to Laptop.";
    v.out_total = 30'000'000;
    v.out_bytes = 10'000'000;
    v.out_count = 2;
    states.push_back(v);
  }
  for (auto phase : {sp::view::in_phase::connecting, sp::view::in_phase::asking, sp::view::in_phase::receiving,
                     sp::view::in_phase::done,
                     sp::view::in_phase::ended}) {
    v.in = phase;
    v.in_peer = "Laptop";
    v.in_code = "654 321";
    v.in_via = "Wi-Fi";
    v.in_names = {"a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg"};
    v.in_count = 5;
    v.in_total = 5000;
    v.in_note = "Received 5 files.";
    v.destination = "/Users/x/Pictures/MediaViewer/Received";
    states.push_back(v);
  }
  // #472 PR 2: over the internet, each state of the code and of a send to one.
  {
    sp::view i = v;
    i.in = sp::view::in_phase::none;
    i.out = sp::view::out_phase::idle;
    i.internet = true;
    states.push_back(i);  // on, no code yet
    using phase = sp::internet_receiver::phase;
    for (auto p : {phase::preparing, phase::ready, phase::used, phase::expired, phase::failed}) {
      i.code_phase = static_cast<std::uint8_t>(p);
      i.code_text = "4FWP-8GZF-AJZX-Z1PX-H4Q0-C30Z";
      i.code_link = "mediaviewer://driftwire?code=4FWP8GZFAJZXZ1PXH4Q0C30Z";
      i.code_note = "Send this code to the other person. It works once, for 10 minutes.";
      i.code_how = "UPnP, IPv6";
      states.push_back(i);
    }
    i.send_code_note = "That is not a whole code.";
    for (auto o : {sp::view::out_phase::reaching, sp::view::out_phase::naming}) {
      i.out = o;
      i.out_internet = true;
      i.out_attempt = 3;
      i.out_peer = "Ann's MacBook";
      i.out_check = "123 456";
      i.out_note = "Nothing answered at the addresses in the code.";
      states.push_back(i);
    }
    i.out = sp::view::out_phase::idle;
    i.in = sp::view::in_phase::connecting;
    i.in_internet = true;
    i.in_peer = "Ann's MacBook";
    i.in_code = "123 456";
    states.push_back(i);
  }
  for (const auto& s : states) {
    const std::string tree = sp::build_tree(s);
    mv::addon::screen::tree t;
    INFO(tree);
    CHECK(t.set(tree) == mv::addon::screen::error::none);
  }
  // The receiver shows the code while the sender types it; the sender's
  // screen never shows one (#442).
  v.in = sp::view::in_phase::connecting;
  CHECK(sp::build_tree(v).find("654 321") != std::string::npos);
  v.in = sp::view::in_phase::none;
  v.out = sp::view::out_phase::confirming;
  const std::string sender = sp::build_tree(v);
  CHECK(sender.find("out_code_entry") != std::string::npos);
  CHECK(sender.find("654 321") == std::string::npos);
}

// 2026-10-19: a transfer under way draws its path, one picture per kind of
// path; the receive code can be selected and copied with one click.
TEST_CASE("driftwire: each kind of path has its own picture, and the code copies", "[driftwire]") {
  sp::view v;
  v.my_name = "Studio";
  v.out = sp::view::out_phase::sending;
  v.out_peer = "Bea";
  v.out_total = 100;
  v.out_bytes = 40;
  struct want {
    const char* via;
    bool punched;
    const char* relay;
    const char* shows;
  };
  for (const want& c : {want{"Wi-Fi", false, "", R"("via":"wifi")"}, want{"Bluetooth", false, "", R"("via":"bluetooth")"},
                        want{"the internet", false, "", R"("via":"internet")"},
                        want{"the internet", true, "", R"("via":"dht")"},
                        want{"the internet", false, "Rob", R"("through":"Rob")"},
                        want{"the internet", false, "your relay server", R"("via":"server")"}}) {
    v.out_via = c.via;
    v.out_punched = c.punched;
    v.out_relay = c.relay;
    const std::string tree = sp::build_tree(v);
    INFO(tree);
    CHECK(tree.find(c.shows) != std::string::npos);
    CHECK(tree.find(R"("state":"moving")") != std::string::npos);
    CHECK(mv::addon::screen::tree{}.set(tree) == mv::addon::screen::error::none);
  }
  v.out_reconnecting = true;
  CHECK(sp::build_tree(v).find(R"("state":"paused")") != std::string::npos);
  v.out = sp::view::out_phase::done;
  CHECK(sp::build_tree(v).find(R"("state":"done")") != std::string::npos);
  // No transfer: no picture.
  v.out = sp::view::out_phase::idle;
  CHECK(sp::build_tree(v).find(R"("type":"link")") == std::string::npos);

  v.internet = true;
  v.code_phase = static_cast<std::uint8_t>(sp::internet_receiver::phase::ready);
  v.code_text = "ABCD EFGH JKMN";
  v.code_link = "mediaviewer://driftwire?code=ABCDEFGHJKMN";
  const std::string tree = sp::build_tree(v);
  INFO(tree);
  CHECK(tree.find(R"("text":"ABCD EFGH JKMN","style":"display","copyable":true)") != std::string::npos);
  CHECK(tree.find(R"("copy":"ABCD EFGH JKMN")") != std::string::npos);
  CHECK(tree.find(R"("copy":"mediaviewer://driftwire?code=ABCDEFGHJKMN")") != std::string::npos);
  CHECK(tree.find("public address") == std::string::npos);
  CHECK(mv::addon::screen::tree{}.set(tree) == mv::addon::screen::error::none);
}

TEST_CASE("driftwire: a set too big for Bluetooth says to use Wi-Fi before Send", "[driftwire]") {
  sp::view v;
  v.my_name = "Studio";
  v.wifi_up = true;
  v.file_names = {"clip.mov"};
  v.file_count = 1;
  v.file_bytes = 340'000'000;
  v.peers = {{"0123456789abcdef", "Laptop", "Bluetooth", false, true}};
  v.selected_peer = "0123456789abcdef";
  CHECK(sp::needs_wifi(v));
  std::string tree = sp::build_tree(v);
  INFO(tree);
  CHECK(tree.find("Click Laptop to send over Wi-Fi") != std::string::npos);
  CHECK(tree.find("Laptop is only in Bluetooth range") != std::string::npos);
  CHECK(mv::addon::screen::tree{}.set(tree) == mv::addon::screen::error::none);
  // No network here: the person turns on Wi-Fi on this computer.
  v.wifi_up = false;
  tree = sp::build_tree(v);
  CHECK(tree.find("turn on Wi-Fi here") != std::string::npos);
  // Small enough for Bluetooth, or the peer on Wi-Fi too: a plain Send.
  v.file_bytes = 1'000'000;
  CHECK_FALSE(sp::needs_wifi(v));
  v.file_bytes = 340'000'000;
  v.peers[0].wifi = true;
  CHECK_FALSE(sp::needs_wifi(v));
  CHECK(sp::build_tree(v).find("over Wi-Fi") == std::string::npos);
}

// ---- what is kept across launches (#442) ------------------------------------------------------------

TEST_CASE("driftwire: the settings page is the window's settings, with the window's ids", "[driftwire]") {
  sp::view v;
  v.my_name = "Studio";
  v.destination = "/Users/a/Downloads";
  v.file_count = 3;  // what is being sent never shows in Settings
  v.file_names = {"IMG_1.JPG"};
  v.peers = {{"0123456789abcdef", "Laptop", "Wi-Fi", true, false}};
  std::string tree = sp::build_settings_tree(v);
  INFO(tree);
  CHECK(mv::addon::screen::tree{}.set(tree) == mv::addon::screen::error::none);
  // Laid out in Settings' own scroller: no scroller of its own.
  CHECK(tree.rfind(R"({"type":"stack","id":"root")", 0) == 0);
  for (const char* id : {R"("id":"receiving")", R"("id":"background")", R"("id":"destination")",
                         R"("id":"internet")"}) {
    CHECK(tree.find(id) != std::string::npos);
  }
  CHECK(tree.find("Keep receiving when the Driftwire window is closed") != std::string::npos);
  CHECK(tree.find("/Users/a/Downloads") != std::string::npos);
  CHECK(tree.find("\"pick\":\"folder\"") != std::string::npos);
  CHECK(tree.find("IMG_1.JPG") == std::string::npos);
  CHECK(tree.find("Laptop") == std::string::npos);
  CHECK(tree.find(R"("type":"tiles")") == std::string::npos);
  // The internet's options only once it is on, as in the window.
  CHECK(tree.find(R"("id":"dht")") == std::string::npos);
  v.internet = true;
  v.relay = true;
  v.known = {"Laptop"};
  v.known_ids = {"ab12"};
  v.known_own = {true};
  tree = sp::build_settings_tree(v);
  CHECK(mv::addon::screen::tree{}.set(tree) == mv::addon::screen::error::none);
  for (const char* id : {R"("id":"dht")", R"("id":"handover")", R"("id":"relay")", R"("id":"relay_server")",
                         R"("id":"forgetab12")", R"("id":"ownab12")"}) {
    CHECK(tree.find(id) != std::string::npos);
  }
  // Nothing that sends or makes a code.
  CHECK(tree.find(R"("id":"make_code")") == std::string::npos);
  CHECK(tree.find(R"("id":"send_code")") == std::string::npos);
  // The window still says "this window".
  CHECK(sp::build_tree(v).find("Keep receiving when this window is closed") != std::string::npos);
}

TEST_CASE("driftwire: the prefs file survives a write that stopped half-way", "[driftwire]") {
  scratch_dir s("driftwire_prefs_file");
  const std::string path = utf8(s / "driftwire.json");
  CHECK_FALSE(sp::load_prefs(path).has_value());
  sp::prefs p;
  p.receiving = true;
  p.destination = "/Volumes/Photos/In";
  REQUIRE(sp::save_prefs(path, p));
  CHECK(sp::load_prefs(path) == p);
  p.receiving = false;
  REQUIRE(sp::save_prefs(path, p));
  CHECK(sp::load_prefs(path) == p);
  CHECK_FALSE(fs::exists(s / "driftwire.json.new"));
  // Stopped after removing the old file, before the rename: the new one is read.
  fs::rename(s / "driftwire.json", s / "driftwire.json.new");
  CHECK(sp::load_prefs(path) == p);
  // A leftover .new does not stop the next write.
  p.receiving = true;
  REQUIRE(sp::save_prefs(path, p));
  CHECK(sp::load_prefs(path) == p);
  write_text(s / "driftwire.json", "not json");
  CHECK_FALSE(sp::load_prefs(path).has_value());
}

// 2026-10-19: the add-on was Sharepoint; its settings carry over once, from
// the old add-on's own data folder beside this one's.
TEST_CASE("driftwire: settings kept by the add-on's old name carry over once", "[driftwire]") {
  scratch_dir s("driftwire_adopt_prefs");
  fs::create_directories(s / "sharepoint" / "data");
  fs::create_directories(s / "driftwire" / "data");
  sp::prefs old;
  old.receiving = true;
  old.internet = true;
  old.destination = "/Volumes/Photos/In";
  REQUIRE(sp::save_prefs(utf8(s / "sharepoint" / "data" / "sharepoint.json"), old));
  const std::string dir = utf8(s / "driftwire" / "data");
  const std::string path = utf8(s / "driftwire" / "data" / "driftwire.json");
  sp::adopt_old_prefs(dir, path);
  CHECK(sp::load_prefs(path) == old);
  // Its own settings, once it has them, are never replaced.
  sp::prefs mine;
  mine.dht = true;
  REQUIRE(sp::save_prefs(path, mine));
  sp::adopt_old_prefs(dir, path);
  CHECK(sp::load_prefs(path) == mine);
  // No old add-on: nothing happens.
  scratch_dir t("driftwire_adopt_none");
  fs::create_directories(t / "driftwire" / "data");
  sp::adopt_old_prefs(utf8(t / "driftwire" / "data"), utf8(t / "driftwire" / "data" / "driftwire.json"));
  CHECK_FALSE(fs::exists(t / "driftwire" / "data" / "driftwire.json"));
}

// #472: every internet path is opt-in. The defaults, a prefs file written
// before the toggles existed, and a fresh engine with its window open all
// have them off, and nothing internet-facing is made (no code, so no
// listener, mapping, hub or DHT).
TEST_CASE("driftwire: every internet path is off until the person turns it on", "[driftwire]") {
  const sp::prefs d;
  CHECK_FALSE(d.internet);
  CHECK_FALSE(d.dht);
  CHECK_FALSE(d.handover);
  CHECK_FALSE(d.relay);
  CHECK(d.relay_server.empty());

  scratch_dir s("driftwire_internet_off");
  fs::create_directories(s / "data");
  const std::string path = utf8(s / "data" / "driftwire.json");
  write_text(s / "data" / "driftwire.json", "{\"receiving\":true}");  // from before #472
  const auto old = sp::load_prefs(path);
  REQUIRE(old);
  CHECK(old->receiving);
  CHECK_FALSE(old->internet);
  CHECK_FALSE(old->dht);
  CHECK_FALSE(old->handover);
  CHECK_FALSE(old->relay);

  bus b;
  fs::create_directories(s / "in");
  fake_host host(s / "in");
  sp::engine::options o = client::options(b, "Bea");
  o.prefs_path = path;
  sp::engine eng(host, o);
  eng.screen_opened("");
  const auto v = eng.snapshot();
  CHECK_FALSE(v.internet);
  CHECK_FALSE(v.dht);
  CHECK_FALSE(v.handover);
  CHECK_FALSE(v.relay);
  CHECK(v.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::none));
  CHECK(v.code_text.empty());
  CHECK_FALSE(v.out_handover);
  CHECK_FALSE(v.in_handover);
}

TEST_CASE("driftwire: received files go to Downloads unless Save to picked another, which is kept",
          "[driftwire]") {
#if defined(__APPLE__)
  const std::string downloads = sp::net::downloads_folder();
  CHECK(downloads.size() > 10);
  CHECK(downloads.substr(downloads.size() - 10) == "/Downloads");
#endif
  scratch_dir s("driftwire_prefs");
  bus b;
  fs::create_directories(s / "Downloads");
  fs::create_directories(s / "chosen");
  fake_host host(s / "Downloads");  // the host's default: its Downloads
  sp::engine::options o = client::options(b, "Bea");
  o.prefs_path = utf8(s / "data" / "driftwire.json");
  fs::create_directories(s / "data");
  {
    sp::engine e(host, o);
    CHECK(e.snapshot().destination == utf8(s / "Downloads"));
    e.on_event("destination", 2, "\"" + json_path(s / "chosen") + "\"");
    CHECK(e.snapshot().destination == utf8(s / "chosen"));
  }
  {
    sp::engine e(host, o);  // the next launch
    CHECK(e.snapshot().destination == utf8(s / "chosen"));
  }
  fs::remove_all(s / "chosen");
  {
    sp::engine e(host, o);  // the folder went: Downloads, until another is picked
    CHECK(e.snapshot().destination == utf8(s / "Downloads"));
  }
}

TEST_CASE("driftwire: Keep receiving is kept across launches, with presence and the status text", "[driftwire]") {
  scratch_dir s("driftwire_keep");
  bus b;
  fs::create_directories(s / "data");
  sp::engine::options o = client::options(b, "Bea");
  o.prefs_path = utf8(s / "data" / "driftwire.json");
  auto members = [&] {
    std::lock_guard lock(b.m);
    return b.members.size();
  };
  {
    fake_host host(s / "in");
    sp::engine e(host, o);  // a launch with nothing kept: no presence
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(members() == 0);
    e.screen_opened("");
    e.on_event("receiving", 2, "true");
    e.screen_closed();
    REQUIRE(eventually([&] {
      std::lock_guard lock(host.m);
      return host.status == sp::kReceivingStatus;
    }));
    CHECK(members() == 1);  // still discoverable with the window closed
  }
  REQUIRE(eventually([&] { return members() == 0; }));
  {
    fake_host host(s / "in");
    sp::engine e(host, o);  // the next launch: on, window closed, nothing asked
    REQUIRE(eventually([&] { return members() == 1; }));
    CHECK(e.snapshot().receiving);
    REQUIRE(eventually([&] {
      std::lock_guard lock(host.m);
      return host.status == sp::kReceivingStatus;
    }));
    // Turned off with the window closed: no beacon, no listener, no status.
    e.on_event("receiving", 2, "false");
    REQUIRE(eventually([&] { return members() == 0; }));
    REQUIRE(eventually([&] {
      std::lock_guard lock(host.m);
      return host.status.empty();
    }));
  }
  {
    fake_host host(s / "in");
    sp::engine e(host, o);  // and off is kept too
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(members() == 0);
    CHECK_FALSE(e.snapshot().receiving);
  }
}

TEST_CASE("driftwire: one process at a time keeps receiving at launch", "[driftwire]") {
  scratch_dir s("driftwire_lock");
  const std::string path = utf8(s / "driftwire.json.lock");
  auto first = sp::net::try_lock_instance(path);
  REQUIRE(first);
#if !defined(_WIN32)
  // flock is per open file description: a second open in this process stands
  // in for a second window's process.
  CHECK_FALSE(sp::net::try_lock_instance(path));
#endif
  first.reset();
  CHECK(sp::net::try_lock_instance(path));
}

// ---- two and three clients ------------------------------------------------------------------------

TEST_CASE("driftwire: Accept delivers the files with their bytes, keeping both on a name clash",
          "[driftwire]") {
  scratch_dir s("driftwire_send");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  const fs::path dest = s / "chosen";
  fs::create_directories(dest);
  write_text(dest / "small.txt", "already here");  // a name the receiver already has

  const auto big = pattern(3 * sp::kChunkBytes + 1234, 11);
  write_bytes(s / "out" / "big.mov", big);
  write_text(s / "out" / "small.txt", "hello");
  write_bytes(s / "out" / "empty.jpg", {});
  ann.pick({s / "out" / "big.mov", s / "out" / "small.txt", s / "out" / "empty.jpg"});
  bea.save_into(dest);

  // The receiver shows the code before anything about the files leaves: it
  // knows only who is connecting until the sender's person types the code.
  ann.send_to(bea, false);
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::confirming; }));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  // #510: both ends tell the host a transfer runs (its Restart now waits).
  REQUIRE(eventually([&] { return !ann.host.busy_posts().empty() && !bea.host.busy_posts().empty(); }));
  CHECK(ann.host.busy_posts().front() == sp::kBusyWhat);
  CHECK(bea.host.busy_posts().front() == sp::kBusyWhat);
  const std::string code = bea.eng.snapshot().in_code;
  CHECK(code.size() == 7);
  CHECK(bea.tree().find(code) != std::string::npos);
  CHECK(ann.tree().find(code) == std::string::npos);  // the sender's screen never shows it
  CHECK(bea.eng.snapshot().in_count == 0);
  ann.type_code(code);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::waiting; }));
  const auto asked = bea.eng.snapshot();
  CHECK(asked.in_peer == "Ann");
  CHECK(asked.in_count == 3);
  CHECK(files_in(dest) == std::vector<std::string>{"small.txt"});  // nothing yet

  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  INFO(bea.eng.snapshot().in_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  CHECK(files_in(dest) == std::vector<std::string>{"big.mov", "empty.jpg", "small (2).txt", "small.txt"});
  CHECK(read_bytes(dest / "big.mov") == big);
  CHECK(read_bytes(dest / "small (2).txt") == std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
  const std::string kept = "already here";
  CHECK(read_bytes(dest / "small.txt") == std::vector<std::uint8_t>(kept.begin(), kept.end()));
  CHECK_FALSE(any_temporary(dest));
  // The originals are untouched (rule 5).
  CHECK(read_bytes(s / "out" / "big.mov") == big);
  // #510: and that it has ended, once each.
  REQUIRE(eventually([&] { return ann.host.busy_posts().size() == 2 && bea.host.busy_posts().size() == 2; }));
  CHECK(ann.host.busy_posts().back().empty());
  CHECK(bea.host.busy_posts().back().empty());
#if defined(__APPLE__)
  // #498: marked as from another computer, so Gatekeeper checks it before it runs.
  char mark[128] = {};
  const ssize_t n = ::getxattr(utf8(dest / "big.mov").c_str(), "com.apple.quarantine", mark, sizeof mark, 0, 0);
  REQUIRE(n > 0);
  CHECK(std::string(mark, static_cast<std::size_t>(n)).find(";MediaViewer Driftwire;") != std::string::npos);
  CHECK(::getxattr(utf8(s / "out" / "big.mov").c_str(), "com.apple.quarantine", nullptr, 0, 0, 0) < 0);
#elif defined(_WIN32)
  // #498: the Mark of the Web, so SmartScreen and Office check it first.
  const std::vector<std::uint8_t> zone = read_bytes(fs::path((dest / "big.mov").wstring() + L":Zone.Identifier"));
  CHECK(std::string(zone.begin(), zone.end()) == "[ZoneTransfer]\r\nZoneId=3\r\n");
  CHECK_FALSE(fs::exists(fs::path((s / "out" / "big.mov").wstring() + L":Zone.Identifier")));
#endif
}

TEST_CASE("driftwire: a new set to send never waits for the one still being read", "[driftwire]") {
  // Issue #442: the share icon on Windows stalled the UI while an earlier
  // selection was still being read (set_files joined the reader on the
  // caller's thread). The caller hands the set over and returns.
  scratch_dir s("driftwire_handoff");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  write_text(s / "one" / "a.jpg", "a");
  write_text(s / "two" / "b.jpg", "b");
  ann.host.hold_pairs.store(true);
  ann.eng.set_files({utf8(s / "one" / "a.jpg")});
  REQUIRE(eventually([&] { return ann.host.pair_calls.load() >= 1; }));  // the reader is held inside the first set
  // Under the old join this call would wait for the release below.
  std::thread release([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ann.host.hold_pairs.store(false);
  });
  const auto t0 = std::chrono::steady_clock::now();
  ann.eng.set_files({utf8(s / "two" / "b.jpg")});
  ann.eng.on_event("clear", 1, "");
  ann.eng.set_files({utf8(s / "two" / "b.jpg")});
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 100);
  CHECK(ann.eng.snapshot().reading_folder);
  release.join();
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1;
  }));
  CHECK(ann.eng.snapshot().file_names == std::vector<std::string>{"b.jpg"});  // the newest set, not the first
  CHECK(ann.host.pair_calls.load() == 2);  // the replaced middle set was never read
}

TEST_CASE("driftwire: a wrong code three times ends the link, and nothing about the files leaves",
          "[driftwire]") {
  scratch_dir s("driftwire_code");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  fs::create_directories(s / "bea_received");
  write_text(s / "out" / "secret name.jpg", "only after the code");
  ann.pick({s / "out" / "secret name.jpg"});
  bea.save_into(s / "bea_received");

  // A wrong code, then the right one: one wrong try is not the end.
  ann.send_to(bea, false);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  std::string code = bea.eng.snapshot().in_code;
  std::string wrong = code;
  wrong[0] = wrong[0] == '9' ? '1' : static_cast<char>(wrong[0] + 1);
  ann.type_code("12");  // a partial entry is not a try
  CHECK(ann.eng.snapshot().out_tries_left == sp::kCodeTries);
  ann.type_code(wrong);
  CHECK(ann.eng.snapshot().out_tries_left == sp::kCodeTries - 1);
  CHECK(ann.eng.snapshot().out_entry_note.find("2 tries left") != std::string::npos);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::confirming);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::connecting);  // no offer yet
  CHECK(bea.eng.snapshot().in_names.empty());
  ann.type_code(code.substr(0, 3) + "-" + code.substr(4));  // separators do not matter
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();

  // Three wrong ones: the link ends, and the receiver never heard a name.
  ann.send_to(bea, false);
  REQUIRE(eventually([&] {
    const auto v = bea.eng.snapshot();
    return v.in == sp::view::in_phase::connecting && v.in_code != code;
  }));
  code = bea.eng.snapshot().in_code;
  wrong = code;
  wrong[6] = wrong[6] == '9' ? '1' : static_cast<char>(wrong[6] + 1);
  for (int i = 0; i < sp::kCodeTries; ++i) ann.type_code(wrong);
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(ann.eng.snapshot().out_note.find("wrong 3 times") != std::string::npos);
  const auto there = bea.eng.snapshot();
  CHECK(there.in == sp::view::in_phase::ended);
  CHECK(there.in_names.empty());
  CHECK(there.in_count == 0);
  CHECK(bea.tree().find("secret name") == std::string::npos);
  CHECK(files_in(s / "bea_received").empty());
}

TEST_CASE("driftwire: many files checked while the next arrive all land whole, same names kept apart",
          "[driftwire]") {
  // #442: file N is flushed, read back and renamed on the verifier while
  // file N+1 arrives. Two files of one offer with one name both stay.
  scratch_dir s("driftwire_pipeline");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  const fs::path dest = s / "dest";
  fs::create_directories(dest);
  std::vector<fs::path> files;
  for (int i = 0; i < 40; ++i) {
    const fs::path f = s / "out" / ("IMG_" + std::to_string(i) + ".JPG");
    write_bytes(f, pattern(static_cast<std::size_t>(1000 + i * 7919), static_cast<std::uint8_t>(i)));
    files.push_back(f);
  }
  write_text(s / "other" / "IMG_0.JPG", "another camera");
  files.push_back(s / "other" / "IMG_0.JPG");
  ann.pick(files);
  bea.save_into(dest);
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(bea.eng.snapshot().in_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  CHECK(bea.eng.snapshot().in_files == 41);
  CHECK(files_in(dest).size() == 41);
  CHECK_FALSE(any_temporary(dest));
  for (int i = 1; i < 40; ++i) {
    const std::string name = "IMG_" + std::to_string(i) + ".JPG";
    CHECK(read_bytes(dest / name) == read_bytes(s / "out" / name));
  }
  // Both IMG_0.JPG arrived; which one got the plain name depends on which check ended first.
  const auto a = read_bytes(dest / "IMG_0.JPG");
  const auto c = read_bytes(dest / "IMG_0 (2).JPG");
  CHECK(((a == read_bytes(s / "out" / "IMG_0.JPG") && c == read_bytes(s / "other" / "IMG_0.JPG")) ||
         (c == read_bytes(s / "out" / "IMG_0.JPG") && a == read_bytes(s / "other" / "IMG_0.JPG"))));
}

TEST_CASE("driftwire: a pair half and a sidecar ride along; a folder keeps its shape", "[driftwire]") {
  scratch_dir s("driftwire_pairs");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  write_text(s / "card" / "IMG_1.JPG", "jpeg");
  write_text(s / "card" / "IMG_1.CR2", "raw");
  write_text(s / "card" / "IMG_1.xmp", "sidecar");
  write_text(s / "card" / "IMG_2.JPG", "other");
  ann.eng.set_files({utf8(s / "card" / "IMG_1.JPG")});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 3;
  }));
  auto names = ann.eng.snapshot().file_names;
  std::sort(names.begin(), names.end());
  CHECK(names == std::vector<std::string>{"IMG_1.CR2", "IMG_1.JPG", "IMG_1.xmp"});

  write_text(s / "Trip" / "a.jpg", "a");
  write_text(s / "Trip" / "Day 2" / "b.jpg", "b");
  write_text(s / "Trip" / "@eaDir" / "a.jpg@SynoEAStream", "nas");
  write_text(s / "Trip" / "Thumbs.db", "win");
  write_text(s / "Trip" / "x.jpg.mvtmp", "left over");
  ann.eng.on_event("send_folder", 2, "\"" + json_path(s / "Trip") + "\"");
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 2;
  }));
  bea.save_into(s / "bea_received");
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");  // the default folder is made on Accept
  bea.eng.wait_idle();
  ann.eng.wait_idle();
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"Trip/Day 2/b.jpg", "Trip/a.jpg"});

  // #498: sent again, the folder lands beside the first, never merged into it.
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  CHECK(bea.eng.snapshot().in_folder == "Trip");
  CHECK(bea.tree().find("In the folder Trip") != std::string::npos);
  bea.eng.on_event("accept", 1, "");
  bea.eng.wait_idle();
  ann.eng.wait_idle();
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"Trip (2)/Day 2/b.jpg", "Trip (2)/a.jpg",
                                                                   "Trip/Day 2/b.jpg", "Trip/a.jpg"});
}

TEST_CASE("driftwire: Decline, closing the window, and a third client receive nothing", "[driftwire]") {
  scratch_dir s("driftwire_decline");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  client cal(b, s / "cal_received", "Cal");
  fs::create_directories(s / "bea_received");
  fs::create_directories(s / "cal_received");
  write_text(s / "out" / "secret.jpg", "only for Bea");
  ann.pick({s / "out" / "secret.jpg"});
  REQUIRE(eventually([&] { return cal.sees(ann) && cal.sees(bea); }));

  // Decline.
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note == "Bea declined.");
  CHECK(files_in(s / "bea_received").empty());

  // Closing the window while it asks declines too.
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.screen_closed();
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note == "Bea declined.");
  CHECK(files_in(s / "bea_received").empty());

  // Cal heard everyone and received nothing.
  CHECK(files_in(s / "cal_received").empty());
  CHECK(cal.eng.snapshot().in == sp::view::in_phase::none);
}

TEST_CASE("driftwire: a receiver already asking answers a second sender busy", "[driftwire]") {
  scratch_dir s("driftwire_busy");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  client cal(b, s / "cal_received", "Cal");
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  cal.pick({s / "out" / "a.jpg"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  cal.send_to(bea);
  cal.eng.wait_idle();
  CHECK(cal.eng.snapshot().out_note.find("receiving from someone else") != std::string::npos);
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
}

TEST_CASE("driftwire: Cancel mid-way keeps what finished and leaves nothing partial", "[driftwire]") {
  scratch_dir s("driftwire_cancel");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  const fs::path dest = s / "dest";
  fs::create_directories(dest);
  write_text(s / "out" / "a_first.jpg", "first");
  const auto big = pattern(16 * sp::kChunkBytes, 3);
  write_bytes(s / "out" / "b_big.mov", big);
  ann.pick({s / "out" / "a_first.jpg", s / "out" / "b_big.mov"});
  bea.save_into(dest);

  SECTION("the receiver cancels") {
    bea.host.slow.store(true);  // every chunk waits on the "viewer": the copy is slow
    ann.send_to(bea);
    REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
    bea.eng.on_event("accept", 1, "");
    REQUIRE(eventually([&] { return bea.eng.snapshot().in_bytes > sp::kChunkBytes; }));
    bea.eng.on_event("in_cancel", 1, "");
  }
  SECTION("the sender cancels") {
    ann.host.slow.store(true);
    ann.send_to(bea);
    REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
    bea.eng.on_event("accept", 1, "");
    REQUIRE(eventually([&] { return ann.eng.snapshot().out_bytes > sp::kChunkBytes; }));
    ann.eng.on_event("out_cancel", 1, "");
  }
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(bea.eng.snapshot().in_note);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::ended);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(files_in(dest) == std::vector<std::string>{"a_first.jpg"});  // whole, verified
  CHECK_FALSE(any_temporary(dest));
}

TEST_CASE("driftwire: no room on the receiver sends nothing", "[driftwire]") {
  scratch_dir s("driftwire_room");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  fs::create_directories(s / "bea_received");
  bea.host.room = 1000;
  write_bytes(s / "out" / "a.mov", pattern(5000, 1));
  ann.pick({s / "out" / "a.mov"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note.find("does not have room") != std::string::npos);
  CHECK(files_in(s / "bea_received").empty());
}

TEST_CASE("driftwire: an offer to a closed window with receiving on opens it", "[driftwire]") {
  scratch_dir s("driftwire_receiving");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  bea.eng.on_event("receiving", 2, "true");
  bea.eng.screen_closed();
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  {
    std::lock_guard lock(bea.host.m);
    CHECK(bea.host.shown == 1);
  }
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
}

TEST_CASE("driftwire: an offer is a notification naming the sender and count, never a file or the code (#452)",
          "[driftwire]") {
  scratch_dir s("driftwire_notice");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  bea.host.notify = true;
  bea.eng.on_event("receiving", 2, "true");
  bea.eng.screen_closed();
  write_text(s / "out" / "secret-name.jpg", "abc");
  ann.pick({s / "out" / "secret-name.jpg"});
  ann.send_to(bea, false);
  // The code shows (in the window, when opened): the notification says who, not the code.
  std::string code;
  REQUIRE(eventually([&] {
    const auto v = bea.eng.snapshot();
    code = v.in_code;
    return v.in == sp::view::in_phase::connecting && !code.empty();
  }));
  REQUIRE(eventually([&] { return bea.host.last_notice().find("Ann wants to send you files") != std::string::npos; }));
  CHECK(bea.host.last_notice().find(code) == std::string::npos);
  {
    std::lock_guard lock(bea.host.m);
    CHECK(bea.host.shown == 0);  // the notification opens it, not the engine
  }
  // The code typed over there: now the count and size, still no name and no code.
  ann.type_code(code);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  REQUIRE(eventually([&] { return bea.host.last_notice().find("1 file,") != std::string::npos; }));
  CHECK(bea.host.last_notice().find("secret-name") == std::string::npos);
  CHECK(bea.host.last_notice().find(code) == std::string::npos);
  // The notification's Decline is the window's Decline; the notification goes.
  bea.eng.on_event("decline", 1, "");  // what the add-on maps MV_SLOT_EVENT_NOTICE to
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(ann.eng.snapshot().out_note == "Bea declined.");
  CHECK(files_in(s / "bea_received").empty());
  CHECK(bea.host.last_notice().empty());  // taken back

  // Nothing is accepted from a notification: Accept is only in the window.
  CHECK(sp::notice_body("Ann", 0, 0) == "Ann wants to send you files. Open Driftwire to see the code.");
  CHECK(sp::notice_body("Ann", 24, 412ull * 1000 * 1000).rfind("Ann wants to send you 24 files, ", 0) == 0);
  CHECK(sp::notice_body("", 1, 10).rfind("A computer nearby wants to send you 1 file, ", 0) == 0);
}

TEST_CASE("driftwire: Receive when MediaViewer is closed keeps receiving and the process, and is kept (#452)",
          "[driftwire]") {
  scratch_dir s("driftwire_background");
  bus b;
  fs::create_directories(s / "data");
  sp::engine::options o = client::options(b, "Bea");
  o.prefs_path = utf8(s / "data" / "driftwire.json");
  auto members = [&] {
    std::lock_guard lock(b.m);
    return b.members.size();
  };
  {
    fake_host host(s / "in");
    sp::engine e(host, o);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_FALSE(host.keep.load());  // off by default: nothing kept, nothing asked
    CHECK(host.asks == 0);
    CHECK(members() == 0);
    e.screen_opened("");
    e.on_event("background", 2, "true");
    CHECK(e.snapshot().receiving);  // implies Keep receiving
    CHECK(host.asks == 1);          // the OS is asked now, never at launch
    e.screen_closed();
    REQUIRE(eventually([&] { return host.keep.load(); }));
    CHECK(members() == 1);
  }
  {
    fake_host host(s / "in");
    sp::engine e(host, o);  // the next launch: kept, nothing asked
    REQUIRE(eventually([&] { return host.keep.load() && members() == 1; }));
    CHECK(e.snapshot().background);
    CHECK(e.snapshot().receiving);
    CHECK(host.asks == 0);
    // Keep receiving off turns this off too: the process is not kept.
    e.on_event("receiving", 2, "false");
    REQUIRE(eventually([&] { return !host.keep.load() && members() == 0; }));
    CHECK_FALSE(e.snapshot().background);
  }
  {
    fake_host host(s / "in");
    sp::engine e(host, o);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(host.keep.load());
    CHECK_FALSE(e.snapshot().background);
    CHECK(members() == 0);
  }
}

TEST_CASE("driftwire: with the window closed and receiving off there is no presence", "[driftwire]") {
  scratch_dir s("driftwire_idle");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  REQUIRE(eventually([&] { return ann.sees(bea); }));
  bea.eng.screen_closed();
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    return b.members.size() == 1;  // Bea left the bus: no beacon, no listener
  }));
  CHECK(eventually([&] { return !bea.eng.snapshot().wifi_up; }));
  REQUIRE(eventually([&] { return !ann.sees(bea); }, 10));
}

TEST_CASE("driftwire: a peer that connects and stalls cannot hold the receiver on shutdown", "[driftwire]") {
  scratch_dir s("driftwire_stall");
  bus b;
  auto bea = std::make_unique<client>(b, s / "bea_received", "Bea");
  std::uint16_t port = 0;
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) port = m.self.port;
    return port != 0;
  }));
  // Connects, then says nothing: the handshake waits for it.
  auto stalled = sp::net::connect({"127.0.0.1", port}, 2000);
  REQUIRE(stalled);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto t0 = std::chrono::steady_clock::now();
  bea.reset();  // the add-on's shutdown
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 3000);  // not the handshake's 8 s
}


TEST_CASE("driftwire: made-up beacons cannot flood the list, hide a real peer or take its address", "[driftwire]") {
  scratch_dir s("driftwire_flood");
  std::atomic<inject_discovery*> inject{nullptr};
  fake_host host(s / "received");
  sp::engine::options o;
  o.my_name = "Ann";
  o.use_bluetooth = false;
  o.loopback_only = true;
  o.make_discovery = [&inject] {
    auto d = std::make_unique<inject_discovery>();
    inject = d.get();
    return std::unique_ptr<sp::discovery>(std::move(d));
  };
  sp::engine eng(host, o);
  eng.screen_opened("");
  REQUIRE(eventually([&] {
    const inject_discovery* d = inject.load();
    return d && d->ready.load();
  }));
  const auto flood_id = [](int i) {
    char id[17];
    std::snprintf(id, sizeof id, "%016x", 0x1000 + i);
    return std::string(id);
  };
  // #502: one address lists a few ids, however many it makes up.
  for (int i = 0; i < 500; ++i) {
    inject.load()->seen(sp::beacon{flood_id(i), "Flood " + std::to_string(i), 40000}, "10.0.0.9");
  }
  REQUIRE(eventually([&] { return eng.snapshot().peers.size() == sp::kMaxIdsPerAddress; }));
  // From many addresses, the list fills to what Wi-Fi may take (room is kept
  // for Bluetooth), and a real peer heard after the flood is still listed:
  // the least recently heard made-up one gives way.
  for (int i = 0; i < 200; ++i) {
    const std::string from = "10.0." + std::to_string(1 + i / 200) + "." + std::to_string(1 + i % 200);
    inject.load()->seen(sp::beacon{flood_id(1000 + i), "Flood", 40000}, from);
  }
  REQUIRE(eventually([&] { return eng.snapshot().peers.size() == sp::kMaxPeers - sp::kBluetoothRoom; }));
  inject.load()->seen(sp::beacon{"00000000000000ee", "Real", 40000}, "10.0.9.1");
  const auto listed = [&](const char* name) {
    const auto v = eng.snapshot();
    return std::any_of(v.peers.begin(), v.peers.end(), [&](const auto& p) { return p.name == name; });
  };
  REQUIRE(eventually([&] { return listed("Real"); }));
  CHECK(eng.snapshot().peers.size() <= sp::kMaxPeers - sp::kBluetoothRoom);
  // A fresh peer keeps its address: a second machine claiming its id is ignored.
  inject.load()->seen(sp::beacon{"00000000000000ee", "Somebody else", 50000}, "10.0.0.66");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(listed("Real"));
  CHECK_FALSE(listed("Somebody else"));
}

// ---- #502: what one source can hold or spam ------------------------------------------------------

TEST_CASE("driftwire: nearby is this machine's link, not anything routable (#502)", "[driftwire]") {
  const auto ip = [](const char* text) { return *sp::net::parse_address(text); };
  const std::vector<sp::net::subnet> nets = {{ip("192.168.1.20"), 24}, {ip("10.8.0.2"), 30}, {ip("2001:db8:1::5"), 64}};
  CHECK(sp::net::on_link(ip("192.168.1.77"), nets));
  CHECK_FALSE(sp::net::on_link(ip("192.168.2.77"), nets));  // another subnet
  CHECK(sp::net::on_link(ip("10.8.0.1"), nets));
  CHECK_FALSE(sp::net::on_link(ip("10.8.0.4"), nets));      // past the /30
  CHECK_FALSE(sp::net::on_link(ip("8.8.8.8"), nets));       // the internet
  CHECK(sp::net::on_link(ip("2001:db8:1::99"), nets));
  CHECK_FALSE(sp::net::on_link(ip("2001:db8:2::99"), nets));
  // Loopback and link-local are always on the link; a /0 never makes everything so.
  CHECK(sp::net::on_link(ip("127.0.0.1"), {}));
  CHECK(sp::net::on_link(ip("169.254.3.4"), {}));
  CHECK(sp::net::on_link(ip("fe80::1"), {}));
  CHECK(sp::net::on_link(ip("::1"), {}));
  const std::vector<sp::net::subnet> any = {{ip("0.0.0.0"), 0}};
  CHECK_FALSE(sp::net::on_link(ip("8.8.8.8"), any));
  // This machine's own networks include loopback.
  const auto mine = sp::net::local_subnets();
  CHECK(std::any_of(mine.begin(), mine.end(), [&](const auto& n) { return sp::net::on_link(ip("127.0.0.1"), {&n, 1}); }));
}

// ---- Linux's text sources, parsed on every OS ---------------------------------------------------

TEST_CASE("driftwire: Linux's Downloads folder is read from user-dirs.dirs as xdg-user-dirs reads it",
          "[driftwire]") {
  const auto dl = [](const std::string& text, const char* home = "/home/ann") {
    return sp::net::parse_user_dirs_download(text, home);
  };
  // As xdg-user-dirs-update writes it, in the desktop's language.
  CHECK(dl("# This file is written by xdg-user-dirs-update\n"
           "XDG_DESKTOP_DIR=\"$HOME/Bureau\"\n"
           "XDG_DOWNLOAD_DIR=\"$HOME/T\xC3\xA9l\xC3\xA9" "chargements\"\n"
           "XDG_MUSIC_DIR=\"$HOME/Musique\"\n") == "/home/ann/T\xC3\xA9l\xC3\xA9" "chargements");
  CHECK(dl("XDG_DOWNLOAD_DIR=\"/srv/in box/\"\n") == "/srv/in box");                   // absolute
  CHECK(dl("  XDG_DOWNLOAD_DIR = \"$HOME/My \\\"Files\\\"\"\r\n") == "/home/ann/My \"Files\"");  // escapes
  CHECK(dl("XDG_DOWNLOAD_DIR=\"$HOME/a\"\nXDG_DOWNLOAD_DIR=\"$HOME/b\"") == "/home/ann/b");      // the last wins
  CHECK(dl("XDG_DOWNLOAD_DIR=\"$HOME/\"\n") == "/home/ann");  // "disabled": the home folder itself
  // Anything else is not a form it writes, and is passed over.
  for (const char* bad : {"", "XDG_DOWNLOAD_DIR=\"Downloads\"", "XDG_DOWNLOAD_DIR=$HOME/Downloads",
                          "XDG_DOWNLOAD_DIR=\"$HOME/open", "# XDG_DOWNLOAD_DIR=\"/x\"", "XDG_DOWNLOAD_DIRS=\"/x\"",
                          "XDG_DESKTOP_DIR=\"/x\"", "XDG_DOWNLOAD_DIR=\"$HOMEX/y\"", "XDG_DOWNLOAD_DIR=\"$HOME\""}) {
    INFO(bad);
    CHECK(dl(bad).empty());
  }
  CHECK(dl("XDG_DOWNLOAD_DIR=\"$HOME/x\"", "").empty());  // no home to put for $HOME
  CHECK(dl("XDG_DOWNLOAD_DIR=\"/x\"", "") == "/x");
#if defined(__linux__)
  // This machine's: the folder $XDG_CONFIG_HOME/user-dirs.dirs names, else ~/Downloads.
  scratch_dir s("driftwire_xdg");
  const char* was = std::getenv("XDG_CONFIG_HOME");
  const std::optional<std::string> saved = was ? std::optional<std::string>(was) : std::nullopt;
  setenv("XDG_CONFIG_HOME", utf8(s.root()).c_str(), 1);
  write_text(s / "user-dirs.dirs", "XDG_DOWNLOAD_DIR=\"/srv/drop\"\n");
  CHECK(sp::net::downloads_folder() == "/srv/drop");
  write_text(s / "user-dirs.dirs", "XDG_DOWNLOAD_DIR=\"Downloads\"\n");  // not a form it reads
  const std::string fallback = sp::net::downloads_folder();
  CHECK(fallback.size() > 10);
  CHECK(fallback.substr(fallback.size() - 10) == "/Downloads");
  if (saved) {
    setenv("XDG_CONFIG_HOME", saved->c_str(), 1);
  } else {
    unsetenv("XDG_CONFIG_HOME");
  }
#endif
}

TEST_CASE("driftwire: Linux's machine name is the pretty host name, unquoted as os-release(5) says", "[driftwire]") {
  const auto pretty = [](const std::string& text) { return sp::net::parse_pretty_hostname(text); };
  CHECK(pretty("PRETTY_HOSTNAME=\"Tristan's ThinkPad\"\n") == "Tristan's ThinkPad");  // as hostnamed writes it
  CHECK(pretty("ICON_NAME=computer-laptop\nCHASSIS=laptop\nPRETTY_HOSTNAME=Bea\\'s\\ Desk\n") == "Bea's Desk");
  CHECK(pretty("PRETTY_HOSTNAME=\"say \\\"hi\\\" \\$5 \\` \\\\ \\n\"") == "say \"hi\" $5 ` \\ \\n");  // \n stays
  CHECK(pretty("PRETTY_HOSTNAME='a \"b\" \\c'") == "a \"b\" \\c");  // single quotes take nothing
  CHECK(pretty("  PRETTY_HOSTNAME = \"Spaced\"  \r\n") == "Spaced");
  CHECK(pretty("PRETTY_HOSTNAME=\"Old\"\nPRETTY_HOSTNAME=\"New\"\n") == "New");  // the last wins
  CHECK(pretty("PRETTY_HOSTNAME=\"Caf\xC3\xA9\"") == "Caf\xC3\xA9");
  for (const char* none : {"", "# PRETTY_HOSTNAME=\"x\"", "; PRETTY_HOSTNAME=x", "PRETTY_HOSTNAME=\"open",
                           "HOSTNAME=x", "PRETTY_HOSTNAMES=x", "PRETTY_HOSTNAME="}) {
    INFO(none);
    CHECK(pretty(none).empty());
  }
}

TEST_CASE("driftwire: Linux's default gateway is the up route through a gateway with the lowest metric",
          "[driftwire]") {
  const auto ip = [](const char* text) { return *sp::net::parse_address(text); };
  // As the kernel prints /proc/net/route: each address the hex of its
  // network-order word read as a native integer, so on any host.
  const auto word = [&](const char* dotted) {
    const auto a = ip(dotted);
    std::uint32_t v = 0;
    std::memcpy(&v, a.bytes.data(), 4);
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08X", static_cast<unsigned>(v));
    return std::string(hex);
  };
  const auto row = [&](const char* iface, const char* dest, const char* gw, const char* flags, int metric,
                       const char* mask) {
    return std::string(iface) + "\t" + word(dest) + "\t" + word(gw) + "\t" + flags + "\t0\t0\t" +
           std::to_string(metric) + "\t" + word(mask) + "\t0\t0\t0\n";
  };
  const std::string head = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";
  const auto gateway = [](const std::string& table) {
    const auto g = sp::net::parse_default_route(table);
    return g ? sp::net::address_text(*g) : std::string("none");
  };
  const std::string table = head + row("wg0", "0.0.0.0", "0.0.0.0", "0001", 0, "0.0.0.0") +  // a device route
                            row("eth1", "0.0.0.0", "172.16.0.1", "0002", 0, "0.0.0.0") +    // down
                            row("tun0", "0.0.0.0", "10.9.0.1", "0003", 0, "128.0.0.0") +    // half the internet
                            row("eth0", "0.0.0.0", "192.168.1.1", "0003", 600, "0.0.0.0") +
                            row("wlan0", "0.0.0.0", "10.0.0.1", "0003", 100, "0.0.0.0") +
                            row("wlan1", "0.0.0.0", "10.0.0.2", "0003", 100, "0.0.0.0") +   // equal: the first
                            row("eth0", "192.168.1.0", "0.0.0.0", "0001", 600, "255.255.255.0");
  CHECK(gateway(table) == "10.0.0.1");
  CHECK(gateway(head + row("eth0", "0.0.0.0", "192.168.1.1", "0003", 600, "0.0.0.0")) == "192.168.1.1");
  CHECK(gateway(head + row("wg0", "0.0.0.0", "0.0.0.0", "0001", 0, "0.0.0.0")) == "none");
  CHECK(gateway(head) == "none");
  CHECK(gateway("") == "none");
  CHECK(gateway(head + "eth0\t00000000\tnot hex\t0003\t0\t0\t0\t00000000\n") == "none");
  if constexpr (std::endian::native == std::endian::little) {
    // This container's own table, verbatim: 192.0.2.1 prints as 010200C0.
    CHECK(gateway(head + "eth0\t00000000\t010200C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n") == "192.0.2.1");
  }
#if defined(__linux__)
  // This machine's own: a gateway that is IPv4, or none (a container may have none).
  if (const auto g = sp::net::default_gateway()) CHECK_FALSE(g->v6);
#endif
}

// ---- #530: staying awake ---------------------------------------------------------------------------

TEST_CASE("driftwire: a transfer's stay-awake request names who holds it, and is let go", "[driftwire]") {
  CHECK(std::string(sp::power::owner()) == "MediaViewer");
  CHECK(sp::power::reason(true) == "MediaViewer is receiving files (Driftwire)");
  CHECK(sp::power::reason(false) == "MediaViewer is sending files (Driftwire)");
  sp::power::set_owner("Driftwire");  // the command line
  CHECK(sp::power::reason(true) == "Driftwire is receiving files");
  CHECK(sp::power::reason(false) == "Driftwire is sending files");
  // The OS's own request, under a name of this run's: quick whether or not
  // the OS agrees (a container has no logind; a CI runner's polkit may say no).
  sp::power::set_owner("Driftwire test");
  const auto t0 = std::chrono::steady_clock::now();
  auto held = sp::power::hold(true);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
#if defined(__linux__)
  // logind lists it while it is held, and not once it is let go.
  const auto listed = [] {
    std::string out;
    if (FILE* p = ::popen("COLUMNS=500 systemd-inhibit --list --no-pager 2>/dev/null", "r")) {
      char buf[512];
      while (std::fgets(buf, sizeof buf, p)) out += buf;
      ::pclose(p);
    }
    return out.find("Driftwire test") != std::string::npos;
  };
  if (held) {
    CHECK(listed());
    held.reset();
    CHECK(eventually([&] { return !listed(); }, 5));
  } else {
    WARN("logind did not grant a sleep inhibitor here (no system bus, no logind, or polkit said no)");
  }
  held.reset();
  // A system bus that takes the connection and never answers (a wedged
  // dbus-daemon) does not hold the transfer up either: no request, at once.
  // (sd-bus alone would wait out its 90 s authentication timeout.)
  {
    scratch_dir s("dw_bus");
    const std::string path = utf8(s / "bus");
    sockaddr_un at{};
    at.sun_family = AF_UNIX;
    REQUIRE(path.size() < sizeof at.sun_path);
    std::memcpy(at.sun_path, path.c_str(), path.size() + 1);
    const int mute = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(mute >= 0);
    REQUIRE(::bind(mute, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0);
    REQUIRE(::listen(mute, 4) == 0);  // connects succeed; nothing is ever read or said
    const char* was = std::getenv("DBUS_SYSTEM_BUS_ADDRESS");
    const std::optional<std::string> saved = was ? std::optional<std::string>(was) : std::nullopt;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", ("unix:path=" + path).c_str(), 1);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK_FALSE(sp::power::hold(false));
    CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(5));
    if (saved) {
      setenv("DBUS_SYSTEM_BUS_ADDRESS", saved->c_str(), 1);
    } else {
      unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
    }
    ::close(mute);
  }
#endif
  held.reset();
  sp::power::set_owner(nullptr);  // the default again
  CHECK(std::string(sp::power::owner()) == "MediaViewer");
}

TEST_CASE("driftwire: a source is held to a few a minute, others are not (#502)", "[driftwire]") {
  using clk = sp::source_limit::clock;
  sp::source_limit limit(3, std::chrono::minutes(1), 4);
  const auto t0 = clk::now();
  for (int i = 0; i < 3; ++i) CHECK(limit.allow("10.0.0.9", t0 + std::chrono::seconds(i)));
  CHECK_FALSE(limit.allow("10.0.0.9", t0 + std::chrono::seconds(10)));
  CHECK(limit.allow("10.0.0.10", t0 + std::chrono::seconds(10)));  // another source
  CHECK(limit.allow("", t0));                                       // none: not limited
  CHECK(limit.allow("10.0.0.9", t0 + std::chrono::seconds(61)));   // a minute on
  // The table holds 4 sources; a fifth drops the one heard from longest ago.
  for (int i = 0; i < 8; ++i) (void)limit.allow("s" + std::to_string(i), t0 + std::chrono::seconds(70 + i));
  CHECK(limit.allow("10.0.0.9", t0 + std::chrono::seconds(80)));
}

namespace {
// A client whose engine options the test adjusts.
struct tuned_client {
  fake_host host;
  sp::engine eng;
  tuned_client(bus& b, const fs::path& received, const std::string& name,
               const std::function<void(sp::engine::options&)>& tune)
      : host(received), eng(host, [&] {
          auto o = client::options(b, name);
          tune(o);
          return o;
        }()) {
    eng.screen_opened("");
  }
};

// The engine's listening port, from its beacon on the bus. The id is read on
// each try: presence starts on the engine's worker and takes a new id as it
// does (#501), so one read before that would match no beacon.
std::uint16_t port_of_member(bus& b, const sp::engine& eng) {
  std::uint16_t port = 0;
  REQUIRE(eventually([&] {
    const std::string id = eng.id();
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) {
      if (m.self.id == id) port = m.self.port;
    }
    return port != 0;
  }));
  return port;
}

std::size_t shown_notices(fake_host& h) {
  std::lock_guard lock(h.m);
  return static_cast<std::size_t>(
      std::count_if(h.notices.begin(), h.notices.end(), [](const std::string& n) { return !n.empty(); }));
}
}  // namespace

TEST_CASE("driftwire: one source gets a few notifications a minute, then a few receives, then none (#502)",
          "[driftwire]") {
  scratch_dir s("driftwire_notice_flood");
  bus b;
  // Protocol 1: a sender's Cancel closes the link at once. (Protocol 2 may
  // take it for a drop and wait kRejoinMs, #511: this is about the limits.)
  b.force_protocol.store(sp::kProtocol1);
  client ann(b, s / "ann_received", "IT Support");
  client bea(b, s / "bea_received", "Bea");
  bea.host.notify = true;
  bea.eng.on_event("receiving", 2, "true");
  bea.eng.screen_closed();
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  // Connect, idle, give up, again: what a sender holding the slot all night
  // does. Past the notices it offers before giving up: five nearby links with
  // no offer would stop every nearby link for a while (#497), and this is
  // about the one source's own limit.
  for (int i = 0; i < sp::kReceivesPerMinute; ++i) {
    const bool offers = i >= sp::kNoticesPerMinute;
    ann.send_to(bea, offers);
    REQUIRE(eventually([&] {
      return bea.eng.snapshot().in == (offers ? sp::view::in_phase::asking : sp::view::in_phase::connecting);
    }));
    ann.eng.on_event("out_cancel", 1, "");
    ann.eng.wait_idle();
    bea.eng.wait_idle();
  }
  CHECK(shown_notices(bea.host) == static_cast<std::size_t>(sp::kNoticesPerMinute));
  {
    std::lock_guard lock(bea.host.m);
    CHECK(bea.host.shown == 0);  // nor was the window brought forward instead
  }
  // Past kReceivesPerMinute the link closes at once: no receive starts.
  ann.send_to(bea, false);
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::ended);  // the last one's end, nothing newer
  CHECK(shown_notices(bea.host) == static_cast<std::size_t>(sp::kNoticesPerMinute));
}

TEST_CASE("driftwire: a sender nobody confirms frees the receiver sooner (#502)", "[driftwire]") {
  scratch_dir s("driftwire_unconfirmed");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  tuned_client bea(b, s / "bea_received", "Bea", [](sp::engine::options& o) { o.confirm_ms = 1500; });
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return std::any_of(v.peers.begin(), v.peers.end(), [&](const auto& p) { return p.id == bea.eng.id(); });
  }));
  ann.eng.on_event("peers", 5, "\"" + bea.eng.id() + "\"");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::ended; }, 10));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 5000);
  CHECK(bea.eng.snapshot().in_note == "Ann did not send anything. Nothing was received.");
  ann.eng.on_event("out_cancel", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
}

TEST_CASE("driftwire: after Accept, a sender that sends nothing does not hold the receiver (#502)", "[driftwire]") {
  scratch_dir s("driftwire_idle_after_accept");
  bus b;
  tuned_client bea(b, s / "bea_received", "Bea", [](sp::engine::options& o) { o.receive_idle_ms = 1500; });
  fs::create_directories(s / "bea_received");
  // A sender of its own: an offer, then nothing (its pings keep the link up).
  auto l = sp::net::connect({"127.0.0.1", port_of_member(b, bea.eng)}, 5000);
  REQUIRE(l);
  auto ch = sp::channel::open(std::shared_ptr<sp::link>(std::move(*l)), true, "aaaaaaaaaaaaaaaa", "Mallory");
  REQUIRE(ch);
  const std::vector<sp::outgoing_file> files = {{"", "big.mov", 1ull << 30, 0, {}}};
  REQUIRE((*ch)->send(sp::msg::offer, sp::offer_json(files)));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::receiving; }));
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::ended; }, 15));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 6000);
  bea.eng.wait_idle();
  CHECK_FALSE(any_temporary(s / "bea_received"));
  (*ch)->shutdown();
}

TEST_CASE("driftwire: a beacon from off this machine's link is not listed (#502)", "[driftwire]") {
  scratch_dir s("driftwire_off_link");
  std::atomic<inject_discovery*> inject{nullptr};
  fake_host host(s / "received");
  sp::engine::options o;
  o.my_name = "Ann";
  o.use_bluetooth = false;
  o.loopback_only = true;
  o.local_subnets = std::vector<sp::net::subnet>{{*sp::net::parse_address("10.0.0.2"), 24}};
  o.make_discovery = [&inject] {
    auto d = std::make_unique<inject_discovery>();
    inject = d.get();
    return std::unique_ptr<sp::discovery>(std::move(d));
  };
  sp::engine eng(host, o);
  eng.screen_opened("");
  REQUIRE(eventually([&] {
    const inject_discovery* d = inject.load();
    return d && d->ready.load();
  }));
  for (int i = 0; i < 20; ++i) {
    inject.load()->seen(sp::beacon{"00000000000000a1", "Far away", 40000}, "203.0.113.7");
    inject.load()->seen(sp::beacon{"00000000000000a2", "Other subnet", 40000}, "10.0.1.7");
    inject.load()->seen(sp::beacon{"00000000000000b1", "Nearby", 40000}, "10.0.0.9");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(eventually([&] { return !eng.snapshot().peers.empty(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto v = eng.snapshot();
  REQUIRE(v.peers.size() == 1);
  CHECK(v.peers[0].name == "Nearby");
}

namespace {
// Reports one peer on "Bluetooth" a few times a second; it cannot connect.
struct fake_bluetooth final : sp::bluetooth {
  sp::bt_peer peer;
  std::atomic<bool> stop_{false};
  std::thread t;
  explicit fake_bluetooth(sp::bt_peer p) : peer(std::move(p)) {}
  ~fake_bluetooth() override { stop(); }
  bool start(const std::string&, const std::string&, peer_fn on_peer, link_fn, std::string&) override {
    stop_.store(false);
    t = std::thread([this, on_peer = std::move(on_peer)] {
      while (!stop_.load()) {
        on_peer(peer);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
    return true;
  }
  void stop() noexcept override {
    stop_.store(true);
    if (t.joinable()) t.join();
  }
  mv::result<std::unique_ptr<sp::link>> connect(const std::string&, int) override {
    return mv::err(mv::status::io);
  }
};

// Ann hears Bea only on Bluetooth until the test lets Bea's beacon through.
struct bt_only_sender {
  fake_host host;
  inject_discovery* inject = nullptr;
  sp::engine eng;
  bt_only_sender(const fs::path& received, const std::string& bea_id)
      : host(received), eng(host, options(bea_id)) {
    eng.screen_opened("");
  }
  sp::engine::options options(const std::string& bea_id) {
    sp::engine::options o;
    o.my_name = "Ann";
    o.loopback_only = true;
    o.make_discovery = [this] {
      auto d = std::make_unique<inject_discovery>();
      inject = d.get();
      return std::unique_ptr<sp::discovery>(std::move(d));
    };
    o.make_bluetooth = [bea_id] {
      return std::unique_ptr<sp::bluetooth>(std::make_unique<fake_bluetooth>(sp::bt_peer{"handle", bea_id, "Bea"}));
    };
    return o;
  }
};
}  // namespace

TEST_CASE("driftwire: a big set to a Bluetooth-only peer waits for it on Wi-Fi, then sends", "[driftwire]") {
  scratch_dir s("driftwire_wait_wifi");
  bus b;
  client bea(b, s / "bea_received", "Bea");
  std::uint16_t port = 0;
  REQUIRE(eventually([&] {
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) port = m.self.port;
    return port != 0;
  }));
  bt_only_sender ann(s / "ann_received", bea.eng.id());
  // 33 MiB: over Bluetooth's 32.
  const fs::path big = s / "out" / "clip.mov";
  fs::create_directories(big.parent_path());
  { std::ofstream(big, std::ios::binary).put('x'); }
  fs::resize_file(big, 33ull * 1024 * 1024);
  ann.eng.set_files({utf8(big)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty();
  }));
  CHECK(ann.eng.snapshot().peers[0].via == "Bluetooth");
  ann.eng.on_event("peers", 4, "[\"" + bea.eng.id() + "\"]");
  CHECK(sp::needs_wifi(ann.eng.snapshot()));
  ann.eng.on_event("send", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::waiting_wifi; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::waiting_wifi);  // nothing tried over Bluetooth
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::none);

  // Bea joins the network: her beacon arrives, and the send starts by itself.
  sp::beacon bea_beacon{bea.eng.id(), "Bea", port};
  ann.inject->seen(bea_beacon, "127.0.0.1");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::confirming; }));
  REQUIRE(eventually([&] { return !bea.eng.snapshot().in_code.empty(); }));
  ann.eng.on_event("out_code_entry", 3, "\"" + bea.eng.snapshot().in_code + "\"");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(fs::file_size(s / "bea_received" / "clip.mov") == 33ull * 1024 * 1024);
}

TEST_CASE("driftwire: Cancel ends a send that waits for Wi-Fi", "[driftwire]") {
  scratch_dir s("driftwire_wait_cancel");
  bt_only_sender ann(s / "ann_received", "0123456789abcdef");
  const fs::path big = s / "out" / "clip.mov";
  fs::create_directories(big.parent_path());
  { std::ofstream(big, std::ios::binary).put('x'); }
  fs::resize_file(big, 40ull * 1024 * 1024);
  ann.eng.set_files({utf8(big)});
  REQUIRE(eventually([&] {
    const auto v = ann.eng.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty();
  }));
  ann.eng.on_event("peers", 4, "[\"0123456789abcdef\"]");
  ann.eng.on_event("send", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::waiting_wifi; }));
  ann.eng.on_event("out_cancel", 1, "");
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(ann.eng.snapshot().out_note == "Cancelled. Nothing was sent.");
  // The peer appearing on Wi-Fi afterwards starts nothing.
  ann.inject->seen(sp::beacon{"0123456789abcdef", "Bea", 40000}, "127.0.0.1");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
}

// ---- harness: the idle cost of Keep receiving (#442) --------------------------------------------------
//
// mv_import_tests "[.driftwire-idle]": this process's CPU time while an
// engine sits with its window closed, Keep receiving off and then on (the
// real UDP beacon socket, the listener on loopback so the run opens no
// port to the network; Bluetooth is not in the test build). MV_IDLE_SECONDS
// sets each phase (default 20). Prints one JSON line.
TEST_CASE("driftwire: a link that offers nothing leaves the last transfer's Remember; the next offer takes it",
          "[driftwire]") {
  // #553: a knock, or a sender's rejoin as its send ends, cleared Remember
  // before anyone saw it (the command line's "Remember Ann?" never came).
  scratch_dir s("driftwire_remember_knock");
  bus b;
  client ann(b, s / "ann_received", "Ann", s / "ann_secrets");
  client bea(b, s / "bea_received", "Bea", s / "bea_secrets");
  client cy(b, s / "cy_received", "Cy");
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  cy.pick({s / "out" / "a.jpg"});
  bea.save_into(s / "bea_received");
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  REQUIRE(bea.eng.snapshot().remember_offer);
  REQUIRE(bea.eng.snapshot().remember_peer == "Ann");

  // Cy connects and offers nothing.
  cy.send_to(bea, false);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  CHECK(bea.eng.snapshot().remember_offer);
  cy.eng.on_event("out_cancel", 1, "");
  cy.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(bea.eng.snapshot().remember_offer);
  CHECK(bea.eng.snapshot().remember_peer == "Ann");

  // Cy's offer is the next transfer: Ann's Remember goes with it.
  cy.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  CHECK_FALSE(bea.eng.snapshot().remember_offer);
  bea.eng.on_event("decline", 1, "");
  cy.eng.wait_idle();
  bea.eng.wait_idle();
}

#if !defined(_WIN32)
namespace {
double cpu_seconds() {
  rusage u{};
  ::getrusage(RUSAGE_SELF, &u);
  return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
         static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}
long wakeups() {
  rusage u{};
  ::getrusage(RUSAGE_SELF, &u);
  return u.ru_nvcsw + u.ru_nivcsw;
}
}  // namespace

TEST_CASE("driftwire: a nearby receiver remembers the sender only with the code on the sender's screen (#497)",
          "[driftwire]") {
  scratch_dir s("driftwire_remember_check");
  bus b;
  client ann(b, s / "ann_received", "Ann", s / "ann_secrets");
  client bea(b, s / "bea_received", "Bea", s / "bea_secrets");
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  bea.save_into(s / "bea_received");
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  const auto av = ann.eng.snapshot();
  const auto bv = bea.eng.snapshot();
  REQUIRE(av.remember_offer);
  REQUIRE(bv.remember_offer);
  // Ann typed Bea's code: Bea is proved, and Ann's window shows its own check.
  CHECK(av.remember_check.size() == 7);
  CHECK_FALSE(av.remember_needs_check);
  // The window is posted after the transfer ends, not before wait_idle returns.
  CHECK(eventually([&] { return ann.tree().find(av.remember_check) != std::string::npos; }, 5));
  // Bea only saw a name Ann gave: ticking is not enough.
  CHECK(bv.remember_needs_check);
  CHECK(bv.remember_check.empty());
  CHECK(eventually([&] { return bea.tree().find("remember_check_entry") != std::string::npos; }, 5));
  bea.eng.on_event("remember", 2, "true");
  CHECK(bea.eng.snapshot().known.empty());
  const std::string wrong = av.remember_check == "000 000" ? "111 111" : "000 000";
  bea.eng.on_event("remember_check_entry", 3, "\"" + wrong + "\"");
  CHECK(bea.eng.snapshot().known.empty());
  CHECK(bea.eng.snapshot().remember_check_note.find("not the code") != std::string::npos);
  // The code on Ann's screen: Bea keeps Ann, and Ann keeps Bea with a tick.
  bea.eng.on_event("remember_check_entry", 3, "\"" + av.remember_check + "\"");
  CHECK(bea.eng.snapshot().known == std::vector<std::string>{"Ann"});
  CHECK(bea.eng.snapshot().remembered);
  ann.eng.on_event("remember", 2, "true");
  CHECK(ann.eng.snapshot().known == std::vector<std::string>{"Bea"});
}

TEST_CASE("driftwire: a run of nearby links that never offer stops new ones for a while (#497)", "[driftwire]") {
  scratch_dir s("driftwire_unconfirmed");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  REQUIRE(eventually([&] { return ann.sees(bea); }));
  std::uint16_t port = 0;
  {
    std::lock_guard lock(b.m);
    for (const auto& [k, m] : b.members) {
      if (m.self.id == bea.eng.id()) port = m.self.port;
    }
  }
  REQUIRE(port != 0);
  // A machine in the middle trying for a code: it connects, learns what it
  // can, and leaves before any offer. Five of those in a minute...
  for (int i = 0; i < 5; ++i) {
    auto c = sp::net::connect({"127.0.0.1", port}, 2000);
    REQUIRE(c);
    (*c)->shutdown();
    bea.eng.wait_idle();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bea.eng.wait_idle();
  }
  // ...and the next nearby sender is not answered, so no code shows there.
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  ann.send_to(bea, false);
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::ended; }, 15));
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::none);
}

TEST_CASE("driftwire: a peer that announces protocol 2 (v0.1.34, v0.1.35) is sent to in protocol 2 (#497)",
          "[driftwire]") {
  scratch_dir s("driftwire_protocol2_peer");
  bus b;
  b.force_protocol.store(sp::kProtocol2);
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  bea.save_into(s / "bea_received");
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  CHECK(ann.eng.snapshot().out_protocol == sp::kProtocol2);
  CHECK(bea.eng.snapshot().in_protocol == sp::kProtocol2);
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"a.jpg"});
}

TEST_CASE("driftwire: protocol 3's code is the same on both ends, and the server reveals last (#497)",
          "[driftwire]") {
  for (const std::uint32_t version : {sp::kProtocol2, sp::kProtocolVersion}) {
    INFO(version);
    auto [a, b] = loopback_pair();
    sp::channel::options o;
    o.version = version;
    mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
    std::thread t([&, l = std::move(b)]() mutable { server = sp::channel::open(std::move(l), false, "bbbbbbbbbbbbbbbb", "Bea"); });
    auto client = sp::channel::open(std::move(a), true, "aaaaaaaaaaaaaaaa", "Ann", o);
    t.join();
    REQUIRE(client);
    REQUIRE(server);
    CHECK((*client)->version() == version);
    CHECK((*server)->version() == version);
    CHECK((*client)->code() == (*server)->code());
    CHECK((*client)->code().size() == 7);
  }
}

TEST_CASE("driftwire: a beacon cannot talk a peer down to protocol 1 (#497)", "[driftwire]") {
  scratch_dir s("driftwire_no_downgrade");
  bus b;
  client ann(b, s / "ann_received", "Ann");
  client bea(b, s / "bea_received", "Bea");
  REQUIRE(eventually([&] { return ann.sees(bea); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(700));  // heard at protocol 2
  b.force_protocol.store(sp::kProtocol1);  // now every beacon in Bea's name says 1
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  write_text(s / "out" / "a.jpg", "a");
  ann.pick({s / "out" / "a.jpg"});
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  CHECK(ann.eng.snapshot().out_protocol == sp::kProtocolVersion);
  CHECK(bea.eng.snapshot().in_protocol == sp::kProtocolVersion);
  bea.eng.on_event("decline", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
}

TEST_CASE("driftwire: idle cost of Keep receiving", "[.driftwire-idle]") {
  const char* env = std::getenv("MV_IDLE_SECONDS");
  const int seconds = env ? std::max(1, std::atoi(env)) : 20;
  scratch_dir s("driftwire_idle_cost");
  fake_host host(s / "in");
  sp::engine::options o;
  o.my_name = "Idle";
  o.use_bluetooth = false;
  o.loopback_only = true;
  sp::engine e(host, o);  // the real UDP discovery
  auto phase = [&](const char* name) {
    const double c0 = cpu_seconds();
    const long w0 = wakeups();
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double cpu = cpu_seconds() - c0;
    std::printf("{\"phase\":\"%s\",\"seconds\":%.1f,\"cpu_ms\":%.1f,\"cpu_percent\":%.4f,\"switches_per_s\":%.1f}\n", name,
                wall, cpu * 1000, 100.0 * cpu / wall, static_cast<double>(wakeups() - w0) / wall);
  };
  phase("off");
  e.on_event("receiving", 2, "true");
  REQUIRE(eventually([&] { return e.snapshot().wifi_up; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  phase("on");
  e.on_event("receiving", 2, "false");
  REQUIRE(eventually([&] { return !e.snapshot().wifi_up; }));
  phase("off_again");
}
#endif

// ---- harness: a sender on this machine (#452) ------------------------------------------------------
//
// mv_import_tests "[.driftwire-send]": stands in for the second machine of
// #452's verify, on one computer. It sends MV_SEND_FILE to the first
// MediaViewer it hears on the real UDP beacon (another process: the app with
// Receive when MediaViewer is closed on), prints when the link is up (the
// receiver's notification is due within ~2 s of it), then either waits for
// the six digits the receiver's window shows in MV_SEND_CODE_FILE (written by
// whoever reads that window) and types them, or, without it, holds the link
// MV_SEND_HOLD seconds (default 10) and cancels. Prints JSON lines with
// wall-clock milliseconds. Nothing here listens beyond what the engine does.
TEST_CASE("driftwire: send to the MediaViewer nearby (harness)", "[.driftwire-send]") {
  const char* file = std::getenv("MV_SEND_FILE");
  REQUIRE(file);
  const auto wall_ms = [] {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
  };
  scratch_dir s("driftwire_send");
  fake_host host(s / "in");
  sp::engine::options o;
  o.my_name = "Harness sender";
  o.use_bluetooth = false;
  sp::engine e(host, o);
  e.screen_opened("");
  e.set_files({file});
  REQUIRE(eventually([&] {
    const auto v = e.snapshot();
    return !v.reading_folder && v.file_count == 1 && !v.peers.empty();
  }, 30));
  const auto peer = e.snapshot().peers.front();
  std::printf("{\"event\":\"tile_clicked\",\"peer\":\"%s\",\"ms\":%lld}\n", peer.name.c_str(), wall_ms());
  e.on_event("peers", 5, "\"" + peer.id + "\"");
  REQUIRE(eventually([&] { return e.snapshot().out == sp::view::out_phase::confirming; }, 15));
  std::printf("{\"event\":\"link_up\",\"ms\":%lld}\n", wall_ms());
  std::fflush(stdout);
  const char* code_file = std::getenv("MV_SEND_CODE_FILE");
  if (!code_file) {
    const char* hold = std::getenv("MV_SEND_HOLD");
    std::this_thread::sleep_for(std::chrono::seconds(hold ? std::max(1, std::atoi(hold)) : 10));
    e.on_event("out_cancel", 1, "");
    e.wait_idle();
    std::printf("{\"event\":\"cancelled\",\"ms\":%lld}\n", wall_ms());
    return;
  }
  std::string code;
  REQUIRE(eventually([&] {
    std::ifstream in(code_file);
    std::getline(in, code);
    return code.size() >= 6;
  }, 300));
  e.on_event("out_code_entry", 3, "\"" + code + "\"");
  std::printf("{\"event\":\"code_typed\",\"ms\":%lld}\n", wall_ms());
  std::fflush(stdout);
  REQUIRE(eventually([&] {
    const auto out = e.snapshot().out;
    return out == sp::view::out_phase::done || out == sp::view::out_phase::ended;
  }, 300));
  std::printf("{\"event\":\"%s\",\"note\":\"%s\",\"ms\":%lld}\n",
              e.snapshot().out == sp::view::out_phase::done ? "done" : "ended", e.snapshot().out_note.c_str(), wall_ms());
}

// ---- harness: transfer throughput (#442) -----------------------------------------------------------
//
// mv_import_tests "[.driftwire-bench]": MB/s of a Driftwire send between two
// engines in this process over loopback TCP, from Accept to the receiver's
// done (every file hashed, flushed, read back uncached and renamed, as on a
// real link), against a plain copy over the same kind of link (the bytes
// through a loopback socket into a new file: no hash, no seal, no flush, no
// read-back). Loopback is CPU- and disk-bound, not a Wi-Fi or Bluetooth link:
// it shows what the code costs, not what a radio allows.
//
//   MV_BENCH_DIR        where the fixture and the received files live
//                       (default: a scratch folder, removed afterwards);
//                       the fixture is kept between runs when given
//   MV_BENCH_BIG_MB     one big file (default 1024)
//   MV_BENCH_SMALL      count of small files (default 500)
//   MV_BENCH_SMALL_KB   each small file (default 2048)
//   MV_BENCH_CASES      "big", "small" or "big,small" (default both)
//   MV_BENCH_STAGES     0: no stage timers (#540), as the app runs; the
//                       stage lines are then not printed
// Prints one JSON line per case and mode.
namespace {

std::size_t env_size(const char* name, std::size_t fallback) {
  const char* v = std::getenv(name);
  return v && *v ? static_cast<std::size_t>(std::strtoull(v, nullptr, 10)) : fallback;
}

// Incompressible bytes, written once; kept when the size already matches.
void fixture_file(const fs::path& p, std::size_t bytes, std::uint32_t seed) {
  std::error_code ec;
  if (fs::exists(p, ec) && fs::file_size(p, ec) == bytes) return;
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  std::vector<char> block(1 << 20);
  std::uint64_t x = 0x9E3779B97F4A7C15ull ^ seed;
  std::size_t left = bytes;
  while (left > 0) {
    for (std::size_t i = 0; i + 8 <= block.size(); i += 8) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      std::memcpy(block.data() + i, &x, 8);
    }
    const std::size_t n = std::min(left, block.size());
    out.write(block.data(), static_cast<std::streamsize>(n));
    left -= n;
  }
}

void print_case(const char* name, const char* mode, std::uint64_t bytes, std::size_t files, double seconds) {
  std::printf("{\"case\":\"%s\",\"mode\":\"%s\",\"files\":%zu,\"bytes\":%llu,\"seconds\":%.3f,\"mb_s\":%.1f}\n", name,
              mode, files, static_cast<unsigned long long>(bytes), seconds,
              static_cast<double>(bytes) / 1e6 / seconds);
  std::fflush(stdout);
}

// The plain copy: each file's bytes through one loopback TCP connection into
// a new file, 1 MiB at a time.
double plain_copy(const std::vector<fs::path>& files, const fs::path& dest) {
  fs::create_directories(dest);
  auto l = sp::net::listen(true);
  REQUIRE(l);
  const auto t0 = std::chrono::steady_clock::now();
  bool ok = true;
  std::thread rx([&] {
    auto a = (*l)->accept(5000);
    if (!a) {
      ok = false;
      return;
    }
    std::vector<std::uint8_t> buf(1 << 20);
    for (const auto& f : files) {
      std::uint64_t left = fs::file_size(f);
      mv::io::file_writer out;
      if (!out.create_new(utf8(dest / f.filename()))) {
        ok = false;
        return;
      }
      while (left > 0) {
        auto n = (*a)->read_some({buf.data(), static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()))}, 20000);
        if (!n || *n == 0 || !out.write({buf.data(), *n})) {
          ok = false;
          return;
        }
        left -= *n;
      }
      (void)out.close();
    }
  });
  auto c = sp::net::connect({"127.0.0.1", (*l)->port()}, 5000);
  REQUIRE(c);
  std::vector<std::uint8_t> buf(1 << 20);
  for (const auto& f : files) {
    mv::io::file_reader in;
    REQUIRE(in.open(utf8(f), mv::io::read_mode::sequential));
    for (;;) {
      auto n = in.read(buf);
      REQUIRE(n);
      if (*n == 0) break;
      REQUIRE((*c)->write_all({buf.data(), *n}));
    }
  }
  rx.join();
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ok);
  return s;
}

// A Driftwire send of `files` from Ann to Bea, timed from Accept to done;
// each side's stages (#540) into `tx` and `rx`.
double driftwire_send(const std::vector<fs::path>& files, const fs::path& dest, const fs::path& work,
                      sp::stage_times& tx, sp::stage_times& rx) {
  bus b;
  const bool timed = env_size("MV_BENCH_STAGES", 1) != 0;
  client ann(b, work / "ann_in", "Ann", {}, {}, [&](sp::engine::options& o) { o.send_stages = timed ? &tx : nullptr; });
  client bea(b, work / "bea_in", "Bea", {}, {},
             [&](sp::engine::options& o) { o.receive_stages = timed ? &rx : nullptr; });
  fs::create_directories(dest);
  ann.pick(files);
  bea.save_into(dest);
  ann.send_to(bea);
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }, 60));
  tx.reset();  // the offer's frames are not the transfer's
  rx.reset();
  const auto t0 = std::chrono::steady_clock::now();
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  INFO(bea.eng.snapshot().in_note);
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::done);
  return s;
}

}  // namespace

TEST_CASE("driftwire: transfer throughput against a plain copy", "[.driftwire-bench]") {
  const char* dir_env = std::getenv("MV_BENCH_DIR");
  std::optional<scratch_dir> scratch;
  fs::path base;
  if (dir_env && *dir_env) {
    base = dir_env;
  } else {
    scratch.emplace("driftwire_bench");
    base = scratch->root();
  }
  const std::size_t big_mb = env_size("MV_BENCH_BIG_MB", 1024);
  const std::size_t small_n = env_size("MV_BENCH_SMALL", 500);
  const std::size_t small_kb = env_size("MV_BENCH_SMALL_KB", 2048);
  const char* cases_env = std::getenv("MV_BENCH_CASES");
  const std::string cases = cases_env && *cases_env ? cases_env : "big,small";

  struct bench_case {
    const char* name;
    std::vector<fs::path> files;
  };
  std::vector<bench_case> runs;
  if (cases.find("big") != std::string::npos) {
    const fs::path f = base / "fixture" / "big" / "clip.mov";
    fixture_file(f, big_mb << 20, 1);
    runs.push_back({"big", {f}});
  }
  if (cases.find("small") != std::string::npos) {
    bench_case c{"small", {}};
    for (std::size_t i = 0; i < small_n; ++i) {
      char name[32];
      std::snprintf(name, sizeof name, "IMG_%04zu.JPG", i);
      const fs::path f = base / "fixture" / "small" / name;
      fixture_file(f, small_kb << 10, static_cast<std::uint32_t>(100 + i));
      c.files.push_back(f);
    }
    runs.push_back(std::move(c));
  }
  for (const auto& c : runs) {
    std::uint64_t bytes = 0;
    for (const auto& f : c.files) bytes += fs::file_size(f);
    const fs::path out = base / "out";
    fs::remove_all(out);
    print_case(c.name, "plain", bytes, c.files.size(), plain_copy(c.files, out / "plain"));
    fs::remove_all(out);
    sp::stage_times tx, rx;
    const double s = driftwire_send(c.files, out / "driftwire", out / "work", tx, rx);
    print_case(c.name, "driftwire", bytes, c.files.size(), s);
    // #540: where each side's time went (stages.h): "share" is the stage's
    // thread time over the wall time; the receiver's verifiers are two threads.
    if (env_size("MV_BENCH_STAGES", 1) != 0) std::printf("{\"case\":\"%s\",\"mode\":\"driftwire\",\"side\":\"send\",\"stages\":%s}\n", c.name,
                tx.json(s).c_str());
    if (env_size("MV_BENCH_STAGES", 1) != 0) std::printf("{\"case\":\"%s\",\"mode\":\"driftwire\",\"side\":\"receive\",\"stages\":%s}\n", c.name,
                rx.json(s).c_str());
    std::fflush(stdout);
    fs::remove_all(out);
  }
}
