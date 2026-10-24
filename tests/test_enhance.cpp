// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance, E1 (#297, docs/plans/enhance.md): the catalogue and its rules, the
// spec rule on several machine classes, the download store against a fake
// Hugging Face that records every request (nothing before the click or after
// a restart, the gates before any GET, resume, wrong hashes, adoption, prune,
// the lock and what waits for it, no transport, two windows over one data
// folder, a Remove in another window), and the Models page through the screen
// vocabulary.
// Inline catalogues shaped like tests/data/enhance/fixture-models.json: a
// large GPU-only model, a standard GPU-or-CPU one and a small CPU one.
#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "addon/screen.h"
#include "addons/ai/sha256.h"
#include "addons/enhance/catalog.h"
#include "addons/enhance/choose.h"
#include "addons/enhance/downloads.h"
#include "addons/enhance/lock.h"
#include "addons/enhance/machine.h"
#include "addons/enhance/view.h"
#include "core/json.h"

namespace fs = std::filesystem;
namespace en = mv::enhance;
namespace scr = mv::addon::screen;

namespace {

constexpr std::uint64_t kGiB = 1ull << 30;
constexpr std::uint64_t kMiB = 1ull << 20;

std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

struct scratch {
  fs::path root;
  explicit scratch(const char* name) {
    std::random_device rd;
    root = fs::temp_directory_path() / (std::string("mv_enhance_") + name + "_" + std::to_string(rd()));
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  [[nodiscard]] std::string dir() const { return utf8(root); }
};

std::vector<std::uint8_t> bytes(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 31);
  return v;
}

std::string hex_of(const std::vector<std::uint8_t>& b) {
  mv::ai::sha256 h;
  h.update(b);
  return h.finish_hex();
}

void put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

void put(const fs::path& path, const std::vector<std::uint8_t>& data) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string replace_once(std::string s, const std::string& from, const std::string& to) {
  const std::size_t at = s.find(from);
  REQUIRE(at != std::string::npos);
  return s.replace(at, from.size(), to);
}

// ---- an inline catalogue ----------------------------------------------------------------

const std::string kIndex = R"({"schema":1,"kinds":{
 "still":{"name":"Photos","unit":"a 12 MP photo","cpu_budget_ms":60000,"models":["large","standard","small"]},
 "video":{"name":"Video","unit":"a second of 1080p video","cpu_budget_ms":30000,"models":[]},
 "audio":{"name":"Audio","unit":"a minute of sound","cpu_budget_ms":20000,"models":["small"]}}})";

const std::string kLarge = R"({"id":"large","name":"Large","model":"Swin2SR x4","kinds":["still"],
 "run_bytes":6442450944,"cache_bytes":0,"accelerators":["cuda","coreml"],"licence":"Apache-2.0",
 "knobs":[{"id":"scale","label":"Scale","type":"choice","default":"2",
  "options":[{"id":"2","text":"2x"},{"id":"4","text":"4x"}],"kinds":["still"]}]})";

// The top-level "kinds" comes before the knobs' so replace_once finds it.
const std::string kStandard = R"({"id":"standard","name":"Standard","model":"Real-ESRGAN x4plus","kinds":["still"],
 "run_bytes":2147483648,"cache_bytes":0,"accelerators":["cuda","coreml"],"cpu_unit_ms":45000,
 "licence":"BSD-3-Clause","knobs":[
  {"id":"scale","label":"Scale","type":"choice","default":"4",
   "options":[{"id":"2","text":"2x"},{"id":"4","text":"4x"}],"kinds":["still"]},
  {"id":"denoise","label":"Denoise","type":"slider","min":0,"max":1,"step":0.05,"default":0.5,"kinds":["still"]}]})";

const std::string kSmall = R"({"id":"small","name":"Small","model":"realesr-general-x4v3","kinds":["still","audio"],
 "run_bytes":268435456,"cache_bytes":0,"accelerators":[],"cpu_unit_ms":5000,"licence":"MIT",
 "knobs":[{"id":"denoise","label":"Denoise","type":"slider","min":0,"max":1,"step":0.05,"default":0.5,
  "kinds":["still"]},
  {"id":"voice","label":"Keep voices clear","type":"toggle","default":true,"kinds":["audio"]}]})";

struct pin {
  std::string path;
  std::uint64_t size = 0;
  std::string sha256;
  std::string licence = "MIT";
};

std::string url_of(const std::string& path) {
  return "https://huggingface.co/org/repo/resolve/" + std::string(40, 'a') + "/" + path;
}

std::string manifest_of(const std::vector<pin>& pins) {
  mv::json::writer w;
  w.begin_object().key("id").string("enhance").key("remote").begin_array();
  for (const pin& p : pins) {
    w.begin_object().key("url").string(url_of(p.path)).key("path").string(p.path);
    w.key("size").integer(static_cast<std::int64_t>(p.size)).key("sha256").string(p.sha256);
    w.key("licence").string(p.licence).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

// The figures of the fixture: 1.27 GB, 39 MB and 0.2 MB.
std::vector<pin> spec_pins() {
  return {{"models/large/model.onnx", 1274342603, std::string(64, 'a')},
          {"models/standard/model.onnx", 38696353, std::string(64, 'b')},
          {"models/small/model.onnx", 232589, std::string(64, 'c')}};
}

en::catalog parse(const std::map<std::string, std::string>& models, const std::vector<pin>& pins,
                  const std::string& index = kIndex) {
  const auto remote = en::read_remote(manifest_of(pins));
  auto c = en::parse_catalog(
      index,
      [&models](std::string_view id) -> std::optional<std::string> {
        auto it = models.find(std::string(id));
        if (it == models.end()) return std::nullopt;
        return it->second;
      },
      remote);
  REQUIRE(c.has_value());
  return std::move(c).value();
}

std::map<std::string, std::string> three() {
  return {{"large", kLarge}, {"standard", kStandard}, {"small", kSmall}};
}

en::catalog spec_catalog() { return parse(three(), spec_pins()); }

// ---- machine classes ----------------------------------------------------------------------

en::machine pc(std::uint64_t ram, const std::string& accelerator = "", std::uint64_t vram = 0,
               const std::string& gpu = "") {
  en::machine m;
  m.os = "windows";
  m.arch = "x86_64";
  m.cores = 8;
  m.ram_total = ram;
  m.ram_free = ram / 2;
  m.accelerator = accelerator;
  m.gpu = gpu;
  m.vram = vram;
  m.free_disk = 500 * kGiB;
  return m;
}

en::machine mac(std::uint64_t ram, const std::string& chip = "Apple M2 Pro") {
  en::machine m;
  m.os = "macos";
  m.arch = "arm64";
  m.cores = 10;
  m.ram_total = ram;
  m.ram_free = ram / 2;
  m.accelerator = "apple";
  m.gpu = chip;
  m.free_disk = 500 * kGiB;
  return m;
}

en::machine intel_mac(std::uint64_t ram) {
  en::machine m = mac(ram, "");
  m.arch = "x86_64";
  m.accelerator.clear();
  return m;
}

std::string recommended(const en::catalog& c, en::kind k, const en::machine& here) {
  const en::model* m = en::recommend(c, k, here, {});
  return m ? m->id : std::string("(none)");
}

en::fit fit_of(const en::catalog& c, const char* id, const en::machine& here, en::kind k = en::kind::still) {
  const en::model* m = c.find(id);
  REQUIRE(m);
  return en::offer(*m, c.of(k), here, m->download_bytes());
}

// ---- a fake Hugging Face ------------------------------------------------------------------

struct fake_hf {
  struct call {
    std::string url;
    std::uint64_t from;
  };
  std::mutex m;
  std::map<std::string, std::vector<std::uint8_t>> files;  // by URL
  std::vector<call> calls;
  int made = 0;  // transports made
  std::atomic<int> chunk_ms{0};  // a slow line: this long before each chunk
  // Per request (0-based): the status and how many body bytes to send before
  // cutting; a negative cut sends everything.
  std::function<std::pair<int, long long>(std::size_t n, std::uint64_t from)> script =
      [](std::size_t, std::uint64_t from) { return std::pair<int, long long>{from ? 206 : 200, -1}; };

  std::vector<call> seen() {
    std::lock_guard lock(m);
    return calls;
  }
};

struct proxy final : mv::ai::http_transport {
  fake_hf& hf;
  explicit proxy(fake_hf& f) : hf(f) {}
  mv::ai::http_reply get(const std::string& url, std::uint64_t from, const std::function<bool(int)>& head,
                         const std::function<bool(std::span<const std::uint8_t>)>& body) override {
    std::pair<int, long long> plan;
    std::vector<std::uint8_t> data;
    {
      std::lock_guard lock(hf.m);
      const std::size_t n = hf.calls.size();
      hf.calls.push_back({url, from});
      plan = hf.script(n, from);
      if (auto it = hf.files.find(url); it != hf.files.end()) data = it->second;
    }
    mv::ai::http_reply r;
    if (plan.first == 0) {
      r.error = "offline";
      return r;
    }
    r.status = plan.first;
    if (!head(plan.first) || (plan.first != 200 && plan.first != 206)) return r;
    const std::uint64_t start = plan.first == 206 ? from : 0;
    std::uint64_t end = data.size();
    if (plan.second >= 0) end = std::min<std::uint64_t>(end, start + static_cast<std::uint64_t>(plan.second));
    for (std::uint64_t at = start; at < end; at += 997) {
      if (const int ms = hf.chunk_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      const std::uint64_t k = std::min<std::uint64_t>(997, end - at);
      if (!body(std::span<const std::uint8_t>(data.data() + at, k))) break;
    }
    return r;
  }
};

// A store over a scratch data folder, a fake Hugging Face and small real
// files for the three models (so the hashes are real).
struct rig {
  scratch s{"store"};
  fake_hf hf;
  std::map<std::string, std::vector<std::uint8_t>> body;  // by model id
  std::vector<pin> pins;
  en::catalog cat;
  en::machine here = pc(16 * kGiB);  // CPU only: large needs a GPU
  bool no_transport = false;
  std::atomic<bool> installed{true};  // the add-on, as another window's Remove leaves it
  std::chrono::milliseconds choices_wait{2000};
  std::chrono::milliseconds prune_retry{30'000};
  std::chrono::milliseconds lock_retry{5000};
  std::atomic<int> changes{0};
  // Set while a test holds the fetcher in its wait (an outage).
  std::function<bool(double, const std::atomic<bool>&)> wait = [](double, const std::atomic<bool>& stop) {
    return !stop.load();
  };
  std::unique_ptr<en::model_store> store;

  rig() {
    body["large"] = bytes(7000, 1);
    body["standard"] = bytes(5000, 2);
    body["small"] = bytes(3000, 3);
    repin();
  }
  ~rig() { store.reset(); }

  // The pins and the fake's files from `body` again, and the catalogue.
  void repin() {
    pins.clear();
    for (const auto& [id, b] : body) add("models/" + id + "/model.onnx", b);
    cat = parse(three(), pins);
  }
  void add(const std::string& path, const std::vector<std::uint8_t>& b) {
    pins.push_back({path, b.size(), hex_of(b)});
    hf.files[url_of(path)] = b;
  }

  // A store as a window makes one; a second over the same folder is
  // another window's (each is its own process).
  std::unique_ptr<en::model_store> make() {
    en::store_options o;
    o.background = false;  // a test runner shares its CPUs
    o.fetch.first_backoff_s = 1;
    o.fetch.wait = [this](double secs, const std::atomic<bool>& stop) { return wait(secs, stop); };
    o.still_installed = [this] { return installed.load(); };
    o.choices_wait = choices_wait;
    o.prune_retry = prune_retry;
    o.lock_retry = lock_retry;
    return std::make_unique<en::model_store>(
        s.dir(), cat,
        [this]() -> std::unique_ptr<mv::ai::http_transport> {
          std::lock_guard lock(hf.m);
          ++hf.made;
          if (no_transport) return nullptr;
          return std::make_unique<proxy>(hf);
        },
        [this] { return here; }, [this] { ++changes; }, o);
  }
  void open() { store = make(); }

  [[nodiscard]] fs::path weights(const std::string& id) const {
    return s.root / "cache" / "models" / id / "model.onnx";
  }
  en::kind_status status(en::kind k) const { return store->status()[static_cast<std::size_t>(k)]; }
  void settle() const { REQUIRE(store->wait_idle(std::chrono::seconds(20))); }
};

// Waits (bounded) for a condition another thread makes true.
bool eventually(const std::function<bool()>& what) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < until) {
    if (what()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return what();
}

std::size_t files_under(const fs::path& root) {
  std::error_code ec;
  if (!fs::exists(root, ec)) return 0;
  std::size_t n = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) n += e.is_regular_file() ? 1 : 0;
  return n;
}

}  // namespace

// ---- the catalogue ----------------------------------------------------------------------

TEST_CASE("the catalogue reads its kinds, models and pins", "[enhance]") {
  for (en::kind k : en::kKinds) CHECK(en::kind_from(en::kind_id(k)) == k);
  CHECK(!en::kind_from("photo"));

  std::vector<pin> pins = spec_pins();
  pins.push_back({"models/standard/extra.bin", 10, std::string(64, 'd')});
  pins.push_back({"models/standard-old/model.onnx", 99, std::string(64, 'e')});  // another folder
  pins.push_back({"models/unlisted/model.onnx", 5, std::string(64, 'f')});
  const en::catalog c = parse(three(), pins);

  CHECK(c.of(en::kind::still).name == "Photos");
  CHECK(c.of(en::kind::still).unit == "a 12 MP photo");
  CHECK(c.of(en::kind::still).cpu_budget_ms == 60000);
  CHECK(c.of(en::kind::still).models == std::vector<std::string>{"large", "standard", "small"});
  CHECK(c.of(en::kind::video).models.empty());
  CHECK(c.of(en::kind::audio).models == std::vector<std::string>{"small"});
  REQUIRE(c.models.size() == 3);
  CHECK(!c.find("unlisted"));

  const en::model* s = c.find("standard");
  REQUIRE(s);
  CHECK(s->name == "Standard");
  CHECK(s->model_name == "Real-ESRGAN x4plus");
  CHECK(s->licence == "BSD-3-Clause");
  CHECK(s->run_bytes == 2147483648u);
  CHECK(s->cuda);
  CHECK(s->coreml);
  CHECK(s->cpu_unit_ms == std::optional<std::uint64_t>(45000));
  REQUIRE(s->files.size() == 2);  // its own folder only
  CHECK(s->files[0].path == "models/standard/model.onnx");
  CHECK(s->files[1].path == "models/standard/extra.bin");
  CHECK(s->files[0].url == url_of("models/standard/model.onnx"));
  CHECK(s->download_bytes() == 38696353u + 10u);
  REQUIRE(s->knobs.size() == 2);
  CHECK(s->knobs[0].type == "choice");
  CHECK(s->knobs[0].default_value.s == "4");
  REQUIRE(s->knobs[0].options.size() == 2);
  CHECK(s->knobs[0].options[1].text == "4x");
  CHECK(s->knobs[1].type == "slider");
  CHECK(s->knobs[1].max == 1.0);
  CHECK(s->knobs[1].step == 0.05);
  CHECK(s->knobs[1].default_value.d == 0.5);

  const en::model* small = c.find("small");
  REQUIRE(small);
  CHECK(!small->cuda);
  CHECK(small->serves(en::kind::audio));
  CHECK(small->knobs[1].type == "toggle");
  CHECK(small->knobs[1].default_value.b);
  CHECK(small->knobs[1].kinds == std::vector<en::kind>{en::kind::audio});
  CHECK(!c.find("large")->cpu_unit_ms);
}

TEST_CASE("a model that breaks a rule of the catalogue check is left out", "[enhance]") {
  struct breakage {
    const char* why;
    std::string from, to;
  };
  const std::vector<breakage> cases = {
      {"its id is not its folder's", R"("id":"standard")", R"("id":"other")"},
      {"an unknown field", R"("run_bytes")", R"("extra":1,"run_bytes")"},
      {"an empty name", R"("name":"Standard")", R"("name":"")"},
      {"no model text", R"("model":"Real-ESRGAN x4plus",)", ""},
      {"no kinds", R"("kinds":["still"])", R"("kinds":[])"},
      {"an unknown kind", R"("kinds":["still"])", R"("kinds":["photo"])"},
      {"a kind twice", R"("kinds":["still"])", R"("kinds":["still","still"])"},
      {"run_bytes 0", R"("run_bytes":2147483648)", R"("run_bytes":0)"},
      {"fractional run_bytes", R"("run_bytes":2147483648)", R"("run_bytes":2147483648.5)"},
      {"run_bytes as text", R"("run_bytes":2147483648)", R"("run_bytes":"2 GB")"},
      {"negative cache_bytes", R"("cache_bytes":0)", R"("cache_bytes":-1)"},
      {"an unknown accelerator", R"(["cuda","coreml"])", R"(["metal"])"},
      {"an accelerator twice", R"(["cuda","coreml"])", R"(["cuda","cuda"])"},
      {"no accelerators field", R"("accelerators":["cuda","coreml"],)", ""},
      {"cpu_unit_ms 0", R"("cpu_unit_ms":45000)", R"("cpu_unit_ms":0)"},
      {"runs nowhere", R"("accelerators":["cuda","coreml"],"cpu_unit_ms":45000,)", R"("accelerators":[],)"},
      {"a licence outside the allow-list", R"("BSD-3-Clause")", R"("CC-BY-NC-4.0")"},
      {"a knob id with a capital", R"("id":"scale")", R"("id":"Scale")"},
      {"a knob id twice", R"("id":"denoise")", R"("id":"scale")"},
      {"a knob without a label", R"("label":"Scale",)", ""},
      {"an unknown knob type", R"("type":"slider")", R"("type":"dial")"},
      {"a field another type has", R"("type":"slider",)", R"("type":"slider","options":[],)"},
      {"a knob for a kind the model does not serve", R"("default":0.5,"kinds":["still"])",
       R"("default":0.5,"kinds":["audio"])"},
      {"a slider with min not below max", R"("min":0,"max":1)", R"("min":1,"max":1)"},
      {"a slider step of 0", R"("step":0.05)", R"("step":0)"},
      {"a slider step wider than its range", R"("step":0.05)", R"("step":2)"},
      {"a slider default outside its range", R"("default":0.5)", R"("default":1.5)"},
      {"a choice default that is no option", R"("default":"4")", R"("default":"8")"},
      {"an option without text", R"({"id":"2","text":"2x"})", R"({"id":"2"})"},
      {"an option twice", R"({"id":"2","text":"2x"})", R"({"id":"4","text":"2x"})"},
      {"not JSON", R"("kinds")", R"(kinds)"},
  };
  for (const breakage& b : cases) {
    INFO(b.why);
    auto models = three();
    models["standard"] = replace_once(kStandard, b.from, b.to);
    const en::catalog c = parse(models, spec_pins());
    CHECK(!c.find("standard"));
    CHECK(c.of(en::kind::still).models == std::vector<std::string>{"large", "small"});
    CHECK(c.find("large"));
  }

  SECTION("knobs that are not a list") {
    auto models = three();
    models["standard"] = kStandard.substr(0, kStandard.find("\"knobs\":")) + "\"knobs\":{}}";
    const en::catalog c = parse(models, spec_pins());
    CHECK(!c.find("standard"));
    models["standard"] = kStandard.substr(0, kStandard.find(",\"knobs\":")) + "}";  // none at all is fine
    CHECK(parse(models, spec_pins()).find("standard"));
  }
  SECTION("a toggle whose default is not true or false") {
    auto models = three();
    models["small"] = replace_once(kSmall, R"("default":true)", R"("default":1)");
    const en::catalog c = parse(models, spec_pins());
    CHECK(!c.find("small"));
    CHECK(c.of(en::kind::audio).models.empty());
  }
  SECTION("no model.json") {
    auto models = three();
    models.erase("standard");
    CHECK(!parse(models, spec_pins()).find("standard"));
  }
  SECTION("nothing in its folder to fetch") {
    std::vector<pin> pins = spec_pins();
    pins.erase(pins.begin() + 1);
    CHECK(!parse(three(), pins).find("standard"));
  }
}

TEST_CASE("a kind drops the ids it cannot use, and an unsafe id is never read", "[enhance]") {
  const std::string index = R"({"schema":1,"kinds":{
   "still":{"name":"Photos","unit":"u","cpu_budget_ms":60000,"models":["ghost","standard","standard","../x","small"]},
   "audio":{"name":"Audio","unit":"u","cpu_budget_ms":20000,"models":["large","small"]}}})";
  std::vector<std::string> asked;
  const auto models = three();
  auto c = en::parse_catalog(
      index,
      [&](std::string_view id) -> std::optional<std::string> {
        asked.emplace_back(id);
        auto it = models.find(std::string(id));
        if (it == models.end()) return std::nullopt;
        return it->second;
      },
      en::read_remote(manifest_of(spec_pins())));
  REQUIRE(c.has_value());
  CHECK(c->of(en::kind::still).models == std::vector<std::string>{"standard", "small"});
  CHECK(c->of(en::kind::audio).models == std::vector<std::string>{"small"});  // large serves stills only
  CHECK(c->of(en::kind::video).name == "Video");                              // not in the index: empty
  CHECK(c->of(en::kind::video).models.empty());
  CHECK(std::find(asked.begin(), asked.end(), "../x") == asked.end());
  CHECK(std::count(asked.begin(), asked.end(), "standard") == 1);

  CHECK(!en::parse_catalog("not json", {}, {}).has_value());
  CHECK(!en::parse_catalog(R"({"schema":2,"kinds":{}})", {}, {}).has_value());
  CHECK(!en::parse_catalog(R"({"schema":1})", {}, {}).has_value());
}

TEST_CASE("the manifest's remote list skips malformed entries", "[enhance]") {
  const std::string sha(64, 'a');
  const std::string text = R"({"remote":[
    {"url":"https://huggingface.co/a/resolve/x/f","path":"models/a/f","size":5,"sha256":")" + sha + R"(","licence":"MIT"},
    {"path":"models/a/g","size":5,"sha256":")" + sha + R"(","licence":"MIT"},
    {"url":"u","path":"models/../g","size":5,"sha256":")" + sha + R"(","licence":"MIT"},
    {"url":"u","path":"/models/a/g","size":5,"sha256":")" + sha + R"(","licence":"MIT"},
    {"url":"u","path":"models/a/g","size":0,"sha256":")" + sha + R"(","licence":"MIT"},
    {"url":"u","path":"models/a/g","size":5,"sha256":")" + std::string(64, 'A') + R"(","licence":"MIT"},
    {"url":"u","path":"models/a/g","size":5,"sha256":")" + sha + R"("}]})";
  const auto remote = en::read_remote(text);
  REQUIRE(remote.size() == 1);
  CHECK(remote[0].path == "models/a/f");
  CHECK(remote[0].size == 5);
  CHECK(remote[0].licence == "MIT");
  CHECK(en::read_remote("[]").empty());
}

TEST_CASE("a remote URL the host's rule refuses is skipped", "[enhance]") {
  const auto kept = [](const std::string& url) {
    mv::json::writer w;
    w.begin_object().key("remote").begin_array().begin_object().key("url").string(url);
    w.key("path").string("models/a/model.onnx").key("size").integer(5).key("sha256").string(std::string(64, 'a'));
    w.key("licence").string("MIT").end_object().end_array().end_object();
    return en::read_remote(w.take()).size();
  };
  const std::string base = "https://huggingface.co/org/repo/resolve/" + std::string(40, 'a') + "/";
  CHECK(kept(base + "model.onnx") == 1);
  CHECK(kept(base + std::string(1024 - base.size(), 'm')) == 1);  // 1024 bytes at most
  const std::vector<std::string> refused = {
      base + std::string(1025 - base.size(), 'm'),
      "http://huggingface.co/org/repo/resolve/x/model.onnx",
      "https://huggingface.co.example.com/org/repo/resolve/x/model.onnx",
      "https://example.com/org/repo/resolve/x/model.onnx",
      "https://huggingface.co/",
      "https://huggingface.co/org/repo/blob/x/model.onnx",  // not a /resolve/ link
      base + "model.onnx?download=true",
      base + "model.onnx#top",
      "https://huggingface.co/org@evil/repo/resolve/x/model.onnx",
      base + "dir\\model.onnx",
      base + "model onnx",
      base + "model\x01.onnx",
      base + "model\x7f.onnx",
      base + "mod\xC3\xA9l.onnx",  // not ASCII
      base + "x/../../../other/model.onnx",
      "",
  };
  for (const std::string& url : refused) {
    INFO(url);
    CHECK(kept(url) == 0);
  }
}

TEST_CASE("a version folder's catalogue, and one with none", "[enhance]") {
  scratch s("version");
  auto empty = en::read_catalog(s.dir());
  REQUIRE(empty.has_value());
  CHECK(empty->models.empty());
  CHECK(empty->of(en::kind::still).name == "Photos");
  CHECK(empty->of(en::kind::video).name == "Video");
  CHECK(empty->of(en::kind::audio).name == "Audio");
  for (const en::kind_entry& e : empty->kinds) CHECK(e.models.empty());

  put(s.root / "models" / "enhance.json", kIndex);
  put(s.root / "models" / "standard" / "model.json", kStandard);
  put(s.root / "models" / "small" / "model.json", kSmall);
  put(s.root / "manifest.json", manifest_of(spec_pins()));
  auto c = en::read_catalog(s.dir());
  REQUIRE(c.has_value());
  CHECK(c->of(en::kind::still).models == std::vector<std::string>{"standard", "small"});  // large has no model.json
  CHECK(c->find("standard")->files.size() == 1);

  put(s.root / "models" / "enhance.json", std::string("{not json"));
  CHECK(!en::read_catalog(s.dir()).has_value());
}

// ---- the rule ---------------------------------------------------------------------------

TEST_CASE("the rule recommends by this machine's spec", "[enhance]") {
  const en::catalog c = spec_catalog();
  const en::kind still = en::kind::still;

  SECTION("a CPU-only PC with 8 GB") {
    const en::machine m = pc(8 * kGiB);
    CHECK(fit_of(c, "large", m).reason == en::why::needs_gpu);
    const en::fit standard = fit_of(c, "standard", m);
    CHECK(standard.ok());
    CHECK(standard.on == en::provider::cpu);  // 45 s within the 60 s budget
    CHECK(fit_of(c, "small", m).on == en::provider::cpu);
    CHECK(recommended(c, still, m) == "standard");
    CHECK(recommended(c, en::kind::audio, m) == "small");
    CHECK(recommended(c, en::kind::video, m) == "(none)");
    // Within a 30 s budget the standard model needs a GPU: the small one it is.
    en::catalog tight = c;
    tight.kinds[0].cpu_budget_ms = 30000;
    CHECK(fit_of(tight, "standard", m).reason == en::why::needs_gpu);
    CHECK(recommended(tight, still, m) == "small");
  }
  SECTION("an NVIDIA PC: 12 GB of VRAM, 32 GB of RAM") {
    const en::machine m = pc(32 * kGiB, "nvidia", 12 * kGiB, "NVIDIA GeForce RTX 4070");
    const en::fit large = fit_of(c, "large", m);
    CHECK(large.ok());
    CHECK(large.on == en::provider::cuda);
    CHECK(recommended(c, still, m) == "large");
    CHECK(fit_of(c, "small", m).on == en::provider::cpu);  // lists no accelerator
  }
  SECTION("an NVIDIA PC with 4 GB of VRAM: memory is the card's") {
    const en::machine m = pc(64 * kGiB, "nvidia", 4 * kGiB);
    const en::fit large = fit_of(c, "large", m);
    CHECK(large.reason == en::why::memory);
    CHECK(large.need == 6 * kGiB);
    CHECK(large.have == 4 * kGiB);
    CHECK(recommended(c, still, m) == "standard");
    // A card too small for a model the CPU runs in time: the CPU, from RAM.
    en::catalog wide = c;
    wide.models[1].run_bytes = 6 * kGiB;  // standard: CUDA, or 45 s on the CPU
    const en::fit cpu = fit_of(wide, "standard", m);
    CHECK(cpu.ok());
    CHECK(cpu.on == en::provider::cpu);
    CHECK(en::run_gate(*wide.find("standard"), wide.of(still), m).on == en::provider::cpu);
    // RAM short too: the card's shortfall is the answer.
    const en::fit short_both = fit_of(wide, "standard", pc(4 * kGiB, "nvidia", 4 * kGiB));
    CHECK(short_both.reason == en::why::memory);
    CHECK(short_both.need == 6 * kGiB);
    CHECK(short_both.have == 4 * kGiB);
  }
  SECTION("Apple silicon with 16 GB") {
    const en::machine m = mac(16 * kGiB);
    const en::fit large = fit_of(c, "large", m);
    CHECK(large.ok());
    CHECK(large.on == en::provider::coreml);
    CHECK(recommended(c, still, m) == "large");
    // A model needing 24 GB does not fit: memory is RAM on a Mac.
    en::catalog big = c;
    big.models[0].run_bytes = 24 * kGiB;
    const en::fit f = fit_of(big, "large", m);
    CHECK(f.reason == en::why::memory);
    CHECK(f.need == 24 * kGiB);
    CHECK(f.have == 16 * kGiB);
    CHECK(recommended(big, still, m) == "standard");
  }
  SECTION("Apple silicon with 64 GB") {
    en::catalog big = c;
    big.models[0].run_bytes = 24 * kGiB;
    CHECK(recommended(big, still, mac(64 * kGiB)) == "large");
  }
  SECTION("an Intel Mac: nothing runs") {
    const en::machine m = intel_mac(32 * kGiB);
    for (const char* id : {"large", "standard", "small"}) CHECK(fit_of(c, id, m).reason == en::why::no_runtime);
    CHECK(recommended(c, still, m) == "(none)");
  }
  SECTION("too little disk: the bytes still to fetch plus the provider's cache") {
    en::machine m = pc(16 * kGiB);
    m.free_disk = 20'000'000;  // the standard model is 39 MB
    const en::fit f = fit_of(c, "standard", m);
    CHECK(f.reason == en::why::disk);
    CHECK(f.need == 38696353u);
    CHECK(f.have == 20'000'000u);
    CHECK(recommended(c, still, m) == "small");
    // What is already here does not count; a cache does.
    CHECK(en::offer(*c.find("standard"), c.of(still), m, 1000).ok());
    en::catalog cached = c;
    cached.models[1].cache_bytes = 20'000'000;
    CHECK(en::offer(*cached.find("standard"), c.of(still), m, 1000).reason == en::why::disk);
    // recommend() asks how much is left to fetch.
    CHECK(en::recommend(c, still, m, [](const en::model&) -> std::uint64_t { return 0; })->id == "standard");
  }
  SECTION("a pin over 5 GB is refused on any machine; 5 GB itself is not") {
    en::catalog big = c;
    big.models[0].files[0].size = 5'000'000'001;
    CHECK(fit_of(big, "large", pc(256 * kGiB, "nvidia", 48 * kGiB)).reason == en::why::too_large);
    CHECK(fit_of(big, "large", mac(192 * kGiB)).reason == en::why::too_large);
    big.models[0].files[0].size = en::kMaxWeightsFile;
    CHECK(fit_of(big, "large", mac(192 * kGiB)).ok());
  }
  SECTION("the run gate counts what is free now, not the total") {
    en::machine m = mac(16 * kGiB);
    m.ram_free = 1 * kGiB;
    const en::fit large = en::run_gate(*c.find("large"), c.of(still), m);
    CHECK(large.reason == en::why::memory);
    CHECK(large.have == 1 * kGiB);
    CHECK(en::run_gate(*c.find("small"), c.of(still), m).ok());
    m.free_disk = 0;  // no disk rule
    CHECK(en::run_gate(*c.find("small"), c.of(still), m).ok());
    const en::machine nv = pc(32 * kGiB, "nvidia", 12 * kGiB);
    CHECK(en::run_gate(*c.find("large"), c.of(still), nv).on == en::provider::cuda);
  }
}

// ---- the store --------------------------------------------------------------------------

TEST_CASE("nothing is requested before the click, nor after a restart until Resume", "[enhance]") {
  rig r;
  r.open();
  CHECK(r.hf.made == 0);
  for (const en::kind_status& s : r.store->status()) CHECK(s.state == en::download_state::none);
  CHECK(r.store->wait_idle(std::chrono::milliseconds(0)));
  r.store.reset();

  // A choice saved by an earlier run, its file never finished.
  put(r.s.root / "enhance.json", std::string(R"({"schema":1,"models":{"still":"standard","video":"nope"}})"));
  put(fs::path(r.weights("standard")) += ".part", std::vector<std::uint8_t>(r.body["standard"].begin(),
                                                                            r.body["standard"].begin() + 1200));
  r.open();
  const en::kind_status st = r.status(en::kind::still);
  CHECK(st.model == "standard");
  CHECK(st.state == en::download_state::stopped);
  CHECK(st.done_bytes == 1200);
  CHECK(st.total_bytes == 5000);
  CHECK(r.status(en::kind::video).state == en::download_state::none);  // not a model it offers
  CHECK(!r.store->ready(en::kind::still));
  CHECK(r.hf.made == 0);
  CHECK(r.hf.seen().empty());

  r.store->resume(en::kind::still);
  r.settle();
  CHECK(r.store->ready(en::kind::still));
  CHECK(r.status(en::kind::still).state == en::download_state::ready);
  const auto calls = r.hf.seen();
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].url == url_of("models/standard/model.onnx"));  // the pinned URL, nothing added
  CHECK(calls[0].from == 1200);                                 // the rest, with Range
}

TEST_CASE("Download fetches, verifies and keeps the choice", "[enhance]") {
  rig r;
  r.open();
  CHECK(!r.store->download(en::kind::still, "nope"));
  CHECK(!r.store->download(en::kind::video, "standard"));  // not a video model
  CHECK(r.hf.made == 0);

  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  CHECK(r.store->ready(en::kind::still));
  const en::kind_status st = r.status(en::kind::still);
  CHECK(st.state == en::download_state::ready);
  CHECK(st.done_bytes == 5000);
  CHECK(st.total_bytes == 5000);
  CHECK(r.changes.load() > 0);
  CHECK(fs::file_size(r.weights("standard")) == 5000);
  CHECK(fs::exists(fs::path(r.weights("standard")) += ".ok"));
  CHECK(read(r.s.root / "enhance.json").find(R"("still":"standard")") != std::string::npos);
  CHECK(r.hf.seen().size() == 1);

  // Clicked again, or after a restart: here, so nothing is asked.
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  r.store.reset();
  r.open();
  CHECK(r.status(en::kind::still).state == en::download_state::ready);
  CHECK(r.hf.seen().size() == 1);
}

TEST_CASE("a cut body resumes with a Range request", "[enhance]") {
  rig r;
  r.hf.script = [](std::size_t n, std::uint64_t from) {
    return std::pair<int, long long>{from ? 206 : 200, n == 0 ? 1500 : -1};
  };
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  CHECK(r.store->ready(en::kind::still));
  const auto calls = r.hf.seen();
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].from == 0);
  CHECK(calls[1].from == 1500);
}

TEST_CASE("a body that misses its hash three times fails and is deleted", "[enhance]") {
  rig r;
  r.hf.files[url_of("models/standard/model.onnx")] = bytes(5000, 99);  // right size, wrong bytes
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  CHECK(r.status(en::kind::still).state == en::download_state::failed);
  CHECK(r.hf.seen().size() == 3);
  CHECK(!fs::exists(r.weights("standard")));
  CHECK(!fs::exists(fs::path(r.weights("standard")) += ".part"));
  CHECK(!r.store->ready(en::kind::still));
}

TEST_CASE("a file dropped at its pinned path is adopted with no request", "[enhance]") {
  rig r;
  put(r.weights("standard"), r.body["standard"]);
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  CHECK(r.store->ready(en::kind::still));
  CHECK(r.hf.seen().empty());
}

TEST_CASE("a new choice prunes the old model's files once it is here; Delete prunes the kind's", "[enhance]") {
  rig r;
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  REQUIRE(r.store->download(en::kind::audio, "small"));
  r.settle();
  REQUIRE(fs::exists(r.weights("standard")));

  // Photos change to the small model, which audio already has: no request,
  // and the standard model's files go.
  const std::size_t before = r.hf.seen().size();
  REQUIRE(r.store->download(en::kind::still, "small"));
  r.settle();
  CHECK(r.hf.seen().size() == before);
  CHECK(r.store->ready(en::kind::still));
  CHECK(!fs::exists(r.weights("standard")));
  CHECK(!fs::exists(r.weights("standard").parent_path()));
  CHECK(fs::exists(r.weights("small")));

  // Delete for photos keeps what audio uses; then audio's goes too.
  r.store->remove(en::kind::still);
  r.settle();
  CHECK(r.status(en::kind::still).state == en::download_state::none);
  CHECK(fs::exists(r.weights("small")));
  r.store->remove(en::kind::audio);
  r.settle();
  CHECK(files_under(r.s.root / "cache" / "models") == 0);
  CHECK(read(r.s.root / "enhance.json").find("small") == std::string::npos);
  CHECK(fs::exists(r.s.root / "enhance.json"));  // the data folder itself is the add-on's
}

TEST_CASE("Cancel stops a run, Resume continues it, a new choice replaces it", "[enhance]") {
  rig r;
  // The first request is cut; the fetcher then waits for an outage to pass
  // until the test lets it go.
  r.hf.script = [](std::size_t n, std::uint64_t from) {
    return std::pair<int, long long>{from ? 206 : 200, n == 0 ? 2000 : -1};
  };
  std::atomic<bool> hold{true};
  r.wait = [&hold](double, const std::atomic<bool>& stop) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (hold.load() && !stop.load() && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !stop.load();
  };
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  REQUIRE(eventually([&] { return r.status(en::kind::still).state == en::download_state::waiting; }));
  const en::kind_status waiting = r.status(en::kind::still);
  CHECK(waiting.done_bytes == 2000);
  CHECK(waiting.retry_in_s == 1);

  r.store->cancel(en::kind::still);
  r.settle();
  CHECK(r.status(en::kind::still).state == en::download_state::stopped);
  CHECK(r.hf.seen().size() == 1);

  hold = false;
  r.store->resume(en::kind::still);
  r.settle();
  CHECK(r.store->ready(en::kind::still));
  auto calls = r.hf.seen();
  REQUIRE(calls.size() == 2);
  CHECK(calls[1].from == 2000);

  // A new choice while the old one waits: that run stops, the new one runs,
  // and the old one's files go once the new one is here.
  rig q;
  q.hf.script = r.hf.script;
  hold = true;
  q.wait = r.wait;
  q.open();
  REQUIRE(q.store->download(en::kind::still, "standard"));
  REQUIRE(eventually([&] { return q.status(en::kind::still).state == en::download_state::waiting; }));
  REQUIRE(q.store->download(en::kind::still, "small"));
  q.settle();
  CHECK(q.status(en::kind::still).model == "small");
  CHECK(q.store->ready(en::kind::still));
  CHECK(!fs::exists(fs::path(q.weights("standard")) += ".part"));
  hold = false;
}

TEST_CASE("the gates refuse before any request", "[enhance]") {
  SECTION("a pin over 5 GB") {
    rig r;
    r.cat.models[0].files[0].size = 5'000'000'001;  // large
    r.here = pc(64 * kGiB, "nvidia", 24 * kGiB);
    r.open();
    REQUIRE(r.store->download(en::kind::still, "large"));
    r.settle();
    const en::kind_status st = r.status(en::kind::still);
    CHECK(st.state == en::download_state::refused);
    CHECK(st.reason == en::why::too_large);
    CHECK(r.hf.made == 0);
    CHECK(r.hf.seen().empty());
  }
  SECTION("more than the free disk") {
    rig r;
    r.here.free_disk = 1000;
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    const en::kind_status st = r.status(en::kind::still);
    CHECK(st.state == en::download_state::no_space);
    CHECK(st.need == 5000);
    CHECK(st.have == 1000);
    CHECK(r.hf.seen().empty());
    CHECK(!fs::exists(r.weights("standard").parent_path()));
  }
  SECTION("a model this machine cannot run") {
    rig r;  // CPU only
    r.open();
    REQUIRE(r.store->download(en::kind::still, "large"));
    r.settle();
    const en::kind_status st = r.status(en::kind::still);
    CHECK(st.state == en::download_state::unfit);
    CHECK(st.reason == en::why::needs_gpu);
    CHECK(r.hf.made == 0);
  }
  SECTION("another window holds the lock") {
    rig r;
    r.lock_retry = std::chrono::milliseconds(20);
    fs::create_directories(r.s.root / "cache");
    const std::string lock = utf8(r.s.root / "cache" / "download.lock");
    CHECK(!en::is_held(lock));  // no such file: no one, and the probe makes none
    CHECK(!fs::exists(lock));
    auto held = en::try_lock(lock);
    REQUIRE(held);
    CHECK(!en::try_lock(lock));  // exclusive, even within one process
    CHECK(en::is_held(lock));
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::other_window);
    CHECK(r.status(en::kind::still).lock_wait);
    CHECK(r.hf.seen().empty());
    held.reset();
    // That window let go: this one carries on by itself.
    REQUIRE(eventually([&r] { return r.store->ready(en::kind::still); }));
    CHECK(r.hf.seen().size() == 1);
  }
  SECTION("no transport on this platform") {
    rig r;
    r.no_transport = true;
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::unsupported);
    CHECK(r.hf.seen().empty());
  }
}

TEST_CASE("another window's download reads as such here, and a kind it held off carries on after", "[enhance]") {
  SECTION("a choice not fetched here, while another window holds the lock") {
    rig r;
    put(r.s.root / "enhance.json", std::string(R"({"schema":1,"models":{"still":"standard"}})"));
    r.open();
    CHECK(r.status(en::kind::still).state == en::download_state::stopped);
    CHECK(!fs::exists(r.s.root / "cache"));  // looked at, nothing made
    fs::create_directories(r.s.root / "cache");
    {
      const auto held = en::try_lock(r.store->lock_path());  // another window, downloading
      REQUIRE(held);
      const en::kind_status st = r.status(en::kind::still);
      CHECK(st.state == en::download_state::other_window);
      CHECK(!st.lock_wait);  // nothing queued here: Resume would queue it
      CHECK(r.status(en::kind::audio).state == en::download_state::none);
    }
    CHECK(r.status(en::kind::still).state == en::download_state::stopped);
    CHECK(r.hf.made == 0);
    CHECK(r.store->wait_idle(std::chrono::milliseconds(0)));
  }
  SECTION("kinds refused for the lock run once it frees: one the other window fetched, one it did not") {
    rig r;
    r.lock_retry = std::chrono::milliseconds(20);
    fs::create_directories(r.s.root / "cache");
    auto held = en::try_lock(utf8(r.s.root / "cache" / "download.lock"));
    REQUIRE(held);
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    REQUIRE(r.store->download(en::kind::audio, "small"));
    r.settle();
    for (en::kind k : {en::kind::still, en::kind::audio}) {
      CHECK(r.status(k).state == en::download_state::other_window);
      CHECK(r.status(k).lock_wait);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // a few looks, each held off
    CHECK(r.status(en::kind::still).state == en::download_state::other_window);
    CHECK(r.hf.seen().empty());
    // The other window finishes the photo model, then lets go.
    put(r.weights("standard"), r.body["standard"]);
    const int before = r.changes.load();
    held.reset();
    REQUIRE(eventually([&r] { return r.store->ready(en::kind::still) && r.store->ready(en::kind::audio); }));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::ready);
    CHECK(!r.status(en::kind::still).lock_wait);
    const auto calls = r.hf.seen();
    REQUIRE(calls.size() == 1);  // the photo model adopted by its hash, the audio one fetched
    CHECK(calls[0].url == url_of("models/small/model.onnx"));
    CHECK(r.changes.load() > before);  // the page hears of it
  }
  SECTION("Cancel stops the wait, and Resume runs it again") {
    rig r;
    r.lock_retry = std::chrono::milliseconds(20);
    fs::create_directories(r.s.root / "cache");
    auto held = en::try_lock(utf8(r.s.root / "cache" / "download.lock"));
    REQUIRE(held);
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    REQUIRE(r.status(en::kind::still).lock_wait);
    r.store->cancel(en::kind::still);
    CHECK(r.status(en::kind::still).state == en::download_state::other_window);  // still held there
    CHECK(!r.status(en::kind::still).lock_wait);
    held.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(r.status(en::kind::still).state == en::download_state::stopped);
    CHECK(r.hf.made == 0);
    r.store->resume(en::kind::still);
    r.settle();
    CHECK(r.store->ready(en::kind::still));
  }
  SECTION("this store's own run is never another window's, and looking never gets in its way") {
    rig r;
    put(r.s.root / "enhance.json", std::string(R"({"schema":1,"models":{"audio":"small"}})"));
    r.body["standard"] = bytes(40'000, 2);  // 41 chunks
    r.repin();
    r.hf.chunk_ms = 5;
    r.open();
    std::atomic<bool> done{false};
    std::atomic<int> other{0};
    std::thread page([&] {  // a page asking all the while
      while (!done.load()) {
        if (r.store->status()[2].state == en::download_state::other_window) ++other;
      }
    });
    for (int round = 0; round < 3; ++round) {
      REQUIRE(r.store->download(en::kind::still, round % 2 ? "small" : "standard"));
      r.settle();
      CHECK(r.status(en::kind::still).state == en::download_state::ready);
    }
    done = true;
    page.join();
    CHECK(other.load() == 0);
    CHECK(r.status(en::kind::audio).state != en::download_state::other_window);
  }
}

TEST_CASE("a 404 on the pinned file is gone", "[enhance]") {
  rig r;
  r.hf.script = [](std::size_t, std::uint64_t) { return std::pair<int, long long>{404, -1}; };
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  CHECK(r.status(en::kind::still).state == en::download_state::gone);
  CHECK(r.hf.seen().size() == 1);
}

TEST_CASE("two windows share the choices: neither's click or prune drops the other's model", "[enhance]") {
  rig r;
  r.open();             // window A
  auto b = r.make();    // window B, its store made before A chooses
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  REQUIRE(b->download(en::kind::audio, "small"));
  REQUIRE(b->wait_idle(std::chrono::seconds(20)));
  std::string saved = read(r.s.root / "enhance.json");
  CHECK(saved.find(R"("still":"standard")") != std::string::npos);
  CHECK(saved.find(R"("audio":"small")") != std::string::npos);
  CHECK(fs::exists(r.weights("standard")));
  CHECK(r.store->ready(en::kind::still));
  CHECK(b->chosen(en::kind::still) == "standard");  // B took A's choice as it saved its own
  {
    auto fresh = r.make();  // the next launch
    CHECK(fresh->chosen(en::kind::still) == "standard");
    CHECK(fresh->chosen(en::kind::audio) == "small");
  }
  // A's page, opened again, shows B's choice.
  CHECK(r.store->chosen(en::kind::audio).empty());
  r.store->reload_choices();
  CHECK(r.store->chosen(en::kind::audio) == "small");
  CHECK(r.status(en::kind::audio).state == en::download_state::ready);

  // B's Delete of Audio keeps A's Photos, and A's next open clears Audio.
  REQUIRE(b->remove(en::kind::audio));
  REQUIRE(b->wait_idle(std::chrono::seconds(20)));
  CHECK(fs::exists(r.weights("standard")));
  CHECK(!fs::exists(r.weights("small")));
  saved = read(r.s.root / "enhance.json");
  CHECK(saved.find(R"("still":"standard")") != std::string::npos);
  CHECK(saved.find("audio") == std::string::npos);
  r.store->reload_choices();
  CHECK(r.store->chosen(en::kind::audio).empty());
  CHECK(r.store->chosen(en::kind::still) == "standard");

  // And the other way round: A's new Photos model, then B's click, which
  // writes back no stale Photos choice of its own.
  REQUIRE(r.store->download(en::kind::still, "small"));
  r.settle();
  CHECK(!fs::exists(r.weights("standard")));  // A's old choice, once the new one is here
  REQUIRE(b->download(en::kind::audio, "small"));
  REQUIRE(b->wait_idle(std::chrono::seconds(20)));
  saved = read(r.s.root / "enhance.json");
  CHECK(saved.find(R"("still":"small")") != std::string::npos);
  CHECK(saved.find(R"("audio":"small")") != std::string::npos);
  CHECK(b->chosen(en::kind::still) == "small");
  CHECK(r.store->ready(en::kind::still));
  CHECK(r.hf.seen().size() == 3);  // standard; small for B; small again for A, after B's Delete
}

TEST_CASE("a click while another window saves its choice, or after its Remove, changes nothing", "[enhance]") {
  rig r;
  r.choices_wait = std::chrono::milliseconds(50);
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  const std::string before = read(r.s.root / "enhance.json");
  {
    const auto held = en::try_lock(r.store->choices_lock_path());  // another window, saving
    REQUIRE(held);
    const auto d = r.store->download(en::kind::audio, "small");
    REQUIRE(!d);
    CHECK(d.error() == mv::status::busy);
    const auto rm = r.store->remove(en::kind::still);
    REQUIRE(!rm);
    CHECK(rm.error() == mv::status::busy);
    r.store->reload_choices();  // keeps what it has
  }
  CHECK(read(r.s.root / "enhance.json") == before);
  CHECK(r.status(en::kind::audio).state == en::download_state::none);
  CHECK(r.status(en::kind::still).state == en::download_state::ready);
  CHECK(r.store->wait_idle(std::chrono::milliseconds(0)));

  // Removed with its data in another window: nothing is saved, nothing
  // runs, and no folder is made again.
  fs::remove_all(r.s.root);
  const auto d = r.store->download(en::kind::audio, "small");
  REQUIRE(!d);
  CHECK(d.error() == mv::status::io);
  CHECK(!r.store->remove(en::kind::still));
  r.store->resume(en::kind::still);
  r.settle();
  CHECK(r.status(en::kind::still).state == en::download_state::stopped);
  CHECK(r.status(en::kind::audio).state == en::download_state::none);
  CHECK(r.hf.made == 1);
  CHECK(!fs::exists(r.s.root));
}

TEST_CASE("a prune another window's download holds off is owed, and runs once it can", "[enhance]") {
  rig r;
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  REQUIRE(r.store->download(en::kind::audio, "small"));
  r.settle();
  const auto delete_held_off = [&r] {
    const auto held = en::try_lock(r.store->lock_path());  // another window, downloading
    REQUIRE(held);
    REQUIRE(r.store->remove(en::kind::still));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::none);
    CHECK(fs::exists(r.weights("standard")));  // owed
    r.store->reload_choices();
    r.settle();
    CHECK(fs::exists(r.weights("standard")));  // still held off
  };
  SECTION("at the next page open") {
    delete_held_off();
    r.store->reload_choices();
    r.settle();
    CHECK(!fs::exists(r.weights("standard")));
    CHECK(!fs::exists(r.weights("standard").parent_path()));
    CHECK(fs::exists(r.weights("small")));
  }
  SECTION("after the next run here, even one the rule refuses") {
    delete_held_off();
    REQUIRE(r.store->download(en::kind::still, "large"));  // needs a graphics card
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::unfit);
    CHECK(!fs::exists(r.weights("standard")));
    CHECK(fs::exists(r.weights("small")));
  }
}

TEST_CASE("an owed prune is tried again on the worker until it runs", "[enhance]") {
  rig r;
  r.prune_retry = std::chrono::milliseconds(20);
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  {
    const auto held = en::try_lock(r.store->lock_path());
    REQUIRE(held);
    REQUIRE(r.store->remove(en::kind::still));
    r.settle();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // a few tries, each held off
    CHECK(fs::exists(r.weights("standard")));
  }
  CHECK(eventually([&r] { return !fs::exists(r.weights("standard")); }));
  CHECK(r.store->wait_idle(std::chrono::seconds(20)));
}

TEST_CASE("a Remove in another window stops a run here, and nothing makes its folders again", "[enhance]") {
  SECTION("before the gates") {
    rig r;
    r.installed = false;
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::stopped);
    CHECK(r.hf.made == 0);
    CHECK(!fs::exists(r.s.root / "cache"));
  }
  SECTION("between files") {
    rig r;
    r.add("models/standard/more/extra.bin", bytes(4000, 7));
    r.cat = parse(three(), r.pins);
    r.hf.script = [&r](std::size_t, std::uint64_t from) {
      r.installed = false;  // removed while the first file arrives
      return std::pair<int, long long>{from ? 206 : 200, -1};
    };
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::stopped);
    CHECK(r.hf.seen().size() == 1);
    CHECK(!fs::exists(r.s.root / "cache" / "models" / "standard" / "more"));
  }
  SECTION("while bytes move") {
    rig r;
    r.body["standard"] = bytes(40'000, 2);  // 41 chunks, 0.1 s each
    r.repin();
    r.hf.chunk_ms = 100;
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    REQUIRE(eventually([&r] { return r.status(en::kind::still).state == en::download_state::downloading; }));
    r.installed = false;
#if !defined(_WIN32)
    fs::remove_all(r.s.root);  // and its data folder (Windows cannot delete the open .part)
#endif
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::stopped);
    CHECK(!fs::exists(r.weights("standard")));
#if !defined(_WIN32)
    CHECK(!fs::exists(r.s.root));
#endif
  }
}

TEST_CASE("prune never follows a link out of the weights folder", "[enhance]") {
  rig r;
  scratch elsewhere("elsewhere");  // someone else's folder
  put(elsewhere.root / "readme.txt", std::string("theirs"));
  put(elsewhere.root / "other-app" / "weights.bin", std::string("theirs"));
  fs::create_directories(elsewhere.root / "other-app" / "empty");
  const auto untouched = [&elsewhere] {
    CHECK(fs::exists(elsewhere.root / "readme.txt"));
    CHECK(fs::exists(elsewhere.root / "other-app" / "weights.bin"));
    CHECK(fs::is_directory(elsewhere.root / "other-app" / "empty"));
  };
  const fs::path models = r.s.root / "cache" / "models";
  std::error_code ec;
  SECTION("the weights folder is a link") {
    fs::create_directories(models.parent_path());
    fs::create_directory_symlink(elsewhere.root, models, ec);
    if (ec) return;  // no links here (Windows without the privilege)
    r.open();
    REQUIRE(r.store->download(en::kind::still, "small"));
    r.settle();
    REQUIRE(r.store->remove(en::kind::still));
    r.settle();
    untouched();
    CHECK(fs::is_symlink(fs::symlink_status(models)));
  }
  SECTION("a link inside it") {
    fs::create_directories(models / "old" / "deeper");  // empty: goes
    fs::create_directory_symlink(elsewhere.root, models / "linked", ec);
    if (ec) return;
    r.open();
    REQUIRE(r.store->download(en::kind::still, "small"));
    r.settle();
    REQUIRE(r.store->remove(en::kind::still));
    r.settle();
    untouched();
    CHECK(fs::is_symlink(fs::symlink_status(models / "linked")));
    CHECK(!fs::exists(models / "old"));
    CHECK(!fs::exists(models / "small"));
  }
}

// ---- the Models page --------------------------------------------------------------------

namespace {

const scr::node* node_of(const scr::tree& t, const std::string& id) {
  const scr::node* n = t.find(id);
  INFO(id);
  REQUIRE(n);
  return n;
}

std::string text_of(const scr::tree& t, const std::string& id) {
  const std::string* s = node_of(t, id)->props.str("text");
  return s ? *s : std::string();
}

void buttons_of(const scr::node& n, std::vector<const scr::node*>& out) {
  if (n.type == scr::node_type::button) out.push_back(&n);
  for (const scr::node& c : n.children) buttons_of(c, out);
}

scr::tree checked(const std::string& json) {
  scr::tree t;
  const scr::error e = t.set(json);
  INFO(scr::error_name(e));
  REQUIRE(e == scr::error::none);
  return t;
}

std::vector<en::kind_status> statuses(en::kind_status still = {}) {
  std::vector<en::kind_status> out(3);
  out[0] = std::move(still);
  out[0].k = en::kind::still;
  out[1].k = en::kind::video;
  out[2].k = en::kind::audio;
  return out;
}

}  // namespace

TEST_CASE("sizes read as ByteSize.cs writes them", "[enhance]") {
  CHECK(en::size_text(0) == "0 bytes");
  CHECK(en::size_text(850) == "850 bytes");
  CHECK(en::size_text(999) == "999 bytes");
  CHECK(en::size_text(1000) == "1.00 KB");
  CHECK(en::size_text(1125) == "1.13 KB");  // the format rounds half away from zero
  CHECK(en::size_text(9950) == "9.95 KB");
  CHECK(en::size_text(99'950) == "100 KB");
  CHECK(en::size_text(999'499) == "999 KB");
  CHECK(en::size_text(999'999) == "1.00 MB");  // rounded first, not "1000 KB"
  CHECK(en::size_text(8'420'000) == "8.42 MB");
  CHECK(en::size_text(42'100'000) == "42.1 MB");
  CHECK(en::size_text(412'000'000) == "412 MB");
  CHECK(en::size_text(1'080'000'000) == "1.08 GB");
  CHECK(en::size_text(4'200'000'000) == "4.20 GB");
  CHECK(en::size_text(1'000'000'000'000) == "1.00 TB");
  CHECK(en::size_text(1'000'000'000'000'000) == "1000 TB");

  CHECK(en::memory_text(16 * kGiB) == "16 GB");
  CHECK(en::memory_text(6 * kGiB) == "6 GB");
  CHECK(en::memory_text(2254857830) == "2.1 GB");
  CHECK(en::memory_text(256 * kMiB) == "256 MB");
  CHECK(en::memory_text(17'013'000'000) == "16 GB");  // what a 16 GB PC reports
}

TEST_CASE("this machine in two lines", "[enhance]") {
  CHECK(en::machine_line(mac(32 * kGiB)) == "This Mac: Apple M2 Pro, 32 GB of memory.");
  CHECK(en::machine_line(pc(32 * kGiB, "nvidia", 12 * kGiB, "NVIDIA GeForce RTX 4070")) ==
        "This PC: 32 GB of memory, NVIDIA GeForce RTX 4070 with 12 GB.");
  CHECK(en::machine_line(pc(16 * kGiB)) == "This PC: 16 GB of memory and no supported graphics card.");
  CHECK(en::machine_line(intel_mac(16 * kGiB)) == "This Mac: 16 GB of memory and an Intel processor.");

  // The rest of the local check: cores and their architecture, free memory, free disk.
  CHECK(en::machine_facts(mac(32 * kGiB)) ==
        "10 Arm cores \xC2\xB7 16 GB of memory free \xC2\xB7 537 GB free on this disk");
  en::machine m = pc(16 * kGiB);
  m.free_disk = 1'300'000'000;
  CHECK(en::machine_facts(m) == "8 x86-64 cores \xC2\xB7 8 GB of memory free \xC2\xB7 1.30 GB free on this disk");
  m.cores = 1;
  m.arch = "other";
  m.ram_total = 0;  // not known: not shown
  m.free_disk = 0;
  CHECK(en::machine_facts(m) == "1 core \xC2\xB7 0 bytes free on this disk");
  m.cores = 0;
  CHECK(en::machine_facts(m) == "0 bytes free on this disk");
}

TEST_CASE("the Models page is a valid view tree that says what fits", "[enhance]") {
  const en::catalog c = spec_catalog();

  SECTION("a CPU-only PC: the standard model is recommended, the large one needs a GPU") {
    const scr::tree t = checked(en::models_tree(c, pc(8 * kGiB), statuses()));
    CHECK(text_of(t, "machine") == "This PC: 8 GB of memory and no supported graphics card.");
    CHECK(text_of(t, "facts") == "8 x86-64 cores \xC2\xB7 4 GB of memory free \xC2\xB7 537 GB free on this disk");
    CHECK(!t.find("notice"));
    CHECK(text_of(t, "kind.still") == "Photos");
    CHECK(text_of(t, "title.still.standard") == "Standard \xC2\xB7 Real-ESRGAN x4plus");
    CHECK(text_of(t, "detail.still.standard") ==
          "Downloads 38.7 MB from Hugging Face \xC2\xB7 needs 2 GB of memory \xC2\xB7 runs on the processor "
          "\xC2\xB7 BSD-3-Clause");
    // Nowhere to run here: no "runs on the graphics card" the PC has not got.
    CHECK(text_of(t, "detail.still.large") ==
          "Downloads 1.27 GB from Hugging Face \xC2\xB7 needs 6 GB of graphics memory \xC2\xB7 Apache-2.0");
    CHECK(text_of(t, "state.still.standard") == "Recommended for this PC");
    CHECK(text_of(t, "state.still.small") == "Fits this PC");
    CHECK(text_of(t, "state.still.large") == "Needs a graphics card");
    const scr::node* dl = node_of(t, "a.still.standard");
    CHECK(*dl->props.str("label") == "Download Standard");
    CHECK(*dl->props.str("help") == "Download Standard (Photos), 38.7 MB");
    CHECK(*dl->props.str("style") == "primary");
    CHECK(!dl->props.boolean("disabled"));
    CHECK(*node_of(t, "a.still.small")->props.str("style") == "plain");
    CHECK(node_of(t, "a.still.large")->props.boolean("disabled") == std::optional<bool>(true));
    CHECK(text_of(t, "none.video") == "No model yet.");
    // One model under two kinds: its buttons say which.
    CHECK(*node_of(t, "a.still.small")->props.str("label") == "Download Small for Photos");
    CHECK(*node_of(t, "a.audio.small")->props.str("label") == "Download Small for Audio");
    CHECK(*node_of(t, "a.still.large")->props.str("label") == "Download Large");
    CHECK(text_of(t, "footnote").find("only when you click Download") != std::string::npos);
  }
  SECTION("a Mac short of memory, and a disk short of space") {
    en::catalog big = c;
    big.models[0].run_bytes = 24 * kGiB;
    big.models[1].files[0].size = 4'200'000'000;
    en::machine m = mac(16 * kGiB);
    m.free_disk = 1'300'000'000;
    const scr::tree t = checked(en::models_tree(big, m, statuses()));
    CHECK(text_of(t, "state.still.large") == "Needs 24 GB of memory; this Mac has 16 GB");
    CHECK(text_of(t, "state.still.standard") == "Needs 4.20 GB free on this disk; 1.30 GB is free");
    CHECK(text_of(t, "state.still.small") == "Recommended for this Mac");
    CHECK(text_of(t, "detail.still.small").find("runs on the processor") != std::string::npos);
  }
  SECTION("too large, and an Intel Mac") {
    en::catalog big = c;
    big.models[0].files[0].size = 5'000'000'001;
    const scr::tree t = checked(en::models_tree(big, pc(64 * kGiB, "nvidia", 24 * kGiB), statuses()));
    CHECK(text_of(t, "state.still.large") == "Too large to download (over 5 GB)");
    CHECK(node_of(t, "a.still.large")->props.boolean("disabled") == std::optional<bool>(true));
    CHECK(text_of(t, "state.still.standard") == "Recommended for this PC");
    const scr::tree i = checked(en::models_tree(c, intel_mac(16 * kGiB), statuses()));
    CHECK(text_of(i, "state.still.small") == "Needs a Mac with Apple silicon");
  }
  SECTION("each download state of the chosen model") {
    struct row {
      en::download_state state;
      std::string line;
      std::vector<std::string> labels;  // the card's buttons, in order
      bool lock_wait = false;
    };
    // The same two ids whatever the state, so focus stays on the button.
    const std::vector<row> rows = {
        {en::download_state::ready, "Downloaded", {"Delete Standard"}},
        {en::download_state::queued, "Waiting to download", {"Cancel Standard"}},
        {en::download_state::downloading, "Downloading 412 MB of 1.08 GB", {"Cancel Standard"}},
        {en::download_state::waiting, "Waiting for Hugging Face\xE2\x80\xA6 downloads resume automatically",
         {"Cancel Standard"}},
        {en::download_state::stopped, "The download stopped.", {"Resume Standard", "Delete Standard"}},
        {en::download_state::gone, "This file is no longer on Hugging Face; an update will replace it.",
         {"Resume Standard", "Delete Standard"}},
        {en::download_state::failed, "The download did not match its checksum.", {"Resume Standard", "Delete Standard"}},
        {en::download_state::refused, "Too large to download (over 5 GB)", {"Resume Standard", "Delete Standard"}},
        {en::download_state::unfit, "Fits this PC now; Resume downloads it.", {"Resume Standard", "Delete Standard"}},
        {en::download_state::no_space, "Needs 4.20 GB free on this disk; 1.30 GB is free",
         {"Resume Standard", "Delete Standard"}},
        {en::download_state::other_window,
         "Another MediaViewer window is downloading a model. Click Resume and this one carries on when it finishes.",
         {"Resume Standard", "Delete Standard"}},
        {en::download_state::other_window,
         "Another MediaViewer window is downloading a model. This one carries on when it finishes.",
         {"Cancel Standard", "Delete Standard"},
         true},
        {en::download_state::unsupported, "Downloads are not available on this computer.",
         {"Resume Standard", "Delete Standard"}},
    };
    for (const row& w : rows) {
      INFO(std::string(en::state_id(w.state)) + (w.lock_wait ? " (queued for the lock)" : ""));
      en::kind_status st;
      st.model = "standard";
      st.state = w.state;
      st.lock_wait = w.lock_wait;
      st.done_bytes = 412'000'000;
      st.total_bytes = 1'080'000'000;
      st.retry_in_s = 29.2;
      st.need = 4'200'000'000;
      st.have = 1'300'000'000;
      const auto shown = statuses(st);
      const scr::tree t = checked(en::models_tree(c, pc(16 * kGiB), shown));
      CHECK(text_of(t, "state.still.standard") == w.line);
      std::vector<const scr::node*> card;
      buttons_of(*node_of(t, "buttons.still.standard"), card);
      std::vector<std::string> labels;
      for (const scr::node* b : card) {
        labels.push_back(*b->props.str("label"));
        CHECK((b->id == "a.still.standard" || b->id == "d.still.standard"));
        // The id reads back as what its label says, against this status.
        const en::action a = en::action_of(b->id, shown);
        const char* said = a.what == en::verb::download ? "Download " : a.what == en::verb::cancel ? "Cancel "
                           : a.what == en::verb::resume ? "Resume "   : a.what == en::verb::remove ? "Delete "
                                                                                                     : "?";
        CHECK(labels.back().rfind(said, 0) == 0);
      }
      CHECK(labels == w.labels);
      const bool moving = w.state == en::download_state::downloading || w.state == en::download_state::waiting;
      CHECK((t.find("progress.still.standard") != nullptr) == moving);
      CHECK(t.find("a.still.small"));  // the others can still be chosen
    }
  }
  SECTION("a chosen model always has a way out: a button it can press, or the reason it cannot") {
    for (int i = 0; i <= static_cast<int>(en::download_state::unfit); ++i) {
      const auto state = static_cast<en::download_state>(i);
      if (state == en::download_state::none) continue;
      for (const std::string id : {"standard", "large"}) {  // fits this PC; needs a graphics card
        INFO(std::string(en::state_id(state)) + " " + id);
        en::kind_status st;
        st.model = id;
        st.state = state;
        st.total_bytes = 5000;
        const scr::tree t = checked(en::models_tree(c, pc(16 * kGiB), statuses(st)));
        CHECK(text_of(t, "state.still." + id) != " ");
        std::vector<const scr::node*> card;
        buttons_of(*node_of(t, "buttons.still." + id), card);
        bool can = false;
        for (const scr::node* b : card) {
          if (!b->props.boolean("disabled").value_or(false)) {
            can = true;
          } else {
            CHECK(!b->props.str("help")->empty());
          }
        }
        CHECK(can);
      }
    }
    // Refused by the rule: Resume says why until the model fits, then it can.
    en::kind_status st;
    st.model = "large";
    st.state = en::download_state::unfit;
    st.reason = en::why::needs_gpu;
    const scr::tree cpu = checked(en::models_tree(c, pc(16 * kGiB), statuses(st)));
    CHECK(text_of(cpu, "state.still.large") == "Needs a graphics card");
    CHECK(node_of(cpu, "a.still.large")->props.boolean("disabled") == std::optional<bool>(true));
    CHECK(*node_of(cpu, "a.still.large")->props.str("help") == "Needs a graphics card");
    CHECK(!node_of(cpu, "d.still.large")->props.boolean("disabled"));
    const scr::tree gpu = checked(en::models_tree(c, pc(32 * kGiB, "nvidia", 12 * kGiB), statuses(st)));
    CHECK(text_of(gpu, "state.still.large") == "Fits this PC now; Resume downloads it.");
    CHECK(!node_of(gpu, "a.still.large")->props.boolean("disabled"));
  }
  SECTION("a click that could not be saved says so") {
    const scr::tree t =
        checked(en::models_tree(c, pc(16 * kGiB), statuses(), {}, "Your choice could not be saved on this computer."));
    CHECK(text_of(t, "notice") == "Your choice could not be saved on this computer.");
  }
  SECTION("every button's id reads back as its action, and no two labels read alike") {
    en::kind_status st;
    st.model = "standard";
    st.state = en::download_state::stopped;
    auto shown = statuses(st);
    shown[2].model = "small";
    shown[2].state = en::download_state::ready;
    const scr::tree t = checked(en::models_tree(c, mac(16 * kGiB), shown));
    std::vector<const scr::node*> all;
    buttons_of(*t.root(), all);
    REQUIRE(all.size() == 5);
    std::set<std::string> labels;
    for (const scr::node* b : all) {
      INFO(b->id);
      const std::string label = *b->props.str("label");
      CHECK(labels.insert(label).second);
      const en::action a = en::action_of(b->id, shown);
      REQUIRE(a.what != en::verb::none);
      const std::string kind_dot_model =
          std::string(en::kind_id(a.k)) + "." + (a.what == en::verb::download ? a.model : shown[static_cast<std::size_t>(a.k)].model);
      CHECK(b->id == (a.what == en::verb::remove ? "d." : "a.") + kind_dot_model);
      const char* said = a.what == en::verb::download ? "Download " : a.what == en::verb::cancel ? "Cancel "
                         : a.what == en::verb::resume ? "Resume "   : "Delete ";
      CHECK(label.rfind(said, 0) == 0);
      if (a.what == en::verb::download) CHECK(c.find(a.model));
    }
    CHECK(labels.count("Resume Standard"));
    CHECK(labels.count("Delete Standard"));
    CHECK(labels.count("Download Small for Photos"));
    CHECK(labels.count("Delete Small for Audio"));

    // One id, read against the status its tree was drawn from: Download,
    // then Cancel, then Resume; a click the state changed under does nothing.
    const auto with = [](en::download_state s) {
      en::kind_status k;
      k.model = "standard";
      k.state = s;
      return statuses(k);
    };
    using en::download_state;
    using en::verb;
    CHECK(en::action_of("a.still.standard", statuses()).what == verb::download);
    CHECK(en::action_of("a.still.standard", statuses()).model == "standard");
    CHECK(en::action_of("a.still.standard", with(download_state::downloading)).what == verb::cancel);
    CHECK(en::action_of("a.still.standard", with(download_state::queued)).what == verb::cancel);
    CHECK(en::action_of("a.still.standard", with(download_state::stopped)).what == verb::resume);
    CHECK(en::action_of("a.still.standard", with(download_state::other_window)).what == verb::resume);
    CHECK(en::action_of("a.still.standard", with(download_state::ready)).what == verb::none);
    CHECK(en::action_of("d.still.standard", with(download_state::ready)).what == verb::remove);
    CHECK(en::action_of("d.still.standard", with(download_state::stopped)).what == verb::remove);
    CHECK(en::action_of("d.still.standard", with(download_state::downloading)).what == verb::none);
    CHECK(en::action_of("d.still.standard", statuses()).what == verb::none);
    CHECK(en::action_of("a.still.small", with(download_state::downloading)).what == verb::download);
    CHECK(en::action_of("a.still.small", with(download_state::downloading)).model == "small");
    CHECK(en::action_of("d.still.small", with(download_state::ready)).what == verb::none);
    CHECK(en::action_of("a.audio.small", with(download_state::ready)).what == verb::download);
    CHECK(en::action_of("a.audio.small", with(download_state::ready)).k == en::kind::audio);
    CHECK(en::action_of("a.still.standard", {}).what == verb::none);  // no tree drawn
    for (const std::string& id : std::vector<std::string>{"a.photo.standard", "a.still.", "a.still.Bad",
                                                          "a.still." + std::string(33, 'x'), "a..standard",
                                                          "x.still.standard", "dl.still.standard", "delete.still", "a",
                                                          "a.", "machine"}) {
      INFO(id);
      CHECK(en::action_of(id, with(download_state::stopped)).what == verb::none);
    }
  }
  SECTION("each button names its model, then its kind, then what it is, cut at 200 bytes") {
    en::catalog twin = c;
    twin.models[0].name = "Standard";  // large: two Standards in Photos
    const scr::tree t = checked(en::models_tree(twin, pc(16 * kGiB), statuses()));
    CHECK(*node_of(t, "a.still.large")->props.str("label") == "Download Standard (Swin2SR x4) for Photos");
    CHECK(*node_of(t, "a.still.standard")->props.str("label") == "Download Standard (Real-ESRGAN x4plus) for Photos");
    CHECK(*node_of(t, "a.still.small")->props.str("label") == "Download Small for Photos");
    twin.models[1].model_name = twin.models[0].model_name;  // and both are the same thing: their ids
    const scr::tree same = checked(en::models_tree(twin, pc(16 * kGiB), statuses()));
    CHECK(*node_of(same, "a.still.large")->props.str("label") == "Download Standard (large) for Photos");
    CHECK(*node_of(same, "a.still.standard")->props.str("label") == "Download Standard (standard) for Photos");

    std::string long_name;
    for (int i = 0; i < 150; ++i) long_name += "\xC3\xA9";  // "é", two bytes each
    twin.models[0].name = long_name;
    en::kind_status st;
    st.model = "large";
    st.state = en::download_state::stopped;
    const scr::tree cut = checked(en::models_tree(twin, pc(16 * kGiB), statuses(st)));
    for (const char* id : {"a.still.large", "d.still.large"}) {
      const std::string label = *node_of(cut, id)->props.str("label");
      CHECK(label.size() <= 200);
      CHECK(label.size() >= 198);
      CHECK(std::string(id[0] == 'a' ? "Resume " : "Delete ").append(long_name).rfind(label, 0) == 0);
    }
  }
  SECTION("ids stay within 64 characters for a model id of 32") {
    const std::string id(32, 'x');
    const std::string index =
        replace_once(kIndex, R"("models":["large","standard","small"])", R"("models":[")" + id + R"("])");
    const en::catalog one = parse({{id, replace_once(kStandard, R"("id":"standard")", R"("id":")" + id + "\"")}},
                                  {{"models/" + id + "/model.onnx", 1000, std::string(64, 'a')}}, index);
    REQUIRE(one.find(id));
    en::kind_status st;
    st.model = id;
    st.state = en::download_state::downloading;
    st.total_bytes = 1000;
    const auto shown = statuses(st);
    const scr::tree t = checked(en::models_tree(one, pc(16 * kGiB), shown));
    CHECK(t.find("a.still." + id));
    CHECK(t.find("progress.still." + id));
    CHECK(t.find("buttons.still." + id));
    CHECK(en::action_of("a.still." + id, shown).what == en::verb::cancel);
  }
}

TEST_CASE("a model that does not fit says what is missing", "[enhance]") {
  const en::catalog c = spec_catalog();
  SECTION("memory that rounds alike is written to the tenth, then the hundredth") {
    en::catalog big = c;
    big.models[1].run_bytes = 16 * kGiB;  // standard, on the processor here
    en::machine m = pc(16 * kGiB);
    m.ram_total = 17'013'000'000;  // what a 16 GB PC reports: "16 GB" too
    CHECK(text_of(checked(en::models_tree(big, m, statuses())), "state.still.standard") ==
          "Needs 16.0 GB of memory; this PC has 15.8 GB");
    big.models[1].run_bytes = 17'017'000'000;  // 15.85: the tenths agree too
    CHECK(text_of(checked(en::models_tree(big, m, statuses())), "state.still.standard") ==
          "Needs 15.85 GB of memory; this PC has 15.84 GB");
    big.models[0].run_bytes = 12 * kGiB;  // large, on the card
    const scr::tree card = checked(en::models_tree(big, pc(32 * kGiB, "nvidia", 12'700'000'000), statuses()));
    CHECK(text_of(card, "state.still.large") == "Needs 12.0 GB of graphics memory; this PC has 11.8 GB");
    big.models[0].run_bytes = 24 * kGiB;  // rounded apart: as before
    CHECK(text_of(checked(en::models_tree(big, mac(16 * kGiB), statuses())), "state.still.large") ==
          "Needs 24 GB of memory; this Mac has 16 GB");
  }
  SECTION("no graphics card it can use: worded by what is missing") {
    en::catalog only = c;
    en::model& large = only.models[0];  // no cpu_unit_ms: a card or nothing
    const auto line = [&only](const en::machine& here) {
      return text_of(checked(en::models_tree(only, here, statuses())), "state.still.large");
    };
    const auto detail = [&only](const en::machine& here) {
      return text_of(checked(en::models_tree(only, here, statuses())), "detail.still.large");
    };
    CHECK(line(pc(16 * kGiB)) == "Needs a graphics card");  // either card would do
    large.coreml = false;
    CHECK(line(pc(16 * kGiB)) == "Needs an NVIDIA graphics card");
    CHECK(line(mac(16 * kGiB)) == "Does not run on this Mac");
    CHECK(detail(mac(16 * kGiB)) ==
          "Downloads 1.27 GB from Hugging Face \xC2\xB7 needs 6 GB of memory \xC2\xB7 Apache-2.0");
    large.coreml = true;
    large.cuda = false;
    CHECK(line(pc(32 * kGiB, "nvidia", 12 * kGiB)) == "Needs a Mac with Apple silicon");
    CHECK(detail(pc(32 * kGiB, "nvidia", 12 * kGiB)).find("graphics") == std::string::npos);
    large.coreml = false;  // passed no card's self-test, too slow on the processor
    CHECK(line(pc(32 * kGiB, "nvidia", 12 * kGiB)) == "Too slow on this PC's processor");
    CHECK(line(pc(16 * kGiB)) == "Needs a graphics card");
    CHECK(line(mac(16 * kGiB)) == "Does not run on this Mac");
    // Where it does run, the detail says so.
    CHECK(detail(pc(32 * kGiB, "nvidia", 12 * kGiB)).find("graphics card") == std::string::npos);
    const scr::tree t = checked(en::models_tree(c, pc(32 * kGiB, "nvidia", 12 * kGiB), statuses()));
    CHECK(text_of(t, "detail.still.large").find("runs on the graphics card") != std::string::npos);
    CHECK(text_of(t, "detail.still.small").find("runs on the processor") != std::string::npos);
  }
  SECTION("a run the rule refused says what the run found, until it fits") {
    en::kind_status st;
    st.model = "standard";
    st.state = en::download_state::unfit;
    st.reason = en::why::memory;  // as the run found it
    st.need = 3 * kGiB;
    st.have = 2 * kGiB;
    const scr::tree small = checked(en::models_tree(c, pc(1 * kGiB), statuses(st)));
    CHECK(text_of(small, "state.still.standard") == "Needs 3 GB of memory; this PC has 2 GB");
    CHECK(*node_of(small, "a.still.standard")->props.str("help") == "Needs 3 GB of memory; this PC has 2 GB");
    CHECK(node_of(small, "a.still.standard")->props.boolean("disabled") == std::optional<bool>(true));
    st.reason = en::why::fits;  // nothing recorded: the page's own probe
    CHECK(text_of(checked(en::models_tree(c, pc(1 * kGiB), statuses(st))), "state.still.standard") ==
          "Needs 2 GB of memory; this PC has 1 GB");
    st.reason = en::why::memory;
    const scr::tree now = checked(en::models_tree(c, pc(16 * kGiB), statuses(st)));
    CHECK(text_of(now, "state.still.standard") == "Fits this PC now; Resume downloads it.");
    CHECK(!node_of(now, "a.still.standard")->props.boolean("disabled"));
    CHECK(*node_of(now, "a.still.standard")->props.str("label") == "Resume Standard");
  }
}

TEST_CASE("a download in progress updates by diff, and no tree holds a path", "[enhance]") {
  rig r;
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  REQUIRE(r.store->download(en::kind::audio, "small"));
  r.settle();
  const std::string tree = en::models_tree(r.cat, r.here, r.store->status());
  scr::tree t = checked(tree);
  CHECK(tree.find(r.s.dir()) == std::string::npos);
  CHECK(tree.find("cache") == std::string::npos);
  CHECK(text_of(t, "state.still.standard") == "Downloaded");

  en::kind_status a;
  a.model = "standard";
  a.state = en::download_state::downloading;
  a.done_bytes = 100'000'000;
  a.total_bytes = 1'080'000'000;
  en::kind_status b = a;
  b.done_bytes = 412'000'000;
  scr::tree live = checked(en::models_tree(r.cat, r.here, statuses(a)));
  const std::string diff = en::progress_diff(statuses(a), statuses(b));
  REQUIRE(!diff.empty());
  CHECK(live.apply(diff) == scr::error::none);
  CHECK(text_of(live, "state.still.standard") == "Downloading 412 MB of 1.08 GB");
  CHECK(en::progress_diff(statuses(b), statuses(b)) == "[]");
  en::kind_status c = b;
  c.state = en::download_state::ready;
  CHECK(en::progress_diff(statuses(b), statuses(c)).empty());  // a new shape: post the tree
  en::kind_status queued_here = b;
  queued_here.state = en::download_state::other_window;
  en::kind_status waits = queued_here;
  waits.lock_wait = true;  // Resume becomes Cancel
  CHECK(en::progress_diff(statuses(queued_here), statuses(waits)).empty());

  // Bytes no one would see move (the same line, the same whole percent): nothing.
  en::kind_status d = b;
  d.done_bytes = 412'100'000;
  CHECK(en::progress_diff(statuses(b), statuses(d)) == "[]");
  // The bar by a whole percent with the line unchanged: a diff.
  en::kind_status e = a;
  e.total_bytes = 100'000'000'000;
  e.done_bytes = 41'990'000'000;
  en::kind_status f = e;
  f.done_bytes = 42'010'000'000;
  scr::tree big = checked(en::models_tree(r.cat, r.here, statuses(e)));
  const std::string bar = en::progress_diff(statuses(e), statuses(f));
  REQUIRE(bar != "[]");
  CHECK(big.apply(bar) == scr::error::none);
  CHECK(text_of(big, "state.still.standard") == "Downloading 42.0 GB of 100 GB");
  // Waiting has no countdown: a new backoff alone changes nothing shown.
  en::kind_status w1 = a;
  w1.state = en::download_state::waiting;
  w1.retry_in_s = 2;
  en::kind_status w2 = w1;
  w2.retry_in_s = 4;
  CHECK(en::progress_diff(statuses(w1), statuses(w2)) == "[]");
}

TEST_CASE("the page counts what is already here for every model, chosen or not", "[enhance]") {
  rig r;
  put(r.weights("standard"), r.body["standard"]);  // dropped in, not chosen
  r.here.free_disk = 1000;                         // under its 5000 bytes
  r.open();
  const scr::tree t = checked(en::models_tree(r.cat, r.here, r.store->status(), [&r](const en::model& m) {
    return r.store->still_to_fetch(m);
  }));
  CHECK(text_of(t, "state.still.standard") == "Recommended for this PC");
  CHECK(!node_of(t, "a.still.standard")->props.boolean("disabled"));
  CHECK(text_of(t, "state.still.small") == "Needs 3.00 KB free on this disk; 1.00 KB is free");
  // Counted from the catalogue alone, the dropped file would still be to fetch.
  const scr::tree blind = checked(en::models_tree(r.cat, r.here, r.store->status()));
  CHECK(text_of(blind, "state.still.standard") == "Needs 5.00 KB free on this disk; 1.00 KB is free");
  CHECK(r.hf.made == 0);
}

TEST_CASE("the local check reads this machine", "[enhance]") {
  scratch s("probe");
  const en::machine m = en::probe(utf8(s.root / "not" / "yet"));
  CHECK(m.cores > 0);
  CHECK(m.ram_total > 0);
  CHECK(m.ram_free <= m.ram_total);
  CHECK(m.free_disk > 0);
  CHECK((m.os == "macos" || m.os == "windows" || m.os == "other"));
  CHECK((m.arch == "arm64" || m.arch == "x86_64" || m.arch == "other"));
  CHECK((m.accelerator.empty() || m.accelerator == "apple" || m.accelerator == "nvidia"));
}
