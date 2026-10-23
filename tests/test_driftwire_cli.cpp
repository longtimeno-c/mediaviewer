// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire command line with its engine (docs/design/28 "From a
// terminal"): the arguments, the host, the program refusing to run without
// a terminal, and sessions over a fake terminal, two engines on the fixture
// bus, driven only by keys typed into them and read only from what their
// screens show: the receiver's screen shows the code, the test types it
// into the sender's, as the two people would.
#include "catch_compat.h"

#include <atomic>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/host.h"
#include "addons/driftwire/cli/session.h"
#include "addons/driftwire/cli/theme.h"
#include "driftwire_cli_vt.h"
#include "driftwire_fixture.h"

namespace cli = mv::driftwire::cli;
using mv::test::fake_console;
using mv::test::read_big_code;

namespace {

cli::parsed_args parse(std::vector<std::string> argv, bool test_hooks = false) {
  return cli::parse_args(std::span<const std::string>(argv), test_hooks);
}

bool has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

const cli::theme& test_theme() {
  static const cli::theme t = cli::make_theme(cli::color_depth::truecolor, true);
  return t;
}

// A link that sleeps before each write: a transfer slow enough to cancel.
class slow_link final : public sp::link {
 public:
  explicit slow_link(std::unique_ptr<sp::link> inner) : inner_(std::move(inner)) {}
  mv::expected write_all(std::span<const std::uint8_t> bytes) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return inner_->write_all(bytes);
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  sp::transport via() const noexcept override { return inner_->via(); }

 private:
  std::unique_ptr<sp::link> inner_;
};

// One person at a terminal: a session on its own thread over a fake console.
struct cli_side {
  fake_console con;
  cli::cli_host host;
  std::thread th;
  std::atomic<int> code{-1};
  std::atomic<bool> done{false};
  std::mutex m;
  std::condition_variable cv;
  std::vector<std::string> events;

  cli_side(const fs::path& received, int columns = 80, int rows = 48)
      : con(columns, rows), host(con, utf8(received)) {
    fs::create_directories(received);
  }
  cli_side(const cli_side&) = delete;
  cli_side& operator=(const cli_side&) = delete;
  ~cli_side() { stop(); }

  void start(sp::engine::options eo, cli::session_options so) {
    so.events = [this](const std::string& l) {
      {
        std::lock_guard lock(m);
        events.push_back(l);
      }
      cv.notify_all();
    };
    if (so.version.empty()) so.version = "9.9.9";
    th = std::thread([this, eo = std::move(eo), so = std::move(so)]() mutable {
      cli::session s(con, host, std::move(eo), std::move(so), test_theme());
      code = s.run();
      done = true;
      cv.notify_all();
    });
  }
  // The first event starting with `prefix` (its text after it), within `seconds`.
  std::optional<std::string> event(std::string_view prefix, int seconds = 20) {
    std::unique_lock lock(m);
    std::optional<std::string> hit;
    cv.wait_for(lock, std::chrono::seconds(seconds), [&] {
      for (const auto& e : events) {
        if (e.rfind(prefix, 0) == 0) {
          hit = e.substr(prefix.size());
          return true;
        }
      }
      return false;
    });
    return hit;
  }
  bool shows(std::string_view part, int seconds = 20) {
    const bool ok = con.wait_for([&](const std::string& s) { return has(s, part); }, seconds);
    if (!ok) UNSCOPED_INFO("not shown: " << part << "\n" << con.text());
    return ok;
  }
  bool screen_shows(std::string_view part, int seconds = 20) {
    const bool ok = con.wait_screen([&](const std::string& s) { return has(s, part); }, seconds);
    if (!ok) UNSCOPED_INFO("not on the screen: " << part << "\n" << con.screen());
    return ok;
  }
  // Waits for the session to end; its exit code.
  int wait_exit(int seconds = 20) {
    std::unique_lock lock(m);
    cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return done.load(); });
    lock.unlock();
    if (done && th.joinable()) th.join();
    return code;
  }
  void stop() {
    if (!th.joinable()) return;
    con.quit();
    th.join();
  }
  std::string text() const { return con.text(); }
};

cli::session_options session_opts(const fs::path& cwd) {
  cli::session_options so;
  so.paths.cwd = utf8(cwd);
  so.paths.home = "";
#if defined(_WIN32)
  so.paths.windows = true;
#endif
  return so;
}

// The engine of one side on the bus, as the fixture's clients (with a
// secret store and a data folder, so Remember and the prefs work).
sp::engine::options bus_opts(bus& b, const std::string& name, const fs::path& dir) {
  return client::options(b, name, dir / "secrets", dir / "data");
}

// Typed at the prompt, then Enter (as two reads, as a person types).
void type_line(cli_side& s, const std::string& line) {
  s.con.type(line);
  s.con.type("\r");
}

// Esc alone: the decoder waits ~30 ms to tell it from the start of a
// sequence (keys.h), so a person's next key comes later than that.
void press_esc(cli_side& s) {
  s.con.type("\x1B");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

// The receiver's screen shows the code: read it off, type it on the sender.
std::string code_on(cli_side& receiver) {
  std::string code;
  receiver.con.wait_cells([&](const std::vector<std::vector<std::string>>& cells) {
    code = read_big_code(cells);
    return !code.empty();
  });
  return code;
}

}  // namespace

// ==== the arguments ===================================================================================

TEST_CASE("driftwire cli args: the commands, their options, and nothing secret", "[driftwire][cli]") {
  {
    const auto p = parse({});
    REQUIRE(p.error.empty());
    CHECK(p.a.cmd == cli::command::session);
  }
  {
    const auto p = parse({"send", "a.jpg", "b c.jpg", "--to", "Ann's MacBook"});
    REQUIRE(p.error.empty());
    CHECK(p.a.cmd == cli::command::send);
    CHECK(p.a.paths == std::vector<std::string>{"a.jpg", "b c.jpg"});
    CHECK(p.a.to == "Ann's MacBook");
  }
  {
    const auto p = parse({"--save-to=/tmp/x", "receive", "--once", "--name", "Bea", "--plain", "--no-color"});
    REQUIRE(p.error.empty());
    CHECK(p.a.cmd == cli::command::receive);
    CHECK(p.a.once);
    CHECK(p.a.save_to == "/tmp/x");
    CHECK(p.a.name == "Bea");
    CHECK(p.a.plain);
    CHECK(p.a.no_color);
  }
  {
    const auto p = parse({"send", "--", "--odd-name.jpg"});
    REQUIRE(p.error.empty());
    CHECK(p.a.paths == std::vector<std::string>{"--odd-name.jpg"});
  }
  CHECK(parse({"code"}).a.cmd == cli::command::code);
  CHECK(parse({"devices"}).a.cmd == cli::command::devices);
  CHECK(parse({"--help"}).a.cmd == cli::command::help);
  CHECK(parse({"send", "x", "-h"}).a.cmd == cli::command::help);
  CHECK(parse({"help"}).a.cmd == cli::command::help);
  CHECK(parse({"--version"}).a.cmd == cli::command::version);
  CHECK(parse({"-V"}).a.cmd == cli::command::version);

  // Usage errors.
  CHECK(has(parse({"send"}).error, "send needs the files"));
  CHECK(has(parse({"photo.jpg"}).error, "is not a command"));
  CHECK(has(parse({"--bogus"}).error, "unknown option --bogus"));
  CHECK(has(parse({"--to"}).error, "--to needs a name"));
  CHECK(has(parse({"--to="}).error, "--to needs a name"));
  CHECK(has(parse({"receive", "--to", "Ann"}).error, "--to goes with send"));
  CHECK(has(parse({"send", "x", "--once"}).error, "--once goes with receive"));
  CHECK(has(parse({"receive", "extra"}).error, "receive takes no arguments"));
  CHECK(has(parse({"devices", "1"}).error, "devices takes no arguments"));
  CHECK(has(parse({"--plain=yes"}).error, "unknown option --plain"));

  // Never a secret on the command line: a code is refused as an argument,
  // and there is no option that takes one (nor --yes).
  CHECK(has(parse({"code", "482917"}).error, "code takes no arguments"));
  CHECK(has(parse({"send", "a.jpg", "--code", "482917"}).error, "unknown option --code"));
  CHECK(has(parse({"--yes"}).error, "unknown option --yes"));
  CHECK(has(parse({"--relay-server", "h:1#k"}).error, "unknown option --relay-server"));

  // The test build's options exist only there.
  CHECK(has(parse({"--test-loopback"}).error, "unknown option"));
  CHECK(has(parse({"--test-data", "/tmp"}).error, "unknown option"));
  {
    const auto p = parse({"code", "--test-loopback", "--test-data", "/tmp/d", "--test-events"}, true);
    REQUIRE(p.error.empty());
    CHECK(p.a.test_loopback);
    CHECK(p.a.test_data == "/tmp/d");
    CHECK(p.a.test_events);
  }

  CHECK(has(cli::help_text(), "driftwire send PATH..."));
  CHECK(has(cli::help_text(), "never arguments"));
}

// ==== the host and the platform port ===================================================================

TEST_CASE("driftwire cli host: a post only wakes the session; the log ring; pairs", "[driftwire][cli]") {
  fake_console con(40, 10);
  cli::cli_host host(con, "/somewhere");
  CHECK(host.default_destination() == "/somewhere");
  CHECK_FALSE(host.should_yield());
  CHECK(host.posts() == 0);
  host.post_tree("{}");
  host.post_tree("{}");
  CHECK(host.posts() == 2);
  cli::term_input in;
  con.wait(in, 0);
  CHECK(in.woken);
  // The terminal shows offers itself: the engine is told the notice was taken.
  CHECK(host.post_offer_notice("Driftwire", "Ann", 7));
  CHECK(host.post_status("x"));
  CHECK(host.keep_alive(true));

  for (std::size_t i = 0; i < cli::cli_host::kLogLines + 5; ++i) host.log(1, ("line " + std::to_string(i)).c_str());
  const auto lines = host.take_log();
  REQUIRE(lines.size() == cli::cli_host::kLogLines);
  CHECK(lines.front() == "line 5");
  CHECK(host.take_log().empty());

  // RAW + JPEG and Live Photo halves pair as the viewer's listing pairs them.
  const auto p = cli::pair_names({"IMG_1.CR2", "IMG_1.JPG", "IMG_2.HEIC", "IMG_2.MOV", "notes.txt"});
  REQUIRE(p.size() == 5);
  CHECK(p[0] == 1);
  CHECK(p[1] == 0);
  CHECK(p[2] == 3);
  CHECK(p[3] == 2);
  CHECK(p[4] == UINT32_MAX);

  scratch_dir s("driftwire_cli_host");
  CHECK(host.free_bytes(utf8(s.root())).has_value());
  CHECK_FALSE(host.free_bytes(utf8(s / "not-there")).has_value());
}

#if !defined(_WIN32)
TEST_CASE("driftwire cli data folder: XDG state, made for this user alone", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_data");
  const char* old_state = std::getenv("XDG_STATE_HOME");  // NOLINT
  const std::string kept = old_state ? old_state : "";
  ::setenv("XDG_STATE_HOME", utf8(s / "state").c_str(), 1);
  const std::string dir = cli::data_dir();
  if (old_state) {
    ::setenv("XDG_STATE_HOME", kept.c_str(), 1);
  } else {
    ::unsetenv("XDG_STATE_HOME");
  }
#if defined(__APPLE__)
  CHECK(has(dir, "/Library/Application Support/Driftwire"));
#else
  REQUIRE(dir == utf8(s / "state" / "driftwire"));
  struct stat st{};
  REQUIRE(::stat(dir.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);
  REQUIRE(::stat(utf8(s / "state").c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);
#endif
}
#endif

// ==== the program without a terminal ===================================================================

namespace {

// `exe args` with no terminal on either end: its exit code and everything it printed.
std::pair<int, std::string> run_piped(const std::string& exe, const std::string& args) {
#if defined(_WIN32)
  const std::string cmd = "\"\"" + exe + "\" " + args + " <NUL 2>&1\"";
  std::FILE* f = ::_popen(cmd.c_str(), "r");
#else
  const std::string cmd = "'" + exe + "' " + args + " </dev/null 2>&1";
  std::FILE* f = ::popen(cmd.c_str(), "r");
#endif
  REQUIRE(f);
  std::string out;
  char buf[512];
  while (const std::size_t n = std::fread(buf, 1, sizeof buf, f)) out.append(buf, n);
#if defined(_WIN32)
  const int code = ::_pclose(f);
#else
  const int st = ::pclose(f);
  const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
  return {code, out};
}

}  // namespace

TEST_CASE("driftwire cli program: no terminal, no session (exit 2); help and version need none",
          "[driftwire][cli]") {
  const std::string exe = MV_DRIFTWIRE_CLI_PATH;
  {
    const auto [code, out] = run_piped(exe, "");
    CHECK(code == 2);
    CHECK(has(out, "driftwire: this needs a terminal (it asks before anything is sent or saved)."));
  }
  {
    const auto [code, out] = run_piped(exe, "receive --once");
    CHECK(code == 2);
    CHECK(has(out, "this needs a terminal"));
  }
  {
    const auto [code, out] = run_piped(exe, "--version");
    CHECK(code == 0);
    CHECK(out.rfind("driftwire ", 0) == 0);
  }
  {
    const auto [code, out] = run_piped(exe, "--help");
    CHECK(code == 0);
    CHECK(has(out, "Usage:"));
  }
  {
    const auto [code, out] = run_piped(exe, "code 123456");
    CHECK(code == 2);
    CHECK(has(out, "code takes no arguments"));
  }
  {
    // The test build's options are not the program's.
    const auto [code, out] = run_piped(exe, "--test-loopback");
    CHECK(code == 2);
    CHECK(has(out, "unknown option --test-loopback"));
  }
}

// ==== sessions on the fixture bus =========================================================================

TEST_CASE("driftwire cli session: a send nearby, the code typed off the other screen, accepted, remembered",
          "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_send");
  bus b;
  const fs::path f = s / "out" / "IMG_0001.HEIC";
  write_bytes(f, pattern(300 * 1024 + 7, 3));
  cli_side ann(s / "ann_received"), bea(s / "bea_received");
  ann.start(bus_opts(b, "Ann", s / "ann"), session_opts(s.root()));
  bea.start(bus_opts(b, "Bea", s / "bea"), session_opts(s.root()));

  // The banner first, then Nearby.
  REQUIRE(ann.shows("Driftwire  9.9.9"));
  REQUIRE(ann.screen_shows("Bea"));
  REQUIRE(bea.screen_shows("Ann"));
  // A path typed (as a terminal pastes a dropped file): the set, in the hint.
  type_line(ann, utf8(f));
  REQUIRE(ann.screen_shows("1 file"));
  CHECK(has(ann.text(), "> " + utf8(f)));  // the line typed, in the transcript

  // Enter on an empty prompt sends to the computer the pointer is on.
  ann.con.type("\r");
  REQUIRE(ann.screen_shows("Type the code on Bea's screen"));
  REQUIRE(bea.screen_shows("Ann wants to send you files"));
  const std::string code = code_on(bea);
  REQUIRE(code.size() == 6);
  // The sender's screen never shows it.
  CHECK(read_big_code(ann.con.cells()).empty());
  ann.con.type_keys(code);  // it goes by itself at the sixth digit

  REQUIRE(bea.screen_shows("Ann wants to send you 1 file"));
  CHECK(bea.screen_shows("IMG_0001.HEIC"));
  CHECK(bea.screen_shows("1. Accept"));
  bea.con.type("1");
  REQUIRE(ann.shows("Sent 1 file to Bea."));
  REQUIRE(bea.shows("Received 1 file from Ann."));
  CHECK(has(bea.text(), "every file checked"));
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"IMG_0001.HEIC"});

  // Remember (both keep an identity here): asked on both sides; nearby,
  // the sender shows a code and the receiver types it (#497).
  REQUIRE(ann.screen_shows("Remember Bea?"));
  REQUIRE(bea.screen_shows("Remember Ann?"));
  ann.con.type("1");
  REQUIRE(ann.shows("Remembered Bea."));
  const std::string check = code_on(ann);
  REQUIRE(check.size() == 6);
  bea.con.type("1");
  REQUIRE(bea.screen_shows("type the code its window shows now"));
  bea.con.type_keys(check);
  REQUIRE(bea.shows("Remembered Ann."));
  press_esc(ann);  // the shown code goes

  type_line(ann, "/devices");
  REQUIRE(ann.shows("1. Bea"));

  type_line(ann, "/quit");
  type_line(bea, "/quit");
  CHECK(ann.wait_exit() == 0);
  CHECK(bea.wait_exit() == 0);
  CHECK(has(ann.con.lines().back(), "Driftwire closed"));
  CHECK(ann.con.errors().empty());
  CHECK(bea.con.errors().empty());
  // What happened stays in the scrollback; the live region is gone.
  CHECK_FALSE(has(ann.con.screen(), "Type or drop files to send"));
}

TEST_CASE("driftwire cli session: declined, and three wrong codes", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_decline");
  bus b;
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(4096, 1));
  cli_side ann(s / "ann_received"), bea(s / "bea_received");
  ann.start(client::options(b, "Ann"), session_opts(s.root()));
  bea.start(client::options(b, "Bea"), session_opts(s.root()));
  REQUIRE(ann.screen_shows("Bea"));
  type_line(ann, utf8(f));
  REQUIRE(ann.screen_shows("1 file"));

  SECTION("declined with n") {
    ann.con.type("\r");
    const std::string code = code_on(bea);
    REQUIRE(code.size() == 6);
    ann.con.type_keys(code);
    REQUIRE(bea.screen_shows("wants to send you 1 file"));
    bea.con.type("n");
    REQUIRE(ann.shows("Bea declined."));
    REQUIRE(bea.shows("Declined. Nothing was received."));
    CHECK(files_in(s / "bea_received").empty());
  }
  SECTION("the code wrong three times") {
    ann.con.type("\r");
    const std::string code = code_on(bea);
    REQUIRE(code.size() == 6);
    const std::string wrong = code == "000000" ? "111111" : "000000";
    ann.con.type_keys(wrong);
    REQUIRE(ann.screen_shows("2 tries left"));
    ann.con.type_keys(wrong);
    REQUIRE(ann.screen_shows("1 try left"));
    ann.con.type_keys(wrong);
    REQUIRE(ann.shows("The code was wrong 3 times"));
    CHECK(files_in(s / "bea_received").empty());
  }
  type_line(ann, "/quit");
  type_line(bea, "/quit");
  CHECK(ann.wait_exit() == 0);
  CHECK(bea.wait_exit() == 0);
}

TEST_CASE("driftwire cli session: esc twice cancels a send under way; the folder offer", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_cancel");
  bus b;
  cli_side ann(s / "ann_received"), bea(s / "bea_received");

  SECTION("cancel") {
    const fs::path f = s / "out" / "big.mov";
    write_bytes(f, pattern(24 * 1024 * 1024, 5));
    auto ao = client::options(b, "Ann");
    ao.connect_wifi = [](const sp::net::endpoint& to, int ms) -> mv::result<std::unique_ptr<sp::link>> {
      auto l = sp::net::connect(to, ms);
      if (!l) return mv::err(l.error());
      return std::unique_ptr<sp::link>(std::make_unique<slow_link>(std::move(*l)));
    };
    ann.start(std::move(ao), session_opts(s.root()));
    bea.start(client::options(b, "Bea"), session_opts(s.root()));
    REQUIRE(ann.screen_shows("Bea"));
    type_line(ann, utf8(f));
    REQUIRE(ann.screen_shows("1 file"));
    ann.con.type("\r");
    const std::string code = code_on(bea);
    REQUIRE(code.size() == 6);
    ann.con.type_keys(code);
    REQUIRE(bea.screen_shows("wants to send you 1 file"));
    bea.con.type("\r");  // Enter on the first choice: Accept
    REQUIRE(ann.screen_shows("esc to cancel"));
    REQUIRE(ann.screen_shows(" of 25 MB"));
    press_esc(ann);
    REQUIRE(ann.screen_shows("Press esc again to cancel"));
    press_esc(ann);
    REQUIRE(ann.shows("Cancelled."));
    REQUIRE(bea.shows("Ann cancelled."));
  }
  SECTION("a folder") {
    const fs::path dir = s / "Trip";
    write_bytes(dir / "one.jpg", pattern(1000, 1));
    write_bytes(dir / "day 2" / "two.jpg", pattern(2000, 2));
    ann.start(client::options(b, "Ann"), session_opts(s.root()));
    bea.start(client::options(b, "Bea"), session_opts(s.root()));
    REQUIRE(ann.screen_shows("Bea"));
    // Relative to the current folder, quoted as a shell would.
    type_line(ann, "'Trip'");
    REQUIRE(ann.screen_shows("the folder \"Trip\""));
    // A file on top of a folder: a send is one or the other.
    type_line(ann, utf8(dir / "one.jpg"));
    REQUIRE(ann.shows("A send is either files or one folder. /clear first."));
    ann.con.type("\r");
    const std::string code = code_on(bea);
    REQUIRE(code.size() == 6);
    ann.con.type_keys(code);
    REQUIRE(bea.screen_shows("the folder \"Trip\""));
    bea.con.type("y");
    REQUIRE(ann.shows("Sent 2 files to Bea."));
    REQUIRE(bea.shows("Received 2 files from Ann."));
    CHECK(files_in(s / "bea_received") == std::vector<std::string>{"Trip/day 2/two.jpg", "Trip/one.jpg"});
  }
  type_line(ann, "/quit");
  type_line(bea, "/quit");
  CHECK(ann.wait_exit() == 0);
  CHECK(bea.wait_exit() == 0);
}

TEST_CASE("driftwire cli session: send --to finds the name; receive --once ends with the transfer",
          "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_to");
  bus b;
  const fs::path f = s / "out" / "clip.mov";
  write_bytes(f, pattern(70000, 4));
  cli_side ann(s / "ann_received"), bea(s / "bea_received");
  auto so = session_opts(s.root());
  so.cmd = cli::command::send;
  so.send_paths = {utf8(f)};
  so.to = "bea";  // any case
  ann.start(client::options(b, "Ann"), so);
  auto bo = session_opts(s.root());
  bo.cmd = cli::command::receive;
  bo.once = true;
  bea.start(client::options(b, "Bea"), bo);
  // Nobody presses Enter on Ann: the send starts once Bea is heard.
  REQUIRE(ann.screen_shows("Type the code on Bea's screen"));
  const std::string code = code_on(bea);
  REQUIRE(code.size() == 6);
  ann.con.type_keys(code);
  REQUIRE(bea.screen_shows("wants to send you 1 file"));
  bea.con.type("y");
  // Both are one-shots: they close by themselves, 0 for a transfer that went through.
  CHECK(ann.wait_exit() == 0);
  CHECK(bea.wait_exit() == 0);
  CHECK(has(ann.text(), "Sent 1 file to Bea."));
  CHECK(has(bea.text(), "Received 1 file from Ann."));
  CHECK(has(ann.con.lines().back(), "Driftwire closed"));
}

TEST_CASE("driftwire cli session: receive --once declined is exit 1; Ctrl+C twice; a quit signal",
          "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_exit");
  bus b;
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(5000, 4));
  cli_side ann(s / "ann_received"), bea(s / "bea_received");
  ann.start(client::options(b, "Ann"), session_opts(s.root()));
  auto bo = session_opts(s.root());
  bo.cmd = cli::command::receive;
  bo.once = true;
  bea.start(client::options(b, "Bea"), bo);
  REQUIRE(ann.screen_shows("Bea"));
  type_line(ann, utf8(f));
  REQUIRE(ann.screen_shows("1 file"));
  ann.con.type("\r");
  const std::string code = code_on(bea);
  REQUIRE(code.size() == 6);
  ann.con.type_keys(code);
  REQUIRE(bea.screen_shows("wants to send you 1 file"));
  press_esc(bea);  // esc declines an offer
  CHECK(bea.wait_exit() == 1);
  REQUIRE(ann.shows("Bea declined."));

  // Ctrl+C: a line typed is cleared first; then twice quits.
  ann.con.type("abc");
  REQUIRE(ann.screen_shows("> abc"));
  ann.con.type("\x03");
  REQUIRE(ann.screen_shows("Type or drop files to send"));
  ann.con.type("\x03");
  REQUIRE(ann.screen_shows("Press Ctrl+C again to quit"));
  ann.con.type("\x03");
  CHECK(ann.wait_exit() == 0);

  // SIGTERM / the console closing: an orderly close.
  cli_side cy(s / "cy_received");
  cy.start(client::options(b, "Cy"), session_opts(s.root()));
  REQUIRE(cy.shows("Driftwire  9.9.9"));
  cy.con.quit();
  CHECK(cy.wait_exit() == 0);
  CHECK(has(cy.con.lines().back(), "Driftwire closed"));
}

TEST_CASE("driftwire cli session: plain mode asks in lines", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_plain");
  bus b;
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(5000, 4));
  cli_side ann(s / "ann_received"), bea(s / "bea_received");
  auto ao = session_opts(s.root());
  ao.plain = true;
  ann.start(client::options(b, "Ann"), ao);
  auto bo = session_opts(s.root());
  bo.plain = true;
  bea.start(client::options(b, "Bea"), bo);
  REQUIRE(ann.shows("Nearby: 1. Bea"));
  REQUIRE(ann.shows("driftwire>"));
  type_line(ann, utf8(f));
  REQUIRE(ann.shows("Enter sends 1 file"));
  ann.con.type("\r");
  REQUIRE(ann.shows("Type the code on Bea's screen:"));
  // The receiver's code is a line of text, not a picture.
  std::string code;
  REQUIRE(bea.con.wait_for([&](const std::string& t) {
    const auto at = t.find("Type this code on Ann: ");
    if (at == std::string::npos) return false;
    code = t.substr(at + 23, 7);
    return true;
  }));
  type_line(ann, code);
  REQUIRE(bea.shows("Accept 1 file (5 KB) from Ann? [y/N/s]"));
  type_line(bea, "y");
  REQUIRE(ann.shows("Sent 1 file to Bea."));
  REQUIRE(bea.shows("Received 1 file from Ann."));
  type_line(ann, "/quit");
  type_line(bea, "/quit");
  CHECK(ann.wait_exit() == 0);
  CHECK(bea.wait_exit() == 0);
  // Plain mode moves no cursor: nothing but text, CR, LF, BS and colour resets.
  for (const auto& osc : ann.con.osc()) CHECK(osc.empty());
  CHECK(ann.con.errors().empty());
}

namespace {

// The fixture's far client, as a session's engine: no one nearby, codes
// over loopback.
sp::engine::options far_opts(const std::string& name, const fs::path& data) {
  sp::engine::options o;
  o.my_name = name;
  o.use_bluetooth = false;
  o.loopback_only = true;
  o.make_discovery = [] { return std::make_unique<inject_discovery>(); };
  o.internet_loopback = true;
  o.internet_mapping = std::nullopt;
  fs::create_directories(data);
  o.prefs_path = utf8(data / sp::kPrefsFile);
  return o;
}

}  // namespace

TEST_CASE("driftwire cli session: a code over the internet (loopback), the check compared, accepted",
          "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_code");
  const fs::path f = s / "out" / "clip.mov";
  write_bytes(f, pattern(2 * 1024 * 1024 + 3, 6));
  cli_side ann(s / "ann_received"), bea(s / "bea_received");
  auto bo = session_opts(s.root());
  bo.cmd = cli::command::code;
  bea.start(far_opts("Bea", s / "bea"), bo);
  ann.start(far_opts("Ann", s / "ann"), session_opts(s.root()));

  // `driftwire code`: Over the internet is off; it asks to turn it on for good.
  REQUIRE(bea.screen_shows("Receiving from anywhere needs Over the internet."));
  bea.con.type("y");
  REQUIRE(bea.screen_shows("Receive from anywhere"));
  const auto code = bea.event("code ");
  REQUIRE(code);
  REQUIRE(bea.screen_shows("Give this code to the person sending"));
  // The card shows the code itself (its first group, at least).
  CHECK(bea.screen_shows(code->substr(0, 4)));
  CHECK(bea.text().find("Send and receive over the internet") != std::string::npos);  // what it means, once

  type_line(ann, utf8(f));
  REQUIRE(ann.screen_shows("1 file"));
  type_line(ann, "/to-code");
  REQUIRE(ann.screen_shows("Sending to a code needs Over the internet."));
  ann.con.type("y");
  REQUIRE(ann.screen_shows("to a code"));
  ann.con.type("\x1B[200~" + *code + "\x1B[201~");  // pasted
  ann.con.type("\r");
  REQUIRE(ann.screen_shows("Connected to Bea (new device)"));
  REQUIRE(ann.screen_shows("It matches"));
  // The check: the same six digits on both screens.
  const auto check = ann.event("out naming ");
  REQUIRE(check);
  REQUIRE(bea.screen_shows("Ann (new device) wants to send you files"));
  CHECK(bea.screen_shows(*check));
  ann.con.type("y");
  REQUIRE(bea.screen_shows("wants to send you 1 file"));
  bea.con.type("y");
  REQUIRE(ann.shows("Sent 1 file to Bea."));
  REQUIRE(bea.shows("Received 1 file from Ann."));
  CHECK(has(bea.text(), "internet"));
  // `code` is a one-shot: it closes with the transfer, 0.
  CHECK(bea.wait_exit() == 0);
  type_line(ann, "/quit");
  CHECK(ann.wait_exit() == 0);
  CHECK(files_in(s / "bea_received") == std::vector<std::string>{"clip.mov"});
}
