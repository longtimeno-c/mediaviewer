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
