// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A paired server's LAN SMB share in the Finder (remote/lan_share.h,
// docs/plans/desktop-client.md §6a): when to mount, with what URL, what is
// already mounted, and what the notice says. Pure; no network, no mount.
#include "catch_compat.h"

#include <string>
#include <vector>

#include "remote/lan_share.h"

namespace ls = mv::remote::lan_share;

TEST_CASE("lan share: only a LAN address is a host to mount from", "[lan-share]") {
  CHECK(ls::lan_host("192.168.0.79:7878") == "192.168.0.79");
  CHECK(ls::lan_host("10.0.0.2:7878") == "10.0.0.2");
  CHECK(ls::lan_host("172.16.4.1:7878") == "172.16.4.1");
  CHECK(ls::lan_host("172.31.255.254:1") == "172.31.255.254");
  CHECK(ls::lan_host("169.254.10.3:7878") == "169.254.10.3");
  CHECK(ls::lan_host("Studio.local:7878") == "studio.local");
  CHECK(ls::lan_host("[fd12:3456::1]:7878") == "fd12:3456::1");
  // Public addresses and names, loopback, and what is not an address at all: never.
  CHECK(ls::lan_host("8.8.8.8:7878").empty());
  CHECK(ls::lan_host("172.32.0.1:7878").empty());
  CHECK(ls::lan_host("172.15.0.1:7878").empty());
  CHECK(ls::lan_host("127.0.0.1:7878").empty());
  CHECK(ls::lan_host("localhost:7878").empty());
  CHECK(ls::lan_host("home.duckdns.org:7878").empty());
  CHECK(ls::lan_host("[2001:db8::1]:7878").empty());
  CHECK(ls::lan_host("[fe80::1%en0]:7878").empty());
  CHECK(ls::lan_host("192.168.0.256:7878").empty());
  CHECK(ls::lan_host("192.168.0:7878").empty());
  CHECK(ls::lan_host("evil.local.example.com:7878").empty());
  CHECK(ls::lan_host("a b.local:7878").empty());
  CHECK(ls::lan_host("").empty());
}

TEST_CASE("lan share: a share's URL is the host's share name for the folder's label", "[lan-share]") {
  CHECK(ls::share_url("192.168.0.79", "Photos") == "smb://192.168.0.79/Photos");
  // The host names the share share_users::share_name(label): no ':' and the like, then encoded.
  CHECK(ls::share_url("192.168.0.79", "Family: Photos") == "smb://192.168.0.79/Family%20Photos");
  CHECK(ls::share_url("studio.local", "My Photos") == "smb://studio.local/My%20Photos");
  CHECK(ls::share_url("fd12::1", "Photos") == "smb://[fd12::1]/Photos");
  CHECK(ls::share_url("", "Photos").empty());
  CHECK(ls::share_url("192.168.0.79", "").empty());
}

TEST_CASE("lan share: what macOS says a volume was mounted from", "[lan-share]") {
  auto a = ls::parse_mounted_from("//tristan@192.168.0.79/Photos");
  REQUIRE(a);
  CHECK(a->host == "192.168.0.79");
  CHECK(a->share == "Photos");
  auto b = ls::parse_mounted_from("//WORKGROUP;Anna@Studio.local/My%20Photos");
  REQUIRE(b);
  CHECK(b->host == "studio.local");
  CHECK(b->share == "My Photos");
  auto c = ls::parse_mounted_from("//GUEST:@192.168.0.79:445/Videos/2024");
  REQUIRE(c);
  CHECK(c->host == "192.168.0.79");
  CHECK(c->share == "Videos");
  auto d = ls::parse_mounted_from("//[fd12::1]/Photos");
  REQUIRE(d);
  CHECK(d->host == "fd12::1");
  CHECK_FALSE(ls::parse_mounted_from("/dev/disk3s1"));
  CHECK_FALSE(ls::parse_mounted_from("//192.168.0.79"));
  CHECK_FALSE(ls::parse_mounted_from("//192.168.0.79/"));
}

TEST_CASE("lan share: mounts each named folder's share at home, none away", "[lan-share]") {
  ls::input in;
  in.address = "192.168.0.79:7878";
  in.labels = {"Photos", "Videos"};
  auto p = ls::plan_mounts(in);
  CHECK(p.host == "192.168.0.79");
  CHECK(p.urls == std::vector<std::string>{"smb://192.168.0.79/Photos", "smb://192.168.0.79/Videos"});
  CHECK(p.why == ls::skip::none);

  SECTION("never over the internet or the relay, even to a LAN-looking address") {
    in.over_internet = true;
    CHECK(ls::plan_mounts(in).urls.empty());
    CHECK(ls::plan_mounts(in).why == ls::skip::away);
    in.over_internet = false;
    in.relayed = true;
    CHECK(ls::plan_mounts(in).why == ls::skip::away);
  }
  SECTION("never to a public address that answered as the local route") {
    in.address = "81.2.69.160:7878";
    CHECK(ls::plan_mounts(in).why == ls::skip::away);
    in.address.clear();  // not connected this run
    CHECK(ls::plan_mounts(in).why == ls::skip::away);
  }
  SECTION("off in Settings") {
    in.enabled = false;
    CHECK(ls::plan_mounts(in).urls.empty());
    CHECK(ls::plan_mounts(in).why == ls::skip::off);
  }
  SECTION("a share already mounted is skipped, matched without case") {
    in.already = {{"192.168.0.79", "photos"}, {"192.168.0.80", "Videos"}};
    p = ls::plan_mounts(in);
    CHECK(p.urls == std::vector<std::string>{"smb://192.168.0.79/Videos"});
    in.already.push_back({"192.168.0.79", "VIDEOS"});
    p = ls::plan_mounts(in);
    CHECK(p.urls.empty());
    CHECK(p.why == ls::skip::all_mounted);
  }
  SECTION("two labels with one share name mount once; no folders, nothing") {
    in.labels = {"Photos", "Photos:", "photos"};
    CHECK(ls::plan_mounts(in).urls == std::vector<std::string>{"smb://192.168.0.79/Photos"});
    in.labels.clear();
    CHECK(ls::plan_mounts(in).why == ls::skip::no_folders);
  }
}

TEST_CASE("lan share: unmount takes only that host's volumes", "[lan-share]") {
  const std::vector<ls::mounted> all = {{"192.168.0.79", "Photos"}, {"192.168.0.7", "Work"}, {"192.168.0.79", "Videos"}};
  const auto mine = ls::mounted_from("192.168.0.79", all);
  REQUIRE(mine.size() == 2);
  CHECK(mine[0].share == "Photos");
  CHECK(mine[1].share == "Videos");
  CHECK(ls::mounted_from("", all).empty());
}

TEST_CASE("lan share: the notice is quiet unless something happened", "[lan-share]") {
  CHECK(ls::notice_words("FamilyNas", {}).empty());
  CHECK(ls::notice_words("FamilyNas", {2, 0, false, false}) == "FamilyNas's shares are in the Finder.");
  CHECK(ls::notice_words("FamilyNas", {1, 0, false, true}) == "FamilyNas's share is in the Finder.");
  CHECK(ls::notice_words("FamilyNas", {1, 1, false, false}) == "FamilyNas's share is in the Finder. 1 other didn't mount.");
  // The automatic mount never shows the OS's dialog: it says where to sign in once.
  CHECK(ls::notice_words("FamilyNas", {0, 2, true, false}) ==
        "FamilyNas's shares need a sign-in once: File > Mount Server Shares in Finder.");
  CHECK(ls::notice_words("FamilyNas", {0, 2, true, true}) ==
        "Didn't mount FamilyNas's shares: the sign-in didn't go through.");
  CHECK(ls::notice_words("FamilyNas", {0, 1, false, false}) ==
        "Couldn't mount FamilyNas's shares in the Finder. File > Mount Server Shares in Finder tries again.");
  CHECK(ls::skip_words("FamilyNas", ls::skip::away) == "FamilyNas's shares mount only at home, on the same network.");
  CHECK(ls::skip_words("FamilyNas", ls::skip::off).empty());
}
