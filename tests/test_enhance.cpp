// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance, E1 (#297, docs/plans/enhance.md): the catalogue and its rules, the
// spec rule on several machine classes, the download store against a fake
// Hugging Face that records every request (nothing before the click or after
// a restart, the gates before any GET, resume, wrong hashes, adoption, prune,
// the lock, no transport), and the Models page through the screen vocabulary.
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
  en::catalog cat;
  en::machine here = pc(16 * kGiB);  // CPU only: large needs a GPU
  bool no_transport = false;
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
    std::vector<pin> pins;
    for (const auto& [id, b] : body) {
      const std::string path = "models/" + id + "/model.onnx";
      pins.push_back({path, b.size(), hex_of(b)});
      hf.files[url_of(path)] = b;
    }
    cat = parse(three(), pins);
  }
  ~rig() { store.reset(); }

  void open() {
    en::store_options o;
    o.background = false;  // a test runner shares its CPUs
    o.fetch.first_backoff_s = 1;
    o.fetch.wait = [this](double secs, const std::atomic<bool>& stop) { return wait(secs, stop); };
    store = std::make_unique<en::model_store>(
        s.dir(), cat,
        [this]() -> std::unique_ptr<mv::ai::http_transport> {
          std::lock_guard lock(hf.m);
          ++hf.made;
          if (no_transport) return nullptr;
          return std::make_unique<proxy>(hf);
        },
        [this] { return here; }, [this] { ++changes; }, o);
  }

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
    fs::create_directories(r.s.root / "cache");
    const std::string lock = utf8(r.s.root / "cache" / "download.lock");
    auto held = en::try_lock(lock);
    REQUIRE(held);
    CHECK(!en::try_lock(lock));  // exclusive, even within one process
    r.open();
    REQUIRE(r.store->download(en::kind::still, "standard"));
    r.settle();
    CHECK(r.status(en::kind::still).state == en::download_state::other_window);
    CHECK(r.hf.seen().empty());
    held.reset();
    CHECK(en::try_lock(lock));
    r.store->resume(en::kind::still);
    r.settle();
    CHECK(r.store->ready(en::kind::still));
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

TEST_CASE("a 404 on the pinned file is gone", "[enhance]") {
  rig r;
  r.hf.script = [](std::size_t, std::uint64_t) { return std::pair<int, long long>{404, -1}; };
  r.open();
  REQUIRE(r.store->download(en::kind::still, "standard"));
  r.settle();
  CHECK(r.status(en::kind::still).state == en::download_state::gone);
  CHECK(r.hf.seen().size() == 1);
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

TEST_CASE("this machine in one line", "[enhance]") {
  CHECK(en::machine_line(mac(32 * kGiB)) == "This Mac: Apple M2 Pro, 32 GB of memory.");
  CHECK(en::machine_line(pc(32 * kGiB, "nvidia", 12 * kGiB, "NVIDIA GeForce RTX 4070")) ==
        "This PC: 32 GB of memory, NVIDIA GeForce RTX 4070 with 12 GB.");
  CHECK(en::machine_line(pc(16 * kGiB)) == "This PC: 16 GB of memory and no supported graphics card.");
  CHECK(en::machine_line(intel_mac(16 * kGiB)) == "This Mac: 16 GB of memory and an Intel processor.");
}

TEST_CASE("the Models page is a valid view tree that says what fits", "[enhance]") {
  const en::catalog c = spec_catalog();

  SECTION("a CPU-only PC: the standard model is recommended, the large one needs a GPU") {
    const scr::tree t = checked(en::models_tree(c, pc(8 * kGiB), statuses()));
    CHECK(text_of(t, "machine") == "This PC: 8 GB of memory and no supported graphics card.");
    CHECK(text_of(t, "kind.still") == "Photos");
    CHECK(text_of(t, "title.still.standard") == "Standard \xC2\xB7 Real-ESRGAN x4plus");
    CHECK(text_of(t, "detail.still.standard") ==
          "Downloads 38.7 MB from Hugging Face \xC2\xB7 needs 2 GB of memory \xC2\xB7 runs on the processor "
          "\xC2\xB7 BSD-3-Clause");
    CHECK(text_of(t, "detail.still.large") ==
          "Downloads 1.27 GB from Hugging Face \xC2\xB7 needs 6 GB of graphics memory \xC2\xB7 runs on the "
          "graphics card \xC2\xB7 Apache-2.0");
    CHECK(text_of(t, "state.still.standard") == "Recommended for this PC");
    CHECK(text_of(t, "state.still.small") == "Fits this PC");
    CHECK(text_of(t, "state.still.large") == "Needs a graphics card");
    const scr::node* dl = node_of(t, "dl.still.standard");
    CHECK(*dl->props.str("style") == "primary");
    CHECK(!dl->props.boolean("disabled"));
    CHECK(*node_of(t, "dl.still.small")->props.str("style") == "plain");
    CHECK(node_of(t, "dl.still.large")->props.boolean("disabled") == std::optional<bool>(true));
    CHECK(text_of(t, "none.video") == "No model yet.");
    CHECK(t.find("dl.audio.small"));
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
    CHECK(node_of(t, "dl.still.large")->props.boolean("disabled") == std::optional<bool>(true));
    CHECK(text_of(t, "state.still.standard") == "Recommended for this PC");
    const scr::tree i = checked(en::models_tree(c, intel_mac(16 * kGiB), statuses()));
    CHECK(text_of(i, "state.still.small") == "Needs a Mac with Apple silicon");
  }
  SECTION("each download state of the chosen model") {
    struct row {
      en::download_state state;
      std::string line;
      std::vector<std::string> buttons;
    };
    const std::vector<row> rows = {
        {en::download_state::ready, "Downloaded", {"delete.still"}},
        {en::download_state::queued, "Waiting to download", {"cancel.still"}},
        {en::download_state::downloading, "Downloading 412 MB of 1.08 GB", {"cancel.still"}},
        {en::download_state::waiting, "Waiting for Hugging Face\xE2\x80\xA6 trying again in 30 s", {"cancel.still"}},
        {en::download_state::stopped, "The download stopped.", {"resume.still", "delete.still"}},
        {en::download_state::gone, "This file is no longer on Hugging Face; an update will replace it.",
         {"resume.still", "delete.still"}},
        {en::download_state::failed, "The download did not match its checksum.", {"resume.still", "delete.still"}},
        {en::download_state::refused, "Too large to download (over 5 GB)", {"delete.still"}},
        {en::download_state::no_space, "Needs 4.20 GB free on this disk; 1.30 GB is free",
         {"resume.still", "delete.still"}},
        {en::download_state::other_window, "Downloading in another MediaViewer window.",
         {"resume.still", "delete.still"}},
        {en::download_state::unsupported, "Downloads are not available on this computer.",
         {"resume.still", "delete.still"}},
    };
    for (const row& w : rows) {
      INFO(en::state_id(w.state));
      en::kind_status st;
      st.model = "standard";
      st.state = w.state;
      st.done_bytes = 412'000'000;
      st.total_bytes = 1'080'000'000;
      st.retry_in_s = 29.2;
      st.need = 4'200'000'000;
      st.have = 1'300'000'000;
      const scr::tree t = checked(en::models_tree(c, pc(16 * kGiB), statuses(st)));
      CHECK(text_of(t, "state.still.standard") == w.line);
      std::vector<const scr::node*> card;
      buttons_of(*node_of(t, "buttons.still.standard"), card);
      std::vector<std::string> ids;
      for (const scr::node* b : card) ids.push_back(b->id);
      CHECK(ids == w.buttons);
      const bool moving = w.state == en::download_state::downloading || w.state == en::download_state::waiting;
      CHECK((t.find("progress.still.standard") != nullptr) == moving);
      CHECK(t.find("dl.still.small"));  // the others can still be chosen
    }
  }
  SECTION("every button's id reads back as its action") {
    en::kind_status st;
    st.model = "standard";
    st.state = en::download_state::stopped;
    const scr::tree t = checked(en::models_tree(c, mac(16 * kGiB), statuses(st)));
    std::vector<const scr::node*> all;
    buttons_of(*t.root(), all);
    REQUIRE(all.size() >= 5);
    for (const scr::node* b : all) {
      INFO(b->id);
      CHECK(b->props.str("label"));
      const en::action a = en::action_of(b->id);
      REQUIRE(a.what != en::verb::none);
      if (a.what == en::verb::download) {
        CHECK(b->id == std::string("dl.") + en::kind_id(a.k) + "." + a.model);
        CHECK(c.find(a.model));
      } else {
        const char* head = a.what == en::verb::cancel ? "cancel." : a.what == en::verb::resume ? "resume." : "delete.";
        CHECK(b->id == head + std::string(en::kind_id(a.k)));
      }
    }
    CHECK(en::action_of("delete.audio").what == en::verb::remove);
    CHECK(en::action_of("dl.still.standard").model == "standard");
    CHECK(en::action_of("dl.photo.standard").what == en::verb::none);
    CHECK(en::action_of("dl.still.").what == en::verb::none);
    CHECK(en::action_of("dl.still.Bad").what == en::verb::none);
    CHECK(en::action_of("cancel").what == en::verb::none);
    CHECK(en::action_of("machine").what == en::verb::none);
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
