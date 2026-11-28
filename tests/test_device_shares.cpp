// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Each paired computer's own account on the host's LAN share (wire/device_shares.h,
// self-host-users-smb.md §12): its name, the store, when the step must run, and what its
// pairing lets it do. Pure; no account is made.
#include "catch_compat.h"

#include <sodium.h>

#include <string>
#include <vector>

#include "wire/device_shares.h"
#include "wire/share_users.h"

namespace ds = mv::driftwire::serve::device_shares;
namespace grants = mv::driftwire::serve::grants;
namespace su = mv::driftwire::serve::share_users;

TEST_CASE("device shares: one account per computer, kept and read back, never twice", "[device-shares]") {
  REQUIRE(sodium_init() >= 0);
  ds::key32 a{}, b{};
  a.fill(0xab);
  b[0] = 0x01;
  CHECK(ds::account_for(a) == "mv-abababab");
  CHECK(ds::account_for(b) == "mv-01000000");
  CHECK(su::valid_name(ds::account_for(a)));
  std::vector<ds::desk> list;
  CHECK(ds::add(list, a));
  CHECK_FALSE(ds::add(list, a));  // already there
  CHECK(ds::add(list, b));
  list[0].password = ds::new_password();
  list[0].role = "change";
  list[0].folders = std::vector<std::string>{"Photos"};
  list[0].applied = 10;
  const std::string text = ds::encode(list);
  auto back = ds::decode(text);
  REQUIRE(back);
  REQUIRE(back->size() == 2);
  CHECK((*back)[0].password == list[0].password);
  CHECK((*back)[0].role == "change");
  CHECK(*(*back)[0].folders == std::vector<std::string>{"Photos"});
  CHECK((*back)[1].password.empty());
  // A list that names the wrong account for a key, or a key twice, is refused whole.
  std::string wrong = text;
  wrong.replace(wrong.find("mv-abababab"), 11, "mv-00000000");
  CHECK_FALSE(ds::decode(wrong));
  CHECK_FALSE(ds::decode(R"({"v":1,"desks":[{"key":")" + std::string(64, 'a') + R"(","account":"mv-aaaaaaaa"},{"key":")" +
                         std::string(64, 'a') + R"(","account":"mv-aaaaaaaa"}]})"));
  // A password is 32 characters nobody types, and never the same twice.
  const auto p1 = ds::new_password(), p2 = ds::new_password();
  CHECK(p1.size() == 32);
  CHECK(p1 != p2);
}

TEST_CASE("device shares: unpairing takes a made account away, and a never-made one at once", "[device-shares]") {
  ds::key32 a{}, b{};
  a.fill(1);
  b.fill(2);
  std::vector<ds::desk> list;
  (void)ds::add(list, a);
  (void)ds::add(list, b);
  list[0].applied = 5;
  list[0].password = "x";
  CHECK(ds::remove(list, a));
  REQUIRE(ds::find(list, a));
  CHECK(ds::find(list, a)->gone);  // the step takes its account away first
  CHECK(ds::remove(list, b));
  CHECK_FALSE(ds::find(list, b));  // never made: nothing to take away
  CHECK(ds::add(list, a));  // paired again before the step ran: kept
  CHECK_FALSE(ds::find(list, a)->gone);
}

TEST_CASE("device shares: the step runs for a new, gone or changed computer only", "[device-shares]") {
  ds::desk d;
  CHECK(ds::waiting(d, "view", std::nullopt));  // never made
  d.applied = 1;
  d.password = "p";
  d.role = "view";
  CHECK_FALSE(ds::waiting(d, "view", std::nullopt));
  CHECK(ds::waiting(d, "change", std::nullopt));
  CHECK(ds::waiting(d, "view", std::vector<std::string>{"Photos"}));
  d.folders = std::vector<std::string>{"Videos", "Photos"};
  CHECK_FALSE(ds::waiting(d, "view", std::vector<std::string>{"Photos", "Videos"}));  // order is not a change
  d.gone = true;
  CHECK(ds::waiting(d, "view", d.folders));
}

TEST_CASE("device shares: a computer's role and folders are its pairing's", "[device-shares]") {
  const std::vector<mv::driftwire::serve::root_config> roots = {{"a1a1a1a1a1a1a1a1", "Photos", "/p"},
                                                               {"b2b2b2b2b2b2b2b2", "Videos", "/v"}};
  CHECK(ds::want_of(nullptr, roots).role == "change");  // unlimited
  CHECK_FALSE(ds::want_of(nullptr, roots).folders);
  grants::grant g;
  g.backup = false;  // a desktop as it pairs: can view files
  CHECK(ds::want_of(&g, roots).role == "view");
  g.backup = true;
  g.roots = std::vector<std::string>{"b2b2b2b2b2b2b2b2"};
  const auto w = ds::want_of(&g, roots);
  CHECK(w.role == "change");
  REQUIRE(w.folders);
  CHECK(*w.folders == std::vector<std::string>{"Videos"});
}

// self-host-users-smb.md §13 (owner, 2026-11-28): people own devices; a device has its person's role.
#include "wire/people.h"
namespace people = mv::driftwire::serve::people;

TEST_CASE("people: a person has a role and devices; a device belongs to one person at most", "[device-shares]") {
  std::vector<people::person> folk;
  CHECK(people::set(folk, "  Tristan ", "change").empty());
  CHECK(people::set(folk, "tristan", "view").empty());  // the same person, by name without case
  REQUIRE(folk.size() == 1);
  CHECK(folk[0].name == "Tristan");
  CHECK(folk[0].role == "view");
  CHECK(people::set(folk, "", "view") == "bad-name");
  CHECK(people::set(folk, "Anna", "admin") == "bad-role");
  CHECK(people::set(folk, "Anna", "change").empty());
  people::key32 phone{}, mac{};
  phone.fill(1);
  mac.fill(2);
  CHECK(people::link(folk, phone, "Tristan").empty());
  CHECK(people::link(folk, mac, "Tristan").empty());
  CHECK(people::link(folk, mac, "Nobody here") == "not-found");
  REQUIRE(people::owner_of(folk, mac));
  CHECK(people::owner_of(folk, mac)->name == "Tristan");
  // Moved to Anna: taken from Tristan.
  CHECK(people::link(folk, mac, "Anna").empty());
  CHECK(people::owner_of(folk, mac)->name == "Anna");
  CHECK(people::find(folk, "Tristan")->devices.size() == 1);
  // Read back whole; a device on two people is refused whole.
  auto back = people::decode(people::encode(folk));
  REQUIRE(back);
  CHECK(back->size() == 2);
  CHECK(people::owner_of(*back, phone)->name == "Tristan");
  const std::string k(64, 'a');
  CHECK_FALSE(people::decode(R"({"v":1,"people":[{"name":"A","role":"view","devices":[")" + k +
                             R"("]},{"name":"B","role":"view","devices":[")" + k + R"("]}]})"));
  CHECK_FALSE(people::decode(R"({"v":1,"people":[{"name":"A","role":"view","devices":[]},{"name":"a","role":"view","devices":[]}]})"));
  // To nobody, then unpaired, then the person removed: devices go free, never with them.
  CHECK(people::link(folk, phone, "").empty());
  CHECK_FALSE(people::owner_of(folk, phone));
  CHECK(people::forget_device(folk, mac));
  CHECK_FALSE(people::forget_device(folk, mac));
  CHECK(people::remove(folk, "ANNA"));
  CHECK(folk.size() == 1);
}

TEST_CASE("device shares: two computers never share one account", "[device-shares]") {
  // account_for is the key's first 4 bytes: a second key that starts the same gets no account
  // of its own, so it can never be given the first one's rights or take its login away.
  ds::key32 a{}, b{};
  a.fill(0x42);
  b.fill(0x42);
  b[31] = 0x01;
  REQUIRE(ds::account_for(a) == ds::account_for(b));
  std::vector<ds::desk> list;
  CHECK(ds::add(list, a));
  CHECK_FALSE(ds::add(list, b));
  REQUIRE(list.size() == 1);
  CHECK(list[0].key == a);
  CHECK(ds::find(list, b) == nullptr);
  // Unpairing the one without an account leaves the other's alone.
  CHECK_FALSE(ds::remove(list, b));
  CHECK_FALSE(list[0].gone);
}
