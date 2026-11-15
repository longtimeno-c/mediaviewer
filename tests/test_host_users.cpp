// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The self-hosted computer's users, roles and LAN SMB share
// (docs/plans/self-host-users-smb.md). Filter with "[host-users]".
//
// Stand-ins only: the OS port is a recording one, the elevated step runs
// in-process on it, and nothing here makes a real account or share or touches
// this computer's sharing settings. The wire and router halves are in
// test_phone_pairing.cpp and test_driftwire_internet.cpp, with the same tag.
#include "catch_compat.h"

#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/host_users.h"
#include "addons/driftwire/host_smb.h"
#include "import_fixture.h"
#include "wire/owner_pass.h"
#include "wire/secrets.h"
#include "wire/serve.h"
#include "wire/serve_wire.h"
#include "wire/share_users.h"

using namespace mv::test;
namespace cli = mv::driftwire::cli;
namespace smb = mv::driftwire::host_smb;
namespace su = mv::driftwire::serve::share_users;
namespace owner = mv::driftwire::serve::owner;
namespace secrets = mv::driftwire::secrets;
namespace serve = mv::driftwire::serve;

namespace {

constexpr const char* kAdmin = "the owner's admin passphrase";

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::unique_ptr<secrets::store> store_in(const fs::path& p) {
  REQUIRE(secrets::owner_only_folder(utf8(p)));
  auto s = secrets::file_store(utf8(p));
  REQUIRE(s);
  return s;
}

// The OS, as the elevated step sees it: every call recorded, nothing real.
struct stand_in_port final : smb::port {
  smb::facts f;
  std::map<std::string, owner> accounts;     // name -> ours / theirs
  std::map<std::string, std::string> passwords;  // what the OS was given (the test's eyes only)
  struct share_rec {
    std::string path;
    std::vector<smb::member> members;
    bool ours = true;
  };
  std::map<std::string, share_rec> shares;
  std::vector<std::string> calls;
  bool started = false;

  stand_in_port() {
    f.supported = true;
    f.computer = "STUDIO";
    f.mdns_host = "studio.local";
  }
  smb::facts probe() override { return f; }
  owner account(const std::string& n) override {
    const auto it = accounts.find(n);
    return it == accounts.end() ? owner::none : it->second;
  }
  std::string make_account(const std::string& n, std::string_view pw) override {
    calls.push_back("make " + n);
    accounts[n] = owner::ours;
    passwords[n] = std::string(pw);
    return {};
  }
  std::string set_password(const std::string& n, std::string_view pw) override {
    calls.push_back("password " + n);
    passwords[n] = std::string(pw);
    return {};
  }
  std::string delete_account(const std::string& n) override {
    calls.push_back("delete " + n);
    accounts.erase(n);
    return {};
  }
  owner share(const std::string& n) override {
    const auto it = shares.find(n);
    if (it == shares.end()) return owner::none;
    return it->second.ours ? owner::ours : owner::theirs;
  }
  std::vector<std::string> last_strip;  // whose entries the last share change removed first
  std::string put_share(const smb::share_want& s, const std::vector<smb::member>& m, const std::vector<std::string>& ours) override {
    calls.push_back("share " + s.name);
    shares[s.name] = {s.path, m, true};
    last_strip = ours;
    return {};
  }
  std::vector<std::string> our_accounts() override {
    std::vector<std::string> out;
    for (const auto& [n, o] : accounts) {
      if (o == owner::ours) out.push_back(n);
    }
    return out;
  }
  std::string remove_share(const std::string& n, const std::vector<std::string>&) override {
    calls.push_back("unshare " + n);
    shares.erase(n);
    return {};
  }
  std::vector<std::string> our_shares() override {
    std::vector<std::string> out;
    for (const auto& [n, s] : shares) {
      if (s.ours) out.push_back(n);
    }
    return out;
  }
  std::string start_service() override {
    started = true;
    return {};
  }
};

// A host folder of the test's own: serve.json with folders, the admin passphrase at the cheapest cost.
struct host_fixture {
  scratch_dir dir{"host_users"};
  cli::host_layout l = cli::host_layout_of(utf8(dir.root() / "data"));
  fs::path photos = dir.root() / "Photos";
  fs::path videos = dir.root() / "Home Videos";
  stand_in_port os;
  std::vector<std::string> said;
  std::vector<std::string> input;  // what the "terminal" types, in order
  std::size_t next = 0;
  std::int64_t clock = 1'763'130'000;
  std::vector<std::string> requests;  // what the step was handed (to check, then wiped)

  host_fixture() {
    fs::create_directories(photos);
    fs::create_directories(videos);
    REQUIRE(cli::make_host_folders(l));
    serve::config c;
    c.name = "Studio";
    c.port = 0;
    c.roots = {{"a1a1a1a1a1a1a1a1", "Photos", utf8(photos)}, {"b2b2b2b2b2b2b2b2", "Home Videos", utf8(videos)},
               {"c3c3c3c3c3c3c3c3", serve::kCatalogueLabel, utf8(dir.root() / "lib")}};
    REQUIRE(cli::host_files::write(l.serve_config, serve::config_json(c)));
    auto st = store();
    const auto v = owner::make(kAdmin, 1, owner::kMinOps, owner::kMinMem);
    REQUIRE(v);
    REQUIRE(owner::save(*st, *v));
  }
  std::unique_ptr<secrets::store> store() { return store_in(fs::path(l.serve) / "secrets"); }
  cli::users_env env() {
    cli::users_env e;
    e.any_port = true;
    e.terminal = false;  // no y/N: the step runs at once
    e.self = "tristan";
    e.now = [this] { return clock; };
    e.say = [this](const std::string& line) { said.push_back(line); };
    e.ask = [this](const std::string&, std::string& out, bool) {
      if (next >= input.size()) return false;
      out = input[next++];
      return true;
    };
    e.elevate = [this](std::string_view request) {
      requests.emplace_back(request);
      std::string why;
      auto r = smb::parse_request(request, why);
      cli::elevated_run ran;
      ran.started = true;
      ran.answer = smb::answer_json(r ? smb::apply(*r, os) : smb::answer{false, why, {}, {}, {}, {}});
      return ran;
    };
    return e;
  }
  int run(std::vector<std::string> argv, std::vector<std::string> typed) {
    input = std::move(typed);
    next = 0;
    argv.insert(argv.begin(), {"host", "users"});
    const auto p = cli::parse_args(argv, true);
    REQUIRE(p.error.empty());
    cli::users_command c;
    const std::string why = cli::parse_users_command(p.a, c);
    REQUIRE(why.empty());
    c.input_stdin = true;  // one typed line each: no "Again"
    auto e = env();
    return cli::run_users(c, l, e);
  }
  std::vector<su::user> users() {
    auto st = store();
    std::vector<su::user> out;
    REQUIRE(su::load(*st, out));
    return out;
  }
  std::string all_said() const {
    std::string s;
    for (const auto& line : said) s += line + "\n";
    return s;
  }
};

}  // namespace

TEST_CASE("host users: names, roles and the list in the store, never a password", "[host-users]") {
  CHECK(su::valid_name("anna"));
  CHECK(su::valid_name("ben.k-2_x"));
  CHECK_FALSE(su::valid_name(""));
  CHECK_FALSE(su::valid_name("Anna"));                    // lowercase only: one name on every OS
  CHECK_FALSE(su::valid_name("2anna"));
  CHECK_FALSE(su::valid_name("an na"));
  CHECK_FALSE(su::valid_name("anna."));
  CHECK_FALSE(su::valid_name(std::string(21, 'a')));
  for (const char* os : {"admin", "administrator", "guest", "root", "everyone", "nobody", "daemon", "system", "users"}) {
    INFO(os);
    CHECK_FALSE(su::valid_name(os));
  }
  CHECK_FALSE(su::valid_name("tristan", "Tristan"));  // the account running it
  CHECK(su::parse_role("view") == su::role::view);
  CHECK(su::parse_role("change") == su::role::change);
  CHECK(su::parse_role("admin") == su::role::admin);
  CHECK_FALSE(su::parse_role("owner"));

  scratch_dir dir("host_users_store");
  auto st = store_in(dir.root() / "s");
  std::vector<su::user> list;
  REQUIRE(su::load(*st, list));
  CHECK(list.empty());
  CHECK(su::upsert(list, {"anna", su::role::view, std::nullopt, 0, 0}, 100).empty());
  CHECK(su::upsert(list, {"ben", su::role::change, std::vector<std::string>{"Photos", "Photos"}, 0, 0}, 100).empty());
  CHECK(su::upsert(list, {"Root", su::role::view, std::nullopt, 0, 0}, 100) == "bad-name");
  CHECK(su::upsert(list, {"cleo", su::role::view, std::vector<std::string>{}, 0, 0}, 100) == "bad-folders");
  REQUIRE(list.size() == 2);
  CHECK(list[1].folders->size() == 1);  // the same label once
  list[0].applied = 150;
  // A change of role waits for the step again; the same role keeps its stamp.
  CHECK(su::upsert(list, {"anna", su::role::view, std::nullopt, 0, 0}, 200).empty());
  CHECK(list[0].applied == 150);
  CHECK(list[0].created == 100);
  CHECK(su::upsert(list, {"anna", su::role::admin, std::nullopt, 0, 0}, 200).empty());
  CHECK(list[0].applied == 0);
  REQUIRE(su::save(*st, list));
  const std::string text = su::encode(list);
  CHECK_FALSE(contains(text, "password"));
  std::vector<su::user> back;
  REQUIRE(su::load(*st, back));
  REQUIRE(back.size() == 2);
  CHECK(back[0].name == "anna");
  CHECK(back[0].r == su::role::admin);
  CHECK(back[1].sees("Photos"));
  CHECK_FALSE(back[1].sees("Home Videos"));
  // A name twice, a bad role, a broken file: never read as a list, never overwritten.
  CHECK_FALSE(su::decode(R"({"v":1,"users":[{"name":"a","role":"view","created":1,"applied":0},{"name":"a","role":"view","created":1,"applied":0}]})"));
  CHECK_FALSE(su::decode(R"({"v":1,"users":[{"name":"a","role":"boss","created":1,"applied":0}]})"));
  const std::string broken = "{not json";
  REQUIRE(st->put(su::kUsersName, std::span(reinterpret_cast<const std::uint8_t*>(broken.data()), broken.size())));
  CHECK_FALSE(su::load(*st, back));
  CHECK(su::remove(list, "ben"));
  CHECK_FALSE(su::remove(list, "ben"));
}

TEST_CASE("host users: each role's rights on Windows, the Mac and a phone", "[host-users]") {
  // Read & execute, Modify, Full control: the share's and the folder's masks.
  CHECK(su::windows_share_mask(su::role::view) == 0x001200A9u);
  CHECK(su::windows_share_mask(su::role::change) == 0x001301BFu);
  CHECK(su::windows_share_mask(su::role::admin) == 0x001F01FFu);
  CHECK(su::windows_ntfs_mask(su::role::change) == su::windows_share_mask(su::role::change));
  CHECK_FALSE(contains(su::mac_acl_perms(su::role::view), "add_file"));
  CHECK(contains(su::mac_acl_perms(su::role::change), "delete_child"));
  CHECK_FALSE(contains(su::mac_acl_perms(su::role::change), "writesecurity"));
  CHECK(contains(su::mac_acl_perms(su::role::admin), "writesecurity"));
  const auto ids = [](const std::string& label) -> std::optional<std::string> {
    if (label == "Photos") return std::string("a1a1a1a1a1a1a1a1");
    return std::nullopt;
  };
  su::user v{"anna", su::role::view, std::vector<std::string>{"Photos", "Gone"}, 0, 0};
  const auto lv = su::phone_limit(v, ids);
  CHECK_FALSE(lv.backup);
  REQUIRE(lv.roots);
  CHECK(*lv.roots == std::vector<std::string>{"a1a1a1a1a1a1a1a1"});
  su::user c{"ben", su::role::change, std::nullopt, 0, 0};
  const auto lc = su::phone_limit(c, ids);
  CHECK(lc.backup);
  CHECK_FALSE(lc.roots);
  CHECK_FALSE(lc.any());
  su::user a{"cleo", su::role::admin, std::vector<std::string>{"Photos"}, 0, 0};
  CHECK_FALSE(su::phone_limit(a, ids).any());
}

TEST_CASE("host users: share names and addresses", "[host-users]") {
  CHECK(su::share_name("Photos") == "Photos");
  CHECK(su::share_name("Photos: 2024/25?") == "Photos 202425");
  CHECK(su::share_name(" .hidden. ") == "hidden");
  CHECK(su::share_name("***") == "Share");
  CHECK(su::share_name(std::string(100, 'x')).size() == 80);
  CHECK(su::unc_address("STUDIO", "Photos") == "\\\\STUDIO\\Photos");
  CHECK(su::smb_url("studio.local", "Home Videos") == "smb://studio.local/Home%20Videos");
  CHECK(su::unc_address("", "Photos").empty());
  CHECK(su::smb_url("studio.local", "").empty());
}

TEST_CASE("host users: the step's request is checked again, and its answer carries no password", "[host-users]") {
  smb::request r;
  r.shares.push_back({"Photos", "/Volumes/Photos"});
  r.users.push_back({"anna", su::role::view, {"Photos"}, "correct horse battery"});
  r.users.push_back({"ben", su::role::change, {"Photos"}, ""});
  r.remove.push_back("cleo");
  std::string text = smb::request_json(r);
  std::string why;
  auto back = smb::parse_request(text, why);
  REQUIRE(back);
  CHECK(why.empty());
  REQUIRE(back->users.size() == 2);
  CHECK(back->users[0].password == "correct horse battery");
  CHECK(back->users[1].password.empty());
  CHECK(back->remove == std::vector<std::string>{"cleo"});

  const auto refused = [&](const std::string& json) {
    std::string w;
    CHECK_FALSE(smb::parse_request(json, w));
    CHECK_FALSE(w.empty());
  };
  refused(R"({"v":1,"shares":[{"name":"Photos","path":"relative/path"}],"users":[],"remove":[]})");
  refused(R"({"v":1,"shares":[{"name":"Pho:tos","path":"/x"}],"users":[],"remove":[]})");
  refused(R"({"v":1,"shares":[],"users":[{"name":"root","role":"view","shares":[]}],"remove":[]})");
  refused(R"({"v":1,"shares":[],"users":[{"name":"anna","role":"owner","shares":[]}],"remove":[]})");
  refused(R"({"v":1,"shares":[],"users":[{"name":"anna","role":"view","shares":["Elsewhere"]}],"remove":[]})");
  refused(R"({"v":1,"shares":[],"users":[{"name":"anna","role":"view","shares":[],"password":"short"}],"remove":[]})");
  refused(R"({"v":1,"shares":[],"users":[{"name":"anna","role":"view","shares":[]}],"remove":["anna"]})");
  refused(R"({"v":2,"shares":[],"users":[],"remove":[]})");

  stand_in_port os;
  const auto a = smb::apply(*back, os);
  const std::string answer = smb::answer_json(a);
  CHECK_FALSE(contains(answer, "correct horse"));
  CHECK_FALSE(contains(answer, "/Volumes"));
  const auto parsed = smb::parse_answer(answer);
  REQUIRE(parsed);
  CHECK(parsed->computer == "STUDIO");
  CHECK(parsed->mdns_host == "studio.local");
  CHECK(smb::acceptable_password("eight888"));
  CHECK_FALSE(smb::acceptable_password("seven77"));
  CHECK_FALSE(smb::acceptable_password("with\nnewline"));
}

TEST_CASE("host users: the step makes only its own accounts and shares, and only what changed", "[host-users]") {
  stand_in_port os;
  os.accounts["bob"] = stand_in_port::owner::theirs;           // a real person's account here
  os.shares["Music"] = {"/Users/x/Music", {}, false};         // someone else's share
  os.f.service_on = false;
  smb::request r;
  r.shares.push_back({"Photos", "/p"});
  r.shares.push_back({"Videos", "/v"});
  r.shares.push_back({"Music", "/m"});
  r.users.push_back({"anna", su::role::view, {"Photos", "Music"}, "anna's password"});
  r.users.push_back({"bob", su::role::change, {"Photos"}, "bob's password"});
  r.users.push_back({"cleo", su::role::admin, {"Photos"}, ""});  // new and no password
  const auto a = smb::apply(r, os);
  CHECK_FALSE(a.ok);
  const auto item = [&](const std::vector<smb::item>& list, const std::string& n) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const smb::item& i) { return i.name == n; });
    REQUIRE(it != list.end());
    return *it;
  };
  CHECK(item(a.users, "anna").ok);
  CHECK_FALSE(item(a.users, "bob").ok);  // not MediaViewer's: left alone
  CHECK(contains(item(a.users, "bob").why, "not MediaViewer's"));
  CHECK(os.passwords.count("bob") == 0);
  CHECK_FALSE(item(a.users, "cleo").ok);
  CHECK(item(a.users, "cleo").why == "needs a password");
  // Photos opened for anna alone (bob's and cleo's accounts are not there to list).
  REQUIRE(os.shares.count("Photos"));
  REQUIRE(os.shares["Photos"].members.size() == 1);
  CHECK(os.shares["Photos"].members[0].user == "anna");
  CHECK(os.shares["Photos"].members[0].r == su::role::view);
  CHECK(os.shares.count("Videos") == 0);       // nobody may open it: not made
  CHECK_FALSE(item(a.shares, "Music").ok);     // someone else's: left alone
  CHECK_FALSE(os.shares["Music"].ours);
  CHECK(os.started);                           // file sharing was off and a share was made

  // Again with anna's role changed and no password: no account is made, the share is set again.
  os.calls.clear();
  smb::request again;
  again.shares.push_back({"Photos", "/p"});
  again.users.push_back({"anna", su::role::change, {"Photos"}, ""});
  again.remove.push_back("bob");
  const auto b = smb::apply(again, os);
  CHECK(std::find(os.calls.begin(), os.calls.end(), "make anna") == os.calls.end());
  CHECK(os.shares["Photos"].members[0].r == su::role::change);
  CHECK(os.accounts.count("bob"));  // removing never deletes someone else's account
  CHECK_FALSE(item(b.users, "bob").ok);

  // anna taken away: her share goes before her account (so its entries can be removed).
  os.calls.clear();
  smb::request gone;
  gone.shares.push_back({"Photos", "/p"});
  gone.remove.push_back("anna");
  const auto c = smb::apply(gone, os);
  CHECK(c.ok);
  REQUIRE(os.calls.size() == 2);
  CHECK(os.calls[0] == "unshare Photos");
  CHECK(os.calls[1] == "delete anna");

  // SMB1 on, or no administrator: nothing at all is done.
  os.calls.clear();
  os.f.smb1_on = true;
  CHECK_FALSE(smb::apply(r, os).ok);
  os.f.smb1_on = false;
  os.f.supported = false;
  os.f.why_not = "no administrator";
  const auto d = smb::apply(r, os);
  CHECK(d.why == "no administrator");
  CHECK(os.calls.empty());
}

TEST_CASE("host users: the plan names every folder, each user's shares, and hands each password over once", "[host-users]") {
  std::vector<su::user> users = {{"anna", su::role::view, std::vector<std::string>{"Photos"}, 1, 0},
                                 {"ben", su::role::change, std::nullopt, 1, 5}};
  std::vector<smb::folder> folders = {{"Photos", "/p"}, {"Home: Videos", "/v"}};
  std::vector<smb::password_for> pw;
  pw.push_back({"anna", "anna's password"});
  const auto r = smb::plan(users, folders, pw, {"cleo", "anna"});
  REQUIRE(r.shares.size() == 2);
  CHECK(r.shares[1].name == "Home Videos");
  REQUIRE(r.users.size() == 2);
  CHECK(r.users[0].shares == std::vector<std::string>{"Photos"});
  CHECK(r.users[0].password == "anna's password");
  CHECK(pw[0].password.empty());  // moved, and the copy left wiped
  CHECK(r.users[1].shares.size() == 2);
  CHECK(r.users[1].password.empty());
  CHECK(r.remove == std::vector<std::string>{"cleo"});  // anna is still a user: never removed
  smb::facts f;
  f.service_on = false;
  const auto lines = smb::explain(r, f);
  std::string all;
  for (const auto& l : lines) all += l + "\n";
  CHECK(contains(all, "anna (can view files, 1 folder)"));
  CHECK(contains(all, "never SMB1"));
  CHECK(contains(all, "never opened on the router"));
  CHECK(contains(all, "Turn on file sharing"));
  CHECK_FALSE(contains(all, "password'"));
  CHECK_FALSE(contains(all, "/p"));
}

TEST_CASE("host users: the command line's words; a password is never an argument", "[host-users]") {
  const auto words = [](std::vector<std::string> argv, std::string& why) {
    argv.insert(argv.begin(), {"host", "users"});
    const auto p = cli::parse_args(argv, true);
    cli::users_command c;
    why = p.error.empty() ? cli::parse_users_command(p.a, c) : p.error;
    return c;
  };
  std::string why;
  auto c = words({}, why);
  CHECK(why.empty());
  CHECK(c.verb == cli::users_verb::list);
  c = words({"add", "anna", "--role", "change", "--folders", "Photos, Home Videos"}, why);
  CHECK(why.empty());
  CHECK(c.verb == cli::users_verb::add);
  CHECK(c.r == su::role::change);
  REQUIRE(c.folders);
  REQUIRE(*c.folders);
  CHECK(**c.folders == std::vector<std::string>{"Photos", "Home Videos"});
  c = words({"role", "anna", "admin", "--folders", "all"}, why);
  CHECK(why.empty());
  REQUIRE(c.folders);
  CHECK_FALSE(*c.folders);
  c = words({"add", "anna", "hunter2hunter2"}, why);
  CHECK(contains(why, "never an argument"));
  c = words({"password", "anna", "hunter2hunter2"}, why);
  CHECK(contains(why, "never an argument"));
  c = words({"add", "Root"}, why);
  CHECK_FALSE(why.empty());
  c = words({"role", "anna", "boss"}, why);
  CHECK_FALSE(why.empty());
  c = words({"remove", "anna", "--role", "view"}, why);
  CHECK_FALSE(why.empty());
  // The options belong to host users alone.
  const auto p = cli::parse_args(std::vector<std::string>{"host", "status", "--role", "view"}, true);
  CHECK_FALSE(p.error.empty());
  const auto q = cli::parse_args(std::vector<std::string>{"send", "x", "--input-stdin"}, true);
  CHECK_FALSE(q.error.empty());
  const auto s = cli::parse_args(std::vector<std::string>{"host", "smb-apply"}, true);
  CHECK(s.error.empty());
  CHECK(s.a.host == cli::host_action::smb_apply);
}

TEST_CASE("host users: the admin passphrase signs in, and misses count toward the owner's lockout", "[host-users]") {
  scratch_dir dir("host_users_admin");
  auto st = store_in(dir.root() / "s");
  std::int64_t until = 0;
  CHECK(cli::check_admin(*st, kAdmin, 1000, until) == cli::admin_answer::none_set);
  const auto v = owner::make(kAdmin, 1, owner::kMinOps, owner::kMinMem);
  REQUIRE(v);
  REQUIRE(owner::save(*st, *v));
  CHECK(cli::check_admin(*st, kAdmin, 1000, until) == cli::admin_answer::ok);
  for (int i = 0; i < owner::kFreeMisses; ++i) {  // five free
    CHECK(cli::check_admin(*st, "not the passphrase", 1000, until) == cli::admin_answer::wrong);
  }
  CHECK(cli::check_admin(*st, "not the passphrase", 1000, until) == cli::admin_answer::locked);
  CHECK(until == 1000 + owner::kFirstLockSeconds);
  // Locked: even the right one waits, and the service's own counter says the same.
  CHECK(cli::check_admin(*st, kAdmin, 1001, until) == cli::admin_answer::locked);
  CHECK(owner::load_lockout(*st, 1001).owner.locked(1001));
  CHECK(cli::check_admin(*st, kAdmin, 1000 + owner::kFirstLockSeconds, until) == cli::admin_answer::ok);
  CHECK_FALSE(owner::load_lockout(*st, 1000 + owner::kFirstLockSeconds).owner.locked(1000 + owner::kFirstLockSeconds));
}

TEST_CASE("host users: add, change and remove through the admin sign-in and one elevated step each", "[host-users]") {
  host_fixture h;
  // A wrong passphrase changes nothing and never reaches the step.
  CHECK(h.run({"add", "anna"}, {"not the passphrase", "anna's password"}) == 1);
  CHECK(h.users().empty());
  CHECK(h.requests.empty());
  // The right one: the list kept, the account and the shares made, the stamp set.
  CHECK(h.run({"add", "anna", "--folders", "Photos"}, {kAdmin, "anna's password"}) == 0);
  auto list = h.users();
  REQUIRE(list.size() == 1);
  CHECK(list[0].r == su::role::view);
  CHECK(list[0].applied == h.clock);
  REQUIRE(h.requests.size() == 1);
  CHECK(contains(h.requests[0], "anna's password"));  // handed to the step, and only there
  CHECK(h.os.passwords["anna"] == "anna's password");
  REQUIRE(h.os.shares.count("Photos"));
  CHECK(h.os.shares.count("Home Videos") == 0);
  CHECK(h.os.shares.count(serve::kCatalogueLabel) == 0);  // the host library is never a network share
  CHECK_FALSE(contains(h.all_said(), "anna's password"));
  {
    auto st = h.store();
    // Nothing in the store holds it either.
    for (const char* item : {su::kUsersName, cli::kShareStateName}) {
      const auto bytes = st->get(item);
      REQUIRE(bytes);
      CHECK_FALSE(contains(std::string(bytes->begin(), bytes->end()), "anna's password"));
    }
    const auto lines = cli::users_status_lines(*st);
    std::string all;
    for (const auto& l : lines) all += l + "\n";
    CHECK(contains(all, "\\\\STUDIO\\Photos"));
    CHECK(contains(all, "smb://studio.local/Photos"));
    CHECK(contains(all, "anna (can view files)"));
    CHECK(contains(all, "never on the router"));
    CHECK_FALSE(contains(all, utf8(h.photos)));
  }
  // Twice the same name: refused.
  CHECK(h.run({"add", "anna"}, {kAdmin, "another password"}) == 1);
  // A new role for every folder: the step sets the shares again, no account made.
  h.os.calls.clear();
  CHECK(h.run({"role", "anna", "change", "--folders", "all"}, {kAdmin}) == 0);
  list = h.users();
  CHECK(list[0].r == su::role::change);
  CHECK_FALSE(list[0].folders);
  CHECK(std::find(h.os.calls.begin(), h.os.calls.end(), "make anna") == h.os.calls.end());
  CHECK(h.os.shares.count("Home Videos") == 1);
  // A new password.
  CHECK(h.run({"password", "anna"}, {kAdmin, "a newer password"}) == 0);
  CHECK(h.os.passwords["anna"] == "a newer password");
  // Removed: her shares and her account go.
  CHECK(h.run({"remove", "anna"}, {kAdmin}) == 0);
  CHECK(h.users().empty());
  CHECK(h.os.accounts.count("anna") == 0);
  CHECK(h.os.shares.empty());
  {
    auto st = h.store();
    const auto lines = cli::users_status_lines(*st);
    REQUIRE(lines.size() == 1);
    CHECK(contains(lines[0], "off"));
  }
}

TEST_CASE("host users: a declined administrator keeps the list waiting, and apply makes it so later", "[host-users]") {
  host_fixture h;
  auto e = h.env();
  bool decline = true;
  e.elevate = [&](std::string_view request) {
    cli::elevated_run r;
    if (decline) {
      r.declined = true;
      r.why = "the administrator's approval was cancelled";
      return r;
    }
    std::string why;
    auto q = smb::parse_request(request, why);
    REQUIRE(q);
    r.started = true;
    r.answer = smb::answer_json(smb::apply(*q, h.os));
    return r;
  };
  h.input = {kAdmin, "ben's password"};
  cli::users_command c;
  c.verb = cli::users_verb::add;
  c.name = "ben";
  c.input_stdin = true;
  CHECK(cli::run_users(c, h.l, e) == 0);
  auto list = h.users();
  REQUIRE(list.size() == 1);
  CHECK(list[0].applied == 0);
  {
    auto st = h.store();
    std::string all;
    for (const auto& l : cli::users_status_lines(*st)) all += l + "\n";
    CHECK(contains(all, "waiting for an administrator: ben"));
  }
  // The app's apply: the password as a NAME<TAB>PASSWORD line, no admin sign-in (the OS asks).
  decline = false;
  h.input = {"ben\tben's password"};
  h.next = 0;
  cli::users_command a;
  a.verb = cli::users_verb::apply;
  a.input_stdin = true;
  CHECK(cli::run_users(a, h.l, e) == 0);
  CHECK(h.users()[0].applied == h.clock);
  CHECK(h.os.passwords["ben"] == "ben's password");
  // A password that is too short, on stdin: refused before the step.
  h.input = {"ben\tshort"};
  h.next = 0;
  CHECK(cli::run_users(a, h.l, e) == 2);
}

TEST_CASE("host users: the step takes away its own accounts the request no longer lists, and strips no one else's entries",
          "[host-users]") {
  stand_in_port os;
  os.accounts["bob"] = stand_in_port::owner::theirs;  // a real account named like a user
  os.accounts["old"] = stand_in_port::owner::ours;    // removed in the app; its step was declined
  smb::request r;
  r.shares.push_back({"Photos", "/p"});
  r.users.push_back({"anna", su::role::view, {"Photos"}, "anna's password"});
  r.users.push_back({"bob", su::role::view, {"Photos"}, "bob's password"});
  const auto a = smb::apply(r, os);
  CHECK(os.accounts.count("old") == 0);  // taken away though nobody asked this time
  CHECK(std::find(os.calls.begin(), os.calls.end(), "delete old") != os.calls.end());
  CHECK(os.accounts["bob"] == stand_in_port::owner::theirs);
  // Only MediaViewer's accounts have entries removed from the folder: anna and old, never bob.
  CHECK(std::find(os.last_strip.begin(), os.last_strip.end(), "anna") != os.last_strip.end());
  CHECK(std::find(os.last_strip.begin(), os.last_strip.end(), "old") != os.last_strip.end());
  CHECK(std::find(os.last_strip.begin(), os.last_strip.end(), "bob") == os.last_strip.end());
  const auto it = std::find_if(a.users.begin(), a.users.end(), [](const smb::item& i) { return i.name == "old"; });
  REQUIRE(it != a.users.end());
  CHECK(it->ok);
}

TEST_CASE("host users: the step never shares a system folder, and takes only valid UTF-8 passwords", "[host-users]") {
  for (const char* p : {"/", "/System", "/System/Library", "/Library/Preferences", "/private/etc", "/usr/local", "/etc",
                        "/Applications", "/Users", "/Users/", "/Volumes", "/home", "/Users/x/../../etc"}) {
    INFO(p);
    CHECK_FALSE(smb::refuse_system_folder(p, false).empty());
  }
  for (const char* p : {"/Users/anna/Pictures", "/Volumes/Photos", "/home/anna/Photos", "/Users/anna"}) {
    INFO(p);
    CHECK(smb::refuse_system_folder(p, false).empty());
  }
  for (const char* p : {"C:\\", "c:/", "C:\\Windows", "C:\\Windows\\System32", "C:\\Program Files\\X",
                        "C:\\ProgramData", "C:\\Users", "c:\\users\\public"}) {
    INFO(p);
    CHECK_FALSE(smb::refuse_system_folder(p, true).empty());
  }
  for (const char* p : {"E:\\", "C:\\Users\\anna\\Pictures", "D:\\Photos"}) {
    INFO(p);
    CHECK(smb::refuse_system_folder(p, true).empty());
  }
  CHECK(smb::acceptable_password("p\xc3\xa4" "ssw\xc3\xb6" "rd"));  // "pässwörd"
  CHECK_FALSE(smb::acceptable_password("bad\xff\xfe" "bytes!"));
  CHECK_FALSE(smb::acceptable_password("over\xc0\xaf" "long!!"));
  CHECK_FALSE(smb::acceptable_password("cut off \xe2\x82"));
}
