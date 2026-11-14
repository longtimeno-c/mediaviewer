// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host drives` (docs/plans/self-host-drives.md): the five choices and
// their words, the erase guards, plans against stand-in disk lists read from
// both OSes' JSON, the typed names, the scripts each OS would run, health, the
// drives keeper and the phone's "storage" word. Nothing here touches a disk:
// the commands run against a stand-in port.
//
// [.host-drives-image] (hidden, run by hand on a Mac): a real AppleRAID mirror
// on two hdiutil images made for it, one detached, then replaced. Only disk
// images under the test's own folder can be touched (the guard), and they are
// detached and deleted after.
#include "catch_compat.h"

#include <atomic>
#include <functional>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/drives.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/host_drives.h"
#include "core/json.h"
#include "import_fixture.h"
#include "wire/serve_wire.h"

using namespace mv::test;
namespace cli = mv::driftwire::cli;
namespace drives = mv::driftwire::cli::drives;
namespace serve = mv::driftwire::serve;
namespace json = mv::json;

namespace {

constexpr std::uint64_t TB = 1'000'000'000'000ull;
constexpr std::uint64_t GB = 1'000'000'000ull;

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

json::value parsed(const std::string& text) {
  auto v = json::parse(text, 32);
  REQUIRE(v);
  return *v;
}

drives::disk image_disk(const std::string& id, const std::string& label, std::uint64_t size, const std::string& image) {
  drives::disk d;
  d.id = id;
  d.model = "Disk Image";
  d.identity = "id-" + id;
  d.size = size;
  d.image = true;
  d.image_path = image;
  if (!label.empty()) d.volumes.push_back({"", label, size, 0, 0, false});
  return d;
}

// The Mac's `diskutil list -plist` for: disk0 (internal, APFS, the system's), disk4
// (an external drive with one HFS+ volume, "Photos"), disk5 (an external, unformatted).
const char* kMacList = R"({
  "AllDisksAndPartitions": [
    {"DeviceIdentifier": "disk0", "Partitions": [
      {"DeviceIdentifier": "disk0s1", "Content": "Apple_APFS_ISC", "Size": 524288000},
      {"DeviceIdentifier": "disk0s2", "Content": "Apple_APFS", "Size": 994662584320}]},
    {"DeviceIdentifier": "disk3", "APFSPhysicalStores": [{"DeviceIdentifier": "disk0s2"}], "APFSVolumes": [
      {"DeviceIdentifier": "disk3s1", "MountPoint": "/", "VolumeName": "Macintosh HD", "Size": 994662584320, "CapacityInUse": 11000000000, "Roles": ["System"]},
      {"DeviceIdentifier": "disk3s5", "MountPoint": "/System/Volumes/Data", "VolumeName": "Data", "CapacityInUse": 400000000000, "Roles": ["Data"]}]},
    {"DeviceIdentifier": "disk4", "Partitions": [
      {"DeviceIdentifier": "disk4s1", "Content": "EFI", "VolumeName": "EFI", "Size": 209715200},
      {"DeviceIdentifier": "disk4s2", "Content": "Apple_HFS", "VolumeName": "Photos", "MountPoint": "/Volumes/Photos", "Size": 1999000000000}]},
    {"DeviceIdentifier": "disk5", "Content": "", "Size": 2000398934016}
  ],
  "WholeDisks": ["disk0", "disk3", "disk4", "disk5"]
})";

std::string mac_info(const std::string& id, bool internal, const std::string& bus, std::uint64_t size,
                     const std::string& model, const char* virt = "Physical") {
  return std::string(R"({"DeviceIdentifier": ")") + id + R"(", "WholeDisk": true, "Internal": )" + (internal ? "true" : "false") +
         R"(, "BusProtocol": ")" + bus + R"(", "TotalSize": )" + std::to_string(size) + R"(, "MediaName": ")" + model +
         R"(", "VirtualOrPhysical": ")" + virt + R"(", "WritableMedia": true, "SMARTStatus": "Verified", "DeviceTreePath": "IODeviceTree:/x/)" +
         id + R"("})";
}

// What `diskutil appleRAID list -plist` said on 2026-11-14 for a mirror on two images:
// both online, then one detached (the set offline, one member with no BSD name).
const char* kRaidOnline = R"({"AppleRAIDSets": [{"Content": "Apple_HFS", "BSD Name": "disk14", "Name": "MVTestMirror",
  "Members": [{"AppleRAIDMemberUUID": "843D", "BSD Name": "disk12s1", "MemberStatus": "Online"},
              {"AppleRAIDMemberUUID": "EC20", "BSD Name": "disk13s1", "MemberStatus": "Online"}],
  "AppleRAIDSetUUID": "67D6", "Spares": [], "Rebuild": "Manual", "Status": "Online", "Size": 67043328, "Level": "Mirror"}]})";
const char* kRaidLostOne = R"({"AppleRAIDSets": [{"Content": "Apple_HFS", "Name": "MVTestMirror",
  "Members": [{"AppleRAIDMemberUUID": "843D", "BSD Name": "disk12s1", "MemberStatus": "Online"},
              {"AppleRAIDMemberUUID": "EC20"}],
  "AppleRAIDSetUUID": "67D6", "Status": "Offline", "Size": 67043328, "Level": "Mirror"}]})";

// The Windows listing script's JSON (drives_win.cpp): the system disk, two USB drives, one VHD;
// and a two-way mirror that lost one of its drives.
const char* kWindows = R"({"pool_ok": true, "disks": [
  {"Number": 0, "FriendlyName": "Samsung SSD 980", "UniqueId": "U0", "Size": 1000204886016, "IsSystem": true, "IsBoot": true,
   "IsReadOnly": false, "IsOffline": false, "BusType": "NVMe", "ImagePath": "", "InPool": false, "Health": "Healthy",
   "Volumes": [{"Path": "C:\\", "Label": "Windows", "Size": 999000000000, "Free": 500000000000, "Folders": 9, "Catalogue": false, "System": true}]},
  {"Number": 2, "FriendlyName": "WD Elements", "UniqueId": "U2", "Size": 4000787030016, "IsSystem": false, "IsBoot": false,
   "IsReadOnly": false, "IsOffline": false, "BusType": "USB", "ImagePath": "", "InPool": false, "Health": "Healthy",
   "Volumes": {"Path": "E:\\", "Label": "Backup", "Size": 4000000000000, "Free": 1000000000000, "Folders": 3, "Catalogue": false, "System": false}},
  {"Number": 3, "FriendlyName": "WD Elements", "UniqueId": "U3", "Size": 4000787030016, "IsSystem": false, "IsBoot": false,
   "IsReadOnly": false, "IsOffline": false, "BusType": "USB", "ImagePath": "", "InPool": false, "Health": "Warning",
   "Operational": "Predictive Failure", "Volumes": []},
  {"Number": 4, "FriendlyName": "Msft Virtual Disk", "UniqueId": "U4", "Size": 68719476736, "IsSystem": false, "IsBoot": false,
   "IsReadOnly": false, "IsOffline": true, "BusType": "File Backed Virtual", "ImagePath": "D:\\ci\\a.vhdx", "InPool": false,
   "Health": "Healthy", "Volumes": []}],
 "sets": {"UniqueId": "V1", "FriendlyName": "Photos", "Size": 3998000000000, "Resiliency": "Mirror", "Columns": 1,
   "Health": "Warning", "Operational": "Degraded,Incomplete", "Mount": "F:\\", "Percent": -1,
   "Members": [{"Number": 5, "UniqueId": "P5", "Health": "Healthy", "Operational": "OK", "Usage": "Auto-Select"},
               {"Number": -1, "UniqueId": "P6", "Health": "Warning", "Operational": "Lost Communication", "Usage": "Auto-Select"}]}})";

std::vector<drives::disk> mac_disks() {
  const json::value all = parsed(kMacList);
  std::vector<drives::disk> out;
  for (const auto& [id, internal, bus, size, model, virt] :
       std::vector<std::tuple<std::string, bool, std::string, std::uint64_t, std::string, const char*>>{
           {"disk0", true, "Apple Fabric", 1000000000000, "APPLE SSD", "Physical"},
           {"disk3", true, "Apple Fabric", 994662584320, "", "Virtual"},
           {"disk4", false, "USB", 2000398934016, "Samsung T7", "Physical"},
           {"disk5", false, "USB", 2000398934016, "Samsung T7", "Physical"}}) {
    if (auto d = drives::parse_mac_disk(parsed(mac_info(id, internal, bus, size, model, virt)), all, "")) out.push_back(*d);
  }
  drives::unique_names(out);
  return out;
}

}  // namespace

TEST_CASE("host drives: the five choices, side by side, with their words", "[host-drives]") {
  const std::vector<std::uint64_t> two = {2 * TB, 4 * TB};
  const auto mac = drives::options_for(two, drives::os_kind::mac);
  REQUIRE(mac.size() == 5);
  CHECK(mac[0].c == drives::choice::separate);
  CHECK(mac[0].offered);
  CHECK(mac[0].space == 6 * TB);
  CHECK(contains(mac[0].space_words, "Nothing is erased"));
  CHECK(mac[1].title == "Mirror (RAID 1)");
  CHECK(mac[1].offered);
  CHECK(mac[1].recommended);
  CHECK(mac[1].space == 2 * TB);
  CHECK(mac[1].space_words == "2 TB, the size of the smallest drive.");
  CHECK(contains(mac[1].dead_words, "Nothing is lost"));
  CHECK(mac[2].title == "Span (JBOD)");
  CHECK(mac[2].space == 6 * TB);
  CHECK(contains(mac[2].dead_words, "often more"));
  CHECK(mac[3].title == "Stripe (RAID 0)");
  CHECK(mac[3].warning);
  CHECK(!mac[3].recommended);
  CHECK(mac[3].space == 4 * TB);
  CHECK(contains(mac[3].dead_words, "Everything is lost"));
  CHECK(mac[3].offered);  // every choice offered equally (owner, 2026-11-14): a warning, never hidden
  CHECK(mac[4].title == "Parity (RAID 5)");
  CHECK(!mac[4].offered);
  CHECK(contains(mac[4].why_not, "macOS can't make this"));
  for (const auto& o : mac) {
    CHECK(!o.space_words.empty());
    CHECK(!o.dead_words.empty());
    CHECK(!o.speed_words.empty());
  }
  // Parity on Windows needs a third drive; with it, all but one drive's worth.
  const auto win2 = drives::options_for(two, drives::os_kind::windows);
  CHECK(!win2[4].offered);
  CHECK(win2[4].why_not == "Needs 3 or more drives.");
  const std::vector<std::uint64_t> three = {4 * TB, 4 * TB, 3 * TB};
  const auto win3 = drives::options_for(three, drives::os_kind::windows);
  CHECK(win3[4].offered);
  CHECK(win3[4].space == 6 * TB);
  CHECK(contains(win3[4].speed_words, "slow to write"));
  // One drive: Separate alone. Storage Spaces unreachable: no combined choice, said why.
  const std::vector<std::uint64_t> one = {2 * TB};
  const auto single = drives::options_for(one, drives::os_kind::windows);
  CHECK(single[0].offered);
  for (int i = 1; i < 4; ++i) CHECK(single[static_cast<std::size_t>(i)].why_not == "Needs 2 or more drives.");
  const auto nopool = drives::options_for(two, drives::os_kind::windows, false);
  CHECK(nopool[0].offered);
  CHECK(contains(nopool[1].why_not, "Storage Spaces is not available"));
  const auto lin = drives::options_for(two, drives::os_kind::linux_os);
  CHECK(lin[0].offered);
  CHECK(contains(lin[1].why_not, "not built yet"));
  CHECK(contains(drives::not_a_backup_text(), "RAID is not a backup"));
  CHECK(contains(drives::not_a_backup_text(), "Deleting a file deletes it on every drive"));
  // The terminal's text and the card's JSON carry every row.
  const std::string text = cli::choices_text(mac, 80);
  CHECK(contains(text, "RAID is not a backup"));
  for (const auto& o : mac) CHECK(contains(text, o.title));
  CHECK(contains(text, "not offered: macOS can't make this"));
  const json::value j = parsed(cli::choices_json(mac));
  REQUIRE(j.find("options"));
  CHECK(j.find("options")->a.size() == 5);
  CHECK(*j.find("options")->a[1].str("choice") == "mirror");
  CHECK(j.find("options")->a[1].boolean("recommended") == true);
}

TEST_CASE("host drives: sizes, choice words, labels", "[host-drives]") {
  CHECK(drives::size_words(2 * TB) == "2 TB");
  CHECK(drives::size_words(1'800'000'000'000ull) == "1.8 TB");
  CHECK(drives::size_words(500 * GB) == "500 GB");
  CHECK(drives::size_words(67'108'864) == "67 MB");
  for (const auto c : drives::kAllChoices) CHECK(drives::parse_choice(drives::choice_word(c)) == c);
  CHECK(drives::parse_choice("raid1") == drives::choice::mirror);
  CHECK(drives::parse_choice("jbod") == drives::choice::span);
  CHECK(!drives::parse_choice("raid6"));
  CHECK(drives::good_label("Photos"));
  CHECK(drives::good_label("Photo Mirror_2-b"));
  CHECK(!drives::good_label(""));
  CHECK(!drives::good_label(" Photos"));
  CHECK(!drives::good_label("Photos'; rm -rf /"));
  CHECK(!drives::good_label("\"quoted\""));
  CHECK(!drives::good_label("abcdefghijklmnopqrstuvwxyz12"));  // 28
  CHECK(!drives::good_label("Fotos\xC3\xA9"));
}

TEST_CASE("host drives: the erase guards", "[host-drives]") {
  drives::guard g;
  g.os = drives::os_kind::mac;
  drives::disk ok;
  ok.id = "disk4";
  ok.name = "Photos";
  ok.volumes.push_back({"/Volumes/Photos", "Photos", TB, GB, 3, false});
  CHECK(drives::refusal_of(ok, g).empty());

  auto d = ok;
  d.system = true;
  CHECK(drives::refusal_of(d, g) == "it holds this computer's system");
  d = ok;
  d.internal = true;
  CHECK(drives::refusal_of(d, g) == "it is this Mac's internal drive");
  g.os = drives::os_kind::windows;
  CHECK(drives::refusal_of(d, g).empty());  // a PC's second internal drive is the usual NAS drive
  g.os = drives::os_kind::mac;
  d = ok;
  d.in_set = true;
  CHECK(contains(drives::refusal_of(d, g), "already part of"));
  d = ok;
  d.read_only = true;
  CHECK(drives::refusal_of(d, g) == "it is read-only");
  d = ok;
  d.volumes[0].has_catalogue = true;
  CHECK(contains(drives::refusal_of(d, g), "MediaViewer catalogue"));
  // The library or the app's data on it.
  g.protected_paths = {"/Volumes/Photos/Driftwire"};
  CHECK(drives::refusal_of(ok, g) == "it holds MediaViewer's library or data");
  g.protected_paths = {"/Volumes/PhotosToo/Driftwire"};  // a name that only starts the same
  CHECK(drives::refusal_of(ok, g).empty());
  // A shared folder on it may be the only copy.
  g.shared_paths = {"/volumes/photos/2024"};  // the Mac's case-insensitive names
  CHECK(contains(drives::refusal_of(ok, g), "may be its only copy"));
  CHECK(contains(drives::refusal_of(ok, g), "host unshare"));
  g.shared_paths.clear();
  // An unmounted volume cannot be looked into: refused until it is mounted (a shared
  // folder's drive unmounted but attached would otherwise pass every check).
  d = ok;
  d.volumes[0].mount.clear();
  CHECK(drives::refusal_of(d, g) == "it has a volume that is not mounted (\"Photos\"), so what is on it cannot be checked: mount it first");

  // The test build: disk images made under its folder, nothing else.
  g.images_under = "/private/var/folders/x/mv_test";
  CHECK(contains(drives::refusal_of(ok, g), "only disk images"));
  d = image_disk("disk12", "", 64'000'000, "/private/var/folders/x/mv_test/a.dmg");
  CHECK(drives::refusal_of(d, g).empty());
  d.image_path = "/private/tmp/another-session/nas.sparseimage";  // someone else's image
  CHECK(contains(drives::refusal_of(d, g), "only disk images"));
  d.image_path = "/private/var/folders/x/mv_test2/a.dmg";
  CHECK(contains(drives::refusal_of(d, g), "only disk images"));
  d.image_path = "/private/var/folders/x/mv_test/a.dmg";
  d.image = false;  // a real disk claiming an image's path
  CHECK(contains(drives::refusal_of(d, g), "only disk images"));
  // And a system image is still the system's.
  d = image_disk("disk12", "", 64'000'000, "/private/var/folders/x/mv_test/a.dmg");
  d.system = true;
  CHECK(drives::refusal_of(d, g) == "it holds this computer's system");

  // Windows' paths.
  CHECK(drives::path_within("E:\\Photos\\2024", "E:\\", drives::os_kind::windows));
  CHECK(drives::path_within("e:/photos", "E:", drives::os_kind::windows));
  CHECK(!drives::path_within("F:\\Photos", "E:\\", drives::os_kind::windows));
  CHECK(drives::path_within("/a/b", "/", drives::os_kind::linux_os));
  CHECK(!drives::path_within("/A/b", "/a", drives::os_kind::linux_os));
  CHECK(!drives::path_within("", "/a", drives::os_kind::mac));
}

TEST_CASE("host drives: the Mac's disk facts from diskutil's lists", "[host-drives]") {
  const auto ds = mac_disks();
  REQUIRE(ds.size() == 3);  // disk3 is APFS's synthesized container, not a drive
  CHECK(ds[0].id == "disk0");
  CHECK(ds[0].system);  // its APFS container holds "/"
  CHECK(ds[0].internal);
  CHECK(ds[1].id == "disk4");
  CHECK(!ds[1].system);
  CHECK(ds[1].model == "Samsung T7");
  REQUIRE(ds[1].volumes.size() == 1);  // the EFI partition is not a volume of the person's
  CHECK(ds[1].volumes[0].label == "Photos");
  CHECK(ds[1].volumes[0].mount == "/Volumes/Photos");
  CHECK(ds[1].name == "Photos");
  CHECK(ds[2].name == "Samsung T7");  // no volume: its model
  CHECK(ds[1].smart == "verified");
  drives::guard g;
  g.os = drives::os_kind::mac;
  CHECK(drives::refusal_of(ds[0], g) == "it holds this computer's system");
  // Two the same get their ids.
  std::vector<drives::disk> twins = {image_disk("disk12", "", 1, ""), image_disk("disk13", "", 1, "")};
  drives::unique_names(twins);
  CHECK(twins[0].name == "Disk Image (disk12)");
  CHECK(twins[1].name == "Disk Image (disk13)");
  // hdiutil's images, whole disks only.
  const json::value hd = parsed(R"({"images": [
    {"image-path": "/private/var/folders/x/mv_test/a.dmg", "system-entities": [{"dev-entry": "/dev/disk12"}]},
    {"image-path": "/private/tmp/other/nas.sparseimage", "system-entities": [{"dev-entry": "/dev/disk10"}, {"dev-entry": "/dev/disk10s1"}]}]})");
  const auto imgs = drives::parse_hdiutil_images(hd);
  REQUIRE(imgs.size() == 2);
  CHECK(imgs[0] == std::pair<std::string, std::string>("disk12", "/private/var/folders/x/mv_test/a.dmg"));
  CHECK(imgs[1].first == "disk10");
  const auto img = drives::parse_mac_disk(parsed(mac_info("disk12", false, "Disk Image", 67108864, "Disk Image", "Virtual")),
                                          parsed(R"({"AllDisksAndPartitions": []})"), imgs[0].second);
  REQUIRE(img);
  CHECK(img->image);
  CHECK(img->image_path == imgs[0].second);
}

TEST_CASE("host drives: AppleRAID's health, as diskutil said it", "[host-drives]") {
  auto sets = drives::parse_mac_raid(parsed(kRaidOnline));
  REQUIRE(sets.size() == 1);
  CHECK(sets[0].name == "MVTestMirror");
  CHECK(sets[0].kind == drives::choice::mirror);
  CHECK(sets[0].state == drives::health::healthy);
  CHECK(sets[0].members.size() == 2);
  CHECK(drives::health_line(sets[0]) == "MVTestMirror (mirror): healthy");
  sets = drives::parse_mac_raid(parsed(kRaidLostOne));
  REQUIRE(sets.size() == 1);
  CHECK(sets[0].state == drives::health::degraded);  // "Offline" with one half there: a drive lost
  CHECK(sets[0].members[1].missing);
  CHECK(contains(drives::health_line(sets[0]), "replace it: driftwire host drives replace \"MVTestMirror\""));
  // A spare left beside one online member: still one drive short.
  auto spare = drives::parse_mac_raid(parsed(R"({"AppleRAIDSets": [{"Name": "M", "AppleRAIDSetUUID": "S", "Level": "Mirror",
    "Status": "Online", "Members": [{"AppleRAIDMemberUUID": "a", "BSD Name": "disk12s1", "MemberStatus": "Online"}]}]})"));
  CHECK(spare[0].state == drives::health::degraded);
  auto rebuild = drives::parse_mac_raid(parsed(R"J({"AppleRAIDSets": [{"Name": "M", "AppleRAIDSetUUID": "S", "Level": "Mirror",
    "Status": "Degraded", "Members": [{"AppleRAIDMemberUUID": "a", "BSD Name": "disk12s1", "MemberStatus": "Online"},
    {"AppleRAIDMemberUUID": "b", "BSD Name": "disk13s1", "MemberStatus": "Rebuilding (42% done)"}]}]})J"));
  CHECK(rebuild[0].state == drives::health::rebuilding);
  CHECK(rebuild[0].percent == 42);
  CHECK(drives::health_line(rebuild[0]) == "M (mirror): rebuilding, 42% done");
  // A stripe that lost a drive is gone, not degraded.
  auto stripe = drives::parse_mac_raid(parsed(R"({"AppleRAIDSets": [{"Name": "S", "AppleRAIDSetUUID": "T", "Level": "Stripe",
    "Status": "Offline", "Members": [{"AppleRAIDMemberUUID": "a", "BSD Name": "disk12s1", "MemberStatus": "Online"}, {"AppleRAIDMemberUUID": "b"}]}]})"));
  CHECK(stripe[0].kind == drives::choice::stripe);
  CHECK(stripe[0].state == drives::health::offline);
}

TEST_CASE("host drives: Windows' disks and Storage Spaces from the script's JSON", "[host-drives]") {
  const auto w = drives::parse_windows(parsed(kWindows));
  REQUIRE(w);
  CHECK(w->pool_ok);
  REQUIRE(w->disks.size() == 4);
  CHECK(w->disks[0].system);
  CHECK(w->disks[1].id == "2");
  REQUIRE(w->disks[1].volumes.size() == 1);  // ConvertTo-Json's one object, not an array of one
  CHECK(w->disks[1].volumes[0].used == 3 * TB);
  CHECK(w->disks[1].volumes[0].folders == 3);
  CHECK(w->disks[2].smart == "failing");  // Predictive Failure
  CHECK(w->disks[3].image);
  CHECK(w->disks[3].image_path == "D:\\ci\\a.vhdx");
  CHECK(!w->disks[3].online);
  REQUIRE(w->sets.size() == 1);
  CHECK(w->sets[0].kind == drives::choice::mirror);
  CHECK(w->sets[0].state == drives::health::degraded);
  CHECK(w->sets[0].members[1].missing);
  drives::guard g;
  g.os = drives::os_kind::windows;
  CHECK(drives::refusal_of(w->disks[0], g) == "it holds this computer's system");
  CHECK(drives::refusal_of(w->disks[3], g) == "Windows has it offline");
  CHECK(!drives::parse_windows(parsed(R"({"sets": []})")));
}

TEST_CASE("host drives: plans against a stand-in list, and the typed names", "[host-drives]") {
  const auto ds = mac_disks();
  drives::guard g;
  g.os = drives::os_kind::mac;
  const std::vector<std::string> both = {"disk4", "disk5"};
  auto p = drives::make_plan(drives::choice::mirror, "Library", both, ds, g, drives::os_kind::mac);
  CHECK(p.ok());
  CHECK(p.space == 2000398934016ull);
  CHECK(p.disks.size() == 2);
  // The names typed must be each drive's, in order.
  CHECK(drives::confirmed(p, std::vector<std::string>{"Photos", "Samsung T7"}));
  CHECK(drives::confirmed(p, std::vector<std::string>{"  Photos ", "Samsung T7\t"}));
  CHECK(!drives::confirmed(p, std::vector<std::string>{"photos", "Samsung T7"}));
  CHECK(!drives::confirmed(p, std::vector<std::string>{"Samsung T7", "Photos"}));
  CHECK(!drives::confirmed(p, std::vector<std::string>{"Photos"}));
  CHECK(!drives::confirmed(p, std::vector<std::string>{"yes", "yes"}));
  // The system's drive is refused, with the reason, whatever is typed.
  const std::vector<std::string> sys = {"disk0", "disk4"};
  p = drives::make_plan(drives::choice::stripe, "Fast", sys, ds, g, drives::os_kind::mac);
  CHECK(!p.ok());
  REQUIRE(p.refusals.size() == 1);
  CHECK(contains(p.refusals[0], "this computer's system"));
  // A folder the server shares on disk4.
  drives::guard shared = g;
  shared.shared_paths = {"/Volumes/Photos/2024"};
  p = drives::make_plan(drives::choice::span, "Big", both, ds, shared, drives::os_kind::mac);
  REQUIRE(p.refusals.size() == 1);
  CHECK(contains(p.refusals[0], "Photos: it holds a folder this server shares"));
  // What cannot be a plan at all.
  CHECK(contains(drives::make_plan(drives::choice::parity, "P", both, ds, g, drives::os_kind::mac).error, "macOS can't"));
  const std::vector<std::string> one = {"disk4"};
  CHECK(contains(drives::make_plan(drives::choice::mirror, "M", one, ds, g, drives::os_kind::mac).error, "Needs 2"));
  const std::vector<std::string> twice = {"disk4", "disk4"};
  CHECK(contains(drives::make_plan(drives::choice::mirror, "M", twice, ds, g, drives::os_kind::mac).error, "named twice"));
  const std::vector<std::string> unknown = {"disk4", "disk9"};
  CHECK(contains(drives::make_plan(drives::choice::mirror, "M", unknown, ds, g, drives::os_kind::mac).error, "no drive disk9"));
  CHECK(contains(drives::make_plan(drives::choice::mirror, "a'b", both, ds, g, drives::os_kind::mac).error, "--label"));
  // Separate erases nothing: no guard, no names; a drive with no volume has nothing to share.
  p = drives::make_plan(drives::choice::separate, "", both, ds, g, drives::os_kind::mac);
  CHECK(p.error.empty());
  REQUIRE(p.refusals.size() == 1);
  CHECK(contains(p.refusals[0], "Samsung T7: it has no mounted volume"));
  CHECK(drives::confirmed(p, {}));
  // Separate never shares the system's drive whole.
  const std::vector<std::string> sys_sep = {"disk0"};
  p = drives::make_plan(drives::choice::separate, "", sys_sep, ds, g, drives::os_kind::mac);
  REQUIRE(p.refusals.size() == 1);
  CHECK(contains(p.refusals[0], "this computer's system"));
  // A name a combined drive already has.
  drives::set_health have;
  have.name = "Library";
  const std::vector<drives::set_health> sets = {have};
  CHECK(contains(drives::make_plan(drives::choice::mirror, "library", both, ds, g, drives::os_kind::mac, true, sets).error,
                 "already a combined drive called \"Library\""));
  // Windows' list.
  const auto w = drives::parse_windows(parsed(kWindows));
  REQUIRE(w);
  auto wd = w->disks;
  drives::unique_names(wd);
  CHECK(wd[1].name == "Backup");
  CHECK(wd[2].name == "WD Elements");
  drives::guard wg;
  wg.os = drives::os_kind::windows;
  const std::vector<std::string> pair = {"2", "3"};
  p = drives::make_plan(drives::choice::mirror, "Photos", pair, wd, wg, drives::os_kind::windows, w->pool_ok);
  CHECK(p.ok());
  // The facts that must not change before the erase.
  auto moved = p.disks[0];
  CHECK(drives::same_disk(p.disks[0], moved));
  moved.identity = "U9";
  CHECK(!drives::same_disk(p.disks[0], moved));
}

TEST_CASE("host drives: what each OS is asked to run", "[host-drives]") {
  const auto ds = mac_disks();
  drives::guard g;
  g.os = drives::os_kind::mac;
  const std::vector<std::string> both = {"disk4", "disk5"};
  auto p = drives::make_plan(drives::choice::mirror, "Photo Mirror", both, ds, g, drives::os_kind::mac);
  const auto args = drives::mac_create_args(p);
  CHECK(args == std::vector<std::string>{"appleRAID", "create", "mirror", "Photo Mirror", "JHFS+", "disk4", "disk5"});
  p.c = drives::choice::span;
  CHECK(drives::mac_create_args(p)[2] == "concat");
  p.c = drives::choice::stripe;
  CHECK(drives::mac_create_args(p)[2] == "stripe");
  p.c = drives::choice::parity;
  CHECK(drives::mac_create_args(p).empty());
  CHECK(drives::mac_admin_script(args) ==
        "do shell script \"/usr/sbin/diskutil 'appleRAID' 'create' 'mirror' 'Photo Mirror' 'JHFS+' 'disk4' 'disk5'\" with administrator privileges");
  // Inside the administrator prompt, before the step: each disk still the one confirmed.
  drives::disk withparts = p.disks[0];
  withparts.parts = {{"disk4s2", "8441E4D2"}};
  const std::vector<drives::disk> checked = {withparts};
  const std::string sh = drives::mac_checked_shell({args}, checked);
  CHECK(contains(sh, "[ \"$(/usr/sbin/diskutil info -plist 'disk4' | /usr/bin/plutil -extract TotalSize raw -o - - 2>/dev/null)\" = '2000398934016' ]"));
  CHECK(contains(sh, "-extract MediaName raw -o - - 2>/dev/null)\" = 'Samsung T7' ]"));
  CHECK(contains(sh, "-extract Internal raw -o - - 2>/dev/null)\" = 'false' ]"));
  CHECK(contains(sh, "info -plist 'disk4s2' | /usr/bin/plutil -extract DiskUUID raw -o - - 2>/dev/null)\" = '8441E4D2' ]"));
  CHECK(sh.find("exit 3") < sh.find("'appleRAID' 'create'"));
  CHECK(contains(drives::mac_admin_wrap(sh), "do shell script \"[ \\\"$(/usr/sbin/diskutil"));
  // A refused plan has no command at all.
  const std::vector<std::string> sys = {"disk0", "disk4"};
  CHECK(drives::mac_create_args(drives::make_plan(drives::choice::mirror, "X", sys, ds, g, drives::os_kind::mac)).empty());

  const auto w = drives::parse_windows(parsed(kWindows));
  REQUIRE(w);
  drives::guard wg;
  wg.os = drives::os_kind::windows;
  const std::vector<std::string> pair = {"2", "3"};
  auto wp = drives::make_plan(drives::choice::mirror, "Photos", pair, w->disks, wg, drives::os_kind::windows);
  std::string s = drives::windows_create_script(wp);
  CHECK(contains(s, "if (-not $d -or $d.UniqueId -ne 'U2') { exit 2 }"));
  CHECK(contains(s, "if (-not $d -or $d.UniqueId -ne 'U3') { exit 2 }"));
  CHECK(contains(s, "if ($d.IsSystem -or $d.IsBoot) { exit 3 }"));
  CHECK(contains(s, "Update-HostStorageCache"));
  CHECK(contains(s, "$global:mvMount = [string]$part.DriveLetter"));
  // Every check comes before the first Clear-Disk.
  CHECK(s.rfind("UniqueId -ne") < s.find("Clear-Disk"));
  CHECK(contains(s, "-ResiliencySettingName Mirror -NumberOfDataCopies 2"));
  CHECK(contains(s, "Format-Volume -Partition $part -FileSystem NTFS -NewFileSystemLabel 'Photos'"));
  wp.c = drives::choice::span;
  CHECK(contains(drives::windows_create_script(wp), "-ResiliencySettingName Simple -NumberOfColumns 1"));
  wp.c = drives::choice::stripe;
  CHECK(contains(drives::windows_create_script(wp), "-ResiliencySettingName Simple -NumberOfColumns 2"));
  wp.c = drives::choice::separate;
  CHECK(drives::windows_create_script(wp).empty());
  CHECK(drives::ps_quote("it's") == "'it''s'");

  // Replacing: the Mac removes the missing member, then adds the new disk (which rebuilds).
  const auto lost = drives::parse_mac_raid(parsed(kRaidLostOne));
  const auto fresh = image_disk("disk13", "", 67108864, "");
  const auto steps = drives::mac_replace_steps(lost[0], fresh);
  REQUIRE(steps.size() == 2);
  CHECK(steps[0] == std::vector<std::string>{"appleRAID", "remove", "EC20", "67D6"});
  CHECK(steps[1] == std::vector<std::string>{"appleRAID", "add", "member", "disk13", "67D6"});
  auto stripe = lost[0];
  stripe.kind = drives::choice::stripe;
  CHECK(drives::mac_replace_steps(stripe, fresh).empty());
  drives::disk wfresh = w->disks[2];
  const std::string r = drives::windows_replace_script(w->sets[0], wfresh);
  CHECK(r.find("UniqueId -ne 'U3'") < r.find("Clear-Disk"));
  CHECK(contains(r, "Get-PhysicalDisk -UniqueId 'P6' | Set-PhysicalDisk -Usage Retired"));
  CHECK(contains(r, "Repair-VirtualDisk -UniqueId 'V1'"));
}

TEST_CASE("host drives: health words, the notification, the phone's word", "[host-drives]") {
  drives::set_health s;
  s.id = "S";
  s.name = "Photos";
  s.kind = drives::choice::mirror;
  s.state = drives::health::degraded;
  CHECK(drives::change_words(s, drives::health::healthy) ==
        "Your server's mirror \"Photos\" lost a drive. Its files are safe until another fails: replace it soon.");
  CHECK(drives::change_words(s, drives::health::degraded).empty());  // said once
  s.state = drives::health::rebuilding;
  CHECK(drives::change_words(s, drives::health::degraded).empty());
  s.state = drives::health::offline;
  CHECK(contains(drives::change_words(s, drives::health::degraded), "offline"));
  std::vector<drives::set_health> sets(2, s);
  sets[0].state = drives::health::healthy;
  sets[1].state = drives::health::rebuilding;
  CHECK(drives::worst(sets) == drives::health::rebuilding);
  sets[0].state = drives::health::degraded;
  CHECK(drives::worst(sets) == drives::health::degraded);
  CHECK(drives::worst({}) == drives::health::healthy);
  for (const auto h : {drives::health::healthy, drives::health::rebuilding, drives::health::degraded, drives::health::offline,
                       drives::health::failing}) {
    CHECK(drives::parse_health(drives::health_word(h)) == h);
  }
  CHECK(cli::drives_status_line({}).empty());
  sets[0].name = "A";
  sets[1].name = "B";
  CHECK(cli::drives_status_line(sets) ==
        "drives      A (mirror): a drive is missing or failed; replace it: driftwire host drives replace \"A\"\n"
        "            B (mirror): rebuilding");

  // The phone reads the word in serve_caps (every session's), and says it.
  const std::string caps = serve::caps_json(1, serve::kAllOps, R"("storage":"degraded")");
  const auto c = serve::parse_caps(caps);
  REQUIRE(c);
  CHECK(c->storage == "degraded");
  CHECK(std::string(serve::storage_words(c->storage)) ==
        "Your server's mirror lost a drive. Your files are safe until another one fails: replace it soon.");
  const auto odd = serve::parse_caps(serve::caps_json(1, serve::kAllOps, R"("storage":"on fire")"));
  REQUIRE(odd);
  CHECK(odd->storage.empty());
  CHECK(serve::parse_caps(serve::caps_json(1, serve::kAllOps))->storage.empty());
  CHECK(std::string(serve::storage_words("healthy")).empty());
}

TEST_CASE("host drives: the arguments", "[host-drives]") {
  auto parse = [](std::vector<std::string> v) { return cli::parse_args(v, true); };
  auto p = parse({"host", "drives"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::drives);
  CHECK(cli::parse_drives_words(p.a.paths).act == cli::drives_action::list);
  p = parse({"host", "drives", "combine", "mirror", "disk4", "disk5", "--label", "Photos", "--confirm-stdin"});
  REQUIRE(p.error.empty());
  CHECK(p.a.label == "Photos");
  CHECK(p.a.confirm_stdin);
  auto w = cli::parse_drives_words(p.a.paths);
  CHECK(w.error.empty());
  CHECK(w.act == cli::drives_action::combine);
  CHECK(w.c == drives::choice::mirror);
  CHECK(w.ids == std::vector<std::string>{"disk4", "disk5"});
  p = parse({"host", "drives", "list", "--json"});
  REQUIRE(p.error.empty());
  CHECK(p.a.json);
  w = cli::parse_drives_words(p.a.paths);
  CHECK(w.act == cli::drives_action::list);
  w = cli::parse_drives_words(std::vector<std::string>{"replace", "Photos", "disk13"});
  CHECK(w.act == cli::drives_action::replace);
  CHECK(w.set == "Photos");
  CHECK(!cli::parse_drives_words(std::vector<std::string>{"combine", "raid6", "disk4"}).error.empty());
  CHECK(!cli::parse_drives_words(std::vector<std::string>{"erase", "disk4"}).error.empty());
  CHECK(!parse({"host", "share", "/x", "--json"}).error.empty());
  CHECK(!parse({"send", "a", "--confirm-stdin"}).error.empty());
  CHECK(!parse({"host", "drives", "--backup"}).error.empty());
  CHECK(!parse({"host", "drives", "--yes"}).error.empty());  // there is no --yes
}

namespace {

// A stand-in port: the disks it was given, what it was asked to do.
struct fake_state {
  std::mutex m;
  drives::listing l;
  std::vector<drives::plan> created;
  std::vector<std::string> replaced;
  std::vector<std::string> notes;
  std::string mount;  // what create answers
  int lists = 0;
  std::function<void(fake_state&, int)> on_list;  // the n-th listing (from 1) may change the disks
};

class fake_port final : public drives::port {
 public:
  explicit fake_port(fake_state& s) : s_(s) {}
  drives::listing list() override {
    std::lock_guard g(s_.m);
    ++s_.lists;
    if (s_.on_list) s_.on_list(s_, s_.lists);
    auto l = s_.l;
    drives::unique_names(l.disks);
    return l;
  }
  drives::run_result create(const drives::plan& p, bool elevate) override {
    std::lock_guard g(s_.m);
    CHECK(!elevate);  // the test build never asks for an administrator
    s_.created.push_back(p);
    return {true, {}, s_.mount};
  }
  drives::run_result replace(const drives::set_health& s, const drives::disk& fresh, bool) override {
    std::lock_guard g(s_.m);
    s_.replaced.push_back(s.name + "<-" + fresh.id);
    return {true, {}, {}};
  }
  void notify(const std::string&, const std::string& body) override {
    std::lock_guard g(s_.m);
    s_.notes.push_back(body);
  }

 private:
  fake_state& s_;
};

int run_drives(const fs::path& data, std::vector<std::string> words, fake_state& st, std::vector<std::string> typed,
               std::string& out, const std::string& label = {}) {
  cli::args a;
  a.cmd = cli::command::host;
  a.host = cli::host_action::drives;
  a.paths = std::move(words);
  a.label = label;
  a.confirm_stdin = true;
  a.data = utf8(data);
  a.test_data = utf8(data);  // the test build's images must be under it
  const cli::host_layout l = cli::host_layout_of(utf8(data));
  REQUIRE(cli::make_host_folders(l));
  cli::drives_run run;
  run.make_port = [&st] { return std::make_unique<fake_port>(st); };
  std::size_t next = 0;
  run.read_line = [&typed, &next](std::string& line) {
    if (next >= typed.size()) return false;
    line = typed[next++];
    return true;
  };
  cli::print_capture cap(out);
  return cli::run_host_drives(a, l, utf8(data), true, run);
}

}  // namespace

TEST_CASE("host drives: combining through the command, against a stand-in port", "[host-drives]") {
  scratch_dir dir("host_drives");
  const fs::path data = dir / "data";
  fs::create_directories(data);
  const std::string root = utf8(fs::weakly_canonical(data));
  fake_state st;
  st.l.disks = {image_disk("disk12", "", 64'000'000, root + "/a.dmg"), image_disk("disk13", "", 64'000'000, root + "/b.dmg"),
                image_disk("disk20", "", 64'000'000, "/private/tmp/someone-else/nas.sparseimage")};
  drives::disk real;
  real.id = "disk0";
  real.model = "APPLE SSD";
  real.size = TB;
  real.system = true;
  st.l.disks.push_back(real);

  std::string out;
  if (drives::this_os() == drives::os_kind::linux_os) {
    // Linux has no combining yet (mdadm, later): it says so and erases nothing.
    CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk13"}, st, {"x", "y"}, out, "Photos") == 2);
    CHECK(contains(out, "not built yet"));
    CHECK(st.created.empty());
    return;
  }
  // A wrong name: nothing is erased.
  CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk13"}, st, {"Disk Image (disk12)", "Disk Image"}, out, "Photos") == 1);
  CHECK(st.created.empty());
  CHECK(contains(out, "EVERYTHING on both drives is erased"));
  CHECK(contains(out, "RAID is not a backup"));
  // Another session's image and the system's drive are refused before anything is asked.
  out.clear();
  CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk20"}, st, {"x", "y"}, out, "Photos") == 1);
  CHECK(contains(out, "only disk images"));
  CHECK(!contains(out, "Type the drive's name"));
  out.clear();
  CHECK(run_drives(data, {"combine", "stripe", "disk0", "disk12"}, st, {}, out, "Photos") == 1);
  CHECK(contains(out, "this computer's system"));
  CHECK(st.created.empty());
  // A drive renumbered or swapped while the names were typed: listed again, refused.
  out.clear();
  st.lists = 0;
  st.on_list = [](fake_state& f, int n) {
    if (n == 2) f.l.disks[1].identity = "another drive now";
  };
  CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk13"}, st, {"Disk Image (disk12)", "Disk Image (disk13)"}, out, "Photos") == 1);
  CHECK(contains(out, "changed since it was confirmed; nothing was erased"));
  CHECK(st.created.empty());
  st.on_list = nullptr;
  st.l.disks[1].identity = "id-disk13";
  // A catalogue that appeared on one meanwhile.
  out.clear();
  st.lists = 0;
  st.on_list = [&](fake_state& f, int n) {
    if (n == 2) f.l.disks[0].volumes = {{utf8(dir / "late"), "Late", 1, 1, 0, true}};
  };
  CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk13"}, st, {"Disk Image (disk12)", "Disk Image (disk13)"}, out, "Photos") == 1);
  CHECK(contains(out, "MediaViewer catalogue"));
  CHECK(st.created.empty());
  st.on_list = nullptr;
  st.l.disks[0].volumes.clear();
  // No --label.
  out.clear();
  CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk13"}, st, {}, out) == 2);
  // The names right: combined, then shared through host share.
  const fs::path mount = dir / "mounted";
  fs::create_directories(mount);
  st.mount = utf8(mount);
  out.clear();
  CHECK(run_drives(data, {"combine", "mirror", "disk12", "disk13"}, st, {"Disk Image (disk12)", "Disk Image (disk13)"}, out, "Photos") == 0);
  REQUIRE(st.created.size() == 1);
  CHECK(st.created[0].c == drives::choice::mirror);
  CHECK(st.created[0].label == "Photos");
  CHECK(contains(out, "Made \"Photos\" (mirror, 64 MB); shared as \"Photos\""));
  const auto cfg = cli::host_files::config(cli::host_layout_of(utf8(data)), true);
  REQUIRE(cfg);
  REQUIRE(cfg->roots.size() == 1);
  CHECK(cfg->roots[0].label == "Photos");
  // Now that folder is shared: a drive holding it would be refused (it may be the only copy).
  st.l.disks[0].volumes = {{utf8(mount), "Photos", 64'000'000, 1'000'000, 1, false}};
  out.clear();
  CHECK(run_drives(data, {"combine", "span", "disk12", "disk13"}, st, {}, out, "Again") == 1);
  CHECK(contains(out, "may be its only copy"));
  CHECK(st.created.size() == 1);

  // Replacing a mirror's lost drive: the new drive's name typed.
  st.l.disks[0].volumes.clear();
  drives::set_health s;
  s.id = "S1";
  s.name = "Photos";
  s.kind = drives::choice::mirror;
  s.state = drives::health::degraded;
  s.members = {{"disk12s1", "a", "Online", false, false, -1}, {"", "b", "", true, false, -1}};
  st.l.sets = {s};
  out.clear();
  CHECK(run_drives(data, {"replace", "Photos", "disk13"}, st, {"nope"}, out) == 1);
  CHECK(st.replaced.empty());
  out.clear();
  CHECK(run_drives(data, {"replace", "Photos", "disk13"}, st, {"Disk Image (disk13)"}, out) == 0);
  REQUIRE(st.replaced.size() == 1);
  CHECK(st.replaced[0] == "Photos<-disk13");
  // `list --json` (the card's) says why a drive cannot be erased, and never a mount path.
  st.l.disks[1].volumes = {{utf8(mount) + "/x", "Inside", 1, 1, 0, false}};
  {
    std::string j;
    cli::args a;
    a.cmd = cli::command::host;
    a.host = cli::host_action::drives;
    a.json = true;
    a.data = utf8(data);
    a.test_data = utf8(data);
    cli::drives_run run;
    run.make_port = [&st] { return std::make_unique<fake_port>(st); };
    {
      cli::print_capture cap(j);
      CHECK(cli::run_host_drives(a, cli::host_layout_of(utf8(data)), utf8(data), true, run) == 0);
    }
    const json::value v = parsed(j);
    REQUIRE(v.find("disks"));
    CHECK(v.find("disks")->a.size() == 4);
    CHECK(contains(*v.find("disks")->a[2].str("refused"), "only disk images"));
    CHECK(contains(*v.find("disks")->a[3].str("refused"), "system"));
    CHECK(!contains(j, utf8(mount)));
    REQUIRE(v.find("sets"));
    CHECK(*v.find("sets")->a[0].str("state") == "degraded");
  }
}

TEST_CASE("host drives: the keeper watches the sets and tells the phone", "[host-drives]") {
  fake_state st;
  drives::set_health s;
  s.id = "S1";
  s.name = "Photos";
  s.kind = drives::choice::mirror;
  s.members = {{"disk12s1", "a", "Online", false, false, -1}, {"disk13s1", "b", "Online", false, false, -1}};
  st.l.sets = {s};
  cli::drives_keeper_config c;
  c.make_port = [&st] { return std::make_unique<fake_port>(st); };
  c.every_ms = 50;
  c.on_loopback = true;
  auto k = cli::make_drives_keeper(c);
  std::mutex wm;
  std::vector<std::string> words;
  cli::keeper_context ctx;
  ctx.loopback_only = true;
  ctx.storage_changed = [&](std::string w) {
    std::lock_guard g(wm);
    words.push_back(std::move(w));
  };
  k->start(ctx);
  const auto until = [](auto pred) {
    for (int i = 0; i < 200 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return pred();
  };
  CHECK(until([&] { return k->status_line() == "drives      Photos (mirror): healthy"; }));
  CHECK(k->state_numbers() == std::vector<std::pair<std::string, std::int64_t>>{{"drives_sets", 1}, {"drives_health", 0}});
  {
    std::lock_guard g(st.m);
    st.l.sets[0].state = drives::health::degraded;
    st.l.sets[0].members[1] = {"", "b", "", true, false, -1};
  }
  CHECK(until([&] {
    std::lock_guard g(wm);
    return !words.empty() && words.back() == "degraded";
  }));
  CHECK(until([&] {
    std::lock_guard g(st.m);
    return st.notes.size() == 1;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::lock_guard g(st.m);
    CHECK(st.notes.size() == 1);  // told once, not every pass
    CHECK(contains(st.notes[0], "Your server's mirror \"Photos\" lost a drive"));
    st.l.sets[0].state = drives::health::healthy;
  }
  CHECK(until([&] {
    std::lock_guard g(wm);
    return words.back().empty();
  }));
  // The tools fail for a while: what was known stands, and nothing is said again.
  {
    std::lock_guard g(st.m);
    st.l.error = "diskutil could not list the RAID sets";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(k->status_line() == "drives      Photos (mirror): healthy");
  {
    std::lock_guard g(st.m);
    CHECK(st.notes.size() == 1);
    st.l.error.clear();
    st.l.sets.clear();  // every drive of it unplugged: gone from the OS's list
  }
  CHECK(until([&] { return contains(k->status_line(), "Photos (mirror): offline"); }));
  CHECK(until([&] {
    std::lock_guard g(wm);
    return words.back() == "offline";
  }));
  k->stop();
}

#if defined(_WIN32)
// The scripts have only been read, never run, on a real PC: at least PowerShell parses each one.
TEST_CASE("host drives: PowerShell parses every script", "[host-drives]") {
  scratch_dir dir("host_drives_ps");
  const auto w = drives::parse_windows(parsed(kWindows));
  REQUIRE(w);
  drives::guard wg;
  wg.os = drives::os_kind::windows;
  std::vector<std::string> scripts;
  for (const auto c : {drives::choice::mirror, drives::choice::span, drives::choice::stripe}) {
    const std::vector<std::string> pair = {"2", "3"};
    scripts.push_back(drives::windows_create_script(drives::make_plan(c, "Photos", pair, w->disks, wg, drives::os_kind::windows)));
  }
  scripts.push_back(drives::windows_replace_script(w->sets[0], w->disks[2]));
  for (std::size_t i = 0; i < scripts.size(); ++i) {
    REQUIRE(!scripts[i].empty());
    const fs::path f = dir / ("s" + std::to_string(i) + ".ps1");
    write_text(f, scripts[i]);
    int code = 0;
    const std::string cmd = "$e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile('" + utf8(f) +
                            "', [ref]$null, [ref]$e); if ($e.Count) { $e | ForEach-Object { $_.Message }; exit 1 }";
    const auto out = drives::run_tool("powershell.exe", std::vector<std::string>{"-NoProfile", "-NonInteractive", "-Command", cmd}, code);
    REQUIRE(out);
    INFO(*out);
    CHECK(code == 0);
  }
}
#endif

#if defined(__APPLE__)
// By hand: mv_tests "[.host-drives-image]". Two hdiutil images made for it, combined into a real
// AppleRAID mirror through the command (the test build: images under its folder only, no
// administrator), one detached (degraded), a third put in its place (healthy again), then the
// set deleted and every image detached and removed.
TEST_CASE("host drives: a real AppleRAID mirror on two disk images", "[.host-drives-image]") {
  scratch_dir dir("host_drives_img");
  const fs::path data = dir / "data";
  fs::create_directories(data);
  const std::string root = utf8(fs::weakly_canonical(data));
  int code = 0;
  std::vector<std::string> attached;
  const auto attach = [&](const std::string& name) {
    const std::string img = root + "/" + name;
    REQUIRE(drives::run_tool("/usr/bin/hdiutil", std::vector<std::string>{"create", "-size", "64m", "-layout", "NONE", "-o", img}, code));
    REQUIRE(code == 0);
    const auto out = drives::run_tool("/usr/bin/hdiutil", std::vector<std::string>{"attach", "-nomount", img}, code);
    REQUIRE(out);
    REQUIRE(code == 0);
    std::string dev = out->substr(0, out->find_first_of(" \t\n"));
    REQUIRE(dev.rfind("/dev/disk", 0) == 0);
    dev = dev.substr(5);
    attached.push_back(dev);
    return dev;
  };
  struct cleanup {
    std::vector<std::string>& devs;
    std::string set;
    ~cleanup() {
      int c = 0;
      if (!set.empty()) (void)drives::run_tool("/usr/sbin/diskutil", std::vector<std::string>{"appleRAID", "delete", set}, c);
      for (const auto& d : devs) (void)drives::run_tool("/usr/bin/hdiutil", std::vector<std::string>{"detach", d, "-force"}, c);
    }
  } tidy{attached, {}};
  const std::string a = attach("a.dmg"), b = attach("b.dmg");

  cli::args args;
  args.cmd = cli::command::host;
  args.host = cli::host_action::drives;
  args.data = root;
  args.test_data = root;
  args.confirm_stdin = true;
  const cli::host_layout l = cli::host_layout_of(root);
  REQUIRE(cli::make_host_folders(l));
  const auto run_with = [&](std::vector<std::string> words, std::vector<std::string> typed, std::string label, std::string& out) {
    args.paths = std::move(words);
    args.label = std::move(label);
    cli::drives_run run;
    std::size_t next = 0;
    run.read_line = [&](std::string& line) {
      if (next >= typed.size()) return false;
      line = typed[next++];
      return true;
    };
    cli::print_capture cap(out);
    return cli::run_host_drives(args, l, root, true, run);
  };
  // This Mac's own drive is refused by the test build (and is the system's).
  std::string out;
  CHECK(run_with({"combine", "mirror", "disk0", a}, {}, "MVTestMirror", out) == 1);
  CHECK((contains(out, "this computer's system") || contains(out, "only disk images")));
  CHECK(contains(out, "nothing was erased"));

  auto port = drives::os_port();
  const auto names = [&] {
    std::map<std::string, std::string> m;
    for (const auto& d : port->list().disks) m[d.id] = d.name;
    return m;
  }();
  REQUIRE(names.count(a));
  REQUIRE(names.count(b));
  out.clear();
  REQUIRE(run_with({"combine", "mirror", a, b}, {names.at(a), names.at(b)}, "MVTestMirror", out) == 0);
  CHECK(contains(out, "Made \"MVTestMirror\" (mirror"));
  auto find_set = [&]() -> std::optional<drives::set_health> {
    for (const auto& s : port->sets_only().sets) {
      if (s.name == "MVTestMirror") return s;
    }
    return std::nullopt;
  };
  auto set = find_set();
  REQUIRE(set);
  tidy.set = set->id;
  CHECK(set->state == drives::health::healthy);
  CHECK(!set->mount.empty());

  // One drive gone.
  REQUIRE(drives::run_tool("/usr/bin/hdiutil", std::vector<std::string>{"detach", b, "-force"}, code));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  set = find_set();
  REQUIRE(set);
  CHECK(set->state == drives::health::degraded);
  CHECK(contains(drives::health_line(*set), "replace it"));

  // A new one in its place.
  const std::string c = attach("c.dmg");
  std::string cname;
  for (const auto& d : port->list().disks) {
    if (d.id == c) cname = d.name;
  }
  out.clear();
  REQUIRE(run_with({"replace", "MVTestMirror", c}, {cname}, "", out) == 0);
  bool healthy = false;
  for (int i = 0; i < 30 && !healthy; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    set = find_set();
    healthy = set && set->state == drives::health::healthy;
  }
  CHECK(healthy);
}
#endif
