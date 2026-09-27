// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ONNX Runtime through its C API, loaded at run time (ort.h).
#include "infer/ort.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include <onnxruntime_c_api.h>

namespace mv::infer {

const char* backend_name(backend b) noexcept {
  switch (b) {
    case backend::cpu: return "cpu";
    case backend::cuda: return "cuda";
    case backend::openvino: return "openvino";
    case backend::coreml: return "coreml";
  }
  return "cpu";
}

const char* fault_name(provider_fault f) noexcept {
  switch (f) {
    case provider_fault::none: return "none";
    case provider_fault::not_in_build: return "not_in_build";
    case provider_fault::runtime_missing: return "runtime_missing";
    case provider_fault::failed: return "failed";
    case provider_fault::mismatch: return "mismatch";
    case provider_fault::slower: return "slower";
  }
  return "failed";
}

std::uint16_t to_half(float f) noexcept {
  std::uint32_t x;
  std::memcpy(&x, &f, 4);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  std::uint32_t mant = x & 0x007FFFFFu;
  const int exp = static_cast<int>((x >> 23) & 0xFF);
  if (exp == 0xFF) return static_cast<std::uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
  int e = exp - 127 + 15;
  if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return static_cast<std::uint16_t>(sign);
    mant |= 0x00800000u;
    const int shift = 14 - e;
    std::uint32_t half = mant >> shift;
    const std::uint32_t rem = mant & ((1u << shift) - 1u);
    const std::uint32_t mid = 1u << (shift - 1);
    if (rem > mid || (rem == mid && (half & 1u))) ++half;
    return static_cast<std::uint16_t>(sign | half);
  }
  std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (mant >> 13);
  const std::uint32_t rem = mant & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;  // may carry into the exponent: correct
  return static_cast<std::uint16_t>(sign | half);
}

float from_half(std::uint16_t h) noexcept {
  const std::uint32_t sign = (static_cast<std::uint32_t>(h) & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1Fu;
  std::uint32_t mant = h & 0x3FFu;
  std::uint32_t x;
  if (exp == 0) {
    if (mant == 0) {
      x = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3FFu;
      x = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    x = sign | 0x7F800000u | (mant << 13);
  } else {
    x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}

// ---- runtime ------------------------------------------------------------------

struct runtime::impl {
  void* lib = nullptr;
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  std::vector<std::string> providers;
  std::string version;
  ~impl() {
    if (env && api) api->ReleaseEnv(env);
    // The library stays mapped for the process: ORT keeps thread-local state
    // and provider libraries that do not unload cleanly. One load per process.
  }
};

runtime::runtime(std::unique_ptr<impl> p) : p_(std::move(p)) {}
runtime::~runtime() = default;

namespace {

bool ok(const OrtApi* api, OrtStatus* st, std::string* message = nullptr) {
  if (!st) return true;
  if (message) {
    const char* m = api->GetErrorMessage(st);
    *message = m ? m : "";
  }
  api->ReleaseStatus(st);
  return false;
}

// A provider's load error names DLLs, never the user's files; classify it.
provider_fault classify(const std::string& message) {
  std::string m = message;
  std::transform(m.begin(), m.end(), m.begin(), [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  for (std::string_view needle : {"cudnn", "cublas", "cudart", "cufft", "curand", "loadlibrary",
                                  "error loading", "dlopen", "126", "cuda driver", "no cuda"}) {
    if (m.find(needle) != std::string::npos) return provider_fault::runtime_missing;
  }
  return provider_fault::failed;
}

const char* provider_id(backend b) {
  switch (b) {
    case backend::cuda: return "CUDAExecutionProvider";
    case backend::openvino: return "OpenVINOExecutionProvider";
    case backend::coreml: return "CoreMLExecutionProvider";
    default: return "CPUExecutionProvider";
  }
}

}  // namespace

result<std::unique_ptr<runtime>> runtime::load(const std::string& dir_utf8) {
  auto p = std::make_unique<impl>();
  std::string path = dir_utf8;
  if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
  path += dylib::library_name();
  p->lib = dylib::open(path);
  if (!p->lib) return err(status::io);
  using get_base_fn = const OrtApiBase*(ORT_API_CALL*)();
  auto get = reinterpret_cast<get_base_fn>(dylib::symbol(p->lib, "OrtGetApiBase"));
  if (!get) return err(status::corrupt);
  const OrtApiBase* base = get();
  if (!base) return err(status::corrupt);
  p->api = base->GetApi(ORT_API_VERSION);
  if (!p->api) return err(status::unsupported_format);  // an older ORT than the header
  p->version = base->GetVersionString ? base->GetVersionString() : "";
  if (!ok(p->api, p->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "mv-ai", &p->env))) {
    return err(status::internal);
  }
  (void)ok(p->api, p->api->DisableTelemetryEvents(p->env));
  char** names = nullptr;
  int n = 0;
  if (ok(p->api, p->api->GetAvailableProviders(&names, &n)) && names) {
    for (int i = 0; i < n; ++i) p->providers.emplace_back(names[i]);
    (void)ok(p->api, p->api->ReleaseAvailableProviders(names, n));
  }
  return std::unique_ptr<runtime>(new runtime(std::move(p)));
}

bool runtime::has_provider(backend b) const noexcept {
  const std::string_view want = provider_id(b);
  return std::any_of(p_->providers.begin(), p_->providers.end(),
                     [&](const std::string& s) { return s == want; });
}

const std::string& runtime::version() const noexcept { return p_->version; }

// ---- session ------------------------------------------------------------------

struct session::impl {
  const OrtApi* api = nullptr;
  OrtSession* s = nullptr;
  OrtMemoryInfo* mem = nullptr;
  backend on = backend::cpu;
  std::vector<std::string> in_names, out_names;
  std::vector<ONNXTensorElementDataType> in_types;
  ~impl() {
    if (s) api->ReleaseSession(s);
    if (mem) api->ReleaseMemoryInfo(mem);
  }
};

session::session(std::unique_ptr<impl> p) : p_(std::move(p)) {}
session::~session() = default;
backend session::on() const noexcept { return p_->on; }
std::size_t session::input_count() const noexcept { return p_->in_names.size(); }
std::size_t session::output_count() const noexcept { return p_->out_names.size(); }

result<std::unique_ptr<session>> session::open(const runtime& rt, const std::string& model_utf8,
                                               const session_options& options,
                                               provider_fault* fault) {
  if (fault) *fault = provider_fault::none;
  const OrtApi* api = rt.get().api;
  auto p = std::make_unique<impl>();
  p->api = api;
  p->on = options.on;
  if (options.on != backend::cpu && !rt.has_provider(options.on)) {
    if (fault) *fault = provider_fault::not_in_build;
    return err(status::unsupported_format);
  }
  OrtSessionOptions* so = nullptr;
  if (!ok(api, api->CreateSessionOptions(&so))) return err(status::internal);
  struct release_so {
    const OrtApi* api;
    OrtSessionOptions* so;
    ~release_so() { api->ReleaseSessionOptions(so); }
  } const guard_so{api, so};
  (void)ok(api, api->SetIntraOpNumThreads(so, std::max(1, options.threads)));
  (void)ok(api, api->SetInterOpNumThreads(so, 1));
  (void)ok(api, api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
  // Idle pool threads sleep instead of spinning: the viewer owns the CPU.
  (void)ok(api, api->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "0"));
  (void)ok(api, api->AddSessionConfigEntry(so, "session.inter_op.allow_spinning", "0"));

  std::string message;
  bool appended = true;
  switch (options.on) {
    case backend::cpu:
      break;
    case backend::cuda: {
      OrtCUDAProviderOptionsV2* cuda = nullptr;
      if (!ok(api, api->CreateCUDAProviderOptions(&cuda), &message)) {
        appended = false;
        break;
      }
      const char* keys[] = {"device_id", "arena_extend_strategy", "cudnn_conv_algo_search"};
      const char* values[] = {"0", "kSameAsRequested", "HEURISTIC"};
      (void)ok(api, api->UpdateCUDAProviderOptions(cuda, keys, values, 3));
      appended = ok(api, api->SessionOptionsAppendExecutionProvider_CUDA_V2(so, cuda), &message);
      api->ReleaseCUDAProviderOptions(cuda);
      break;
    }
    case backend::openvino: {
      const char* keys[] = {"device_type"};
      const char* values[] = {"AUTO"};
      appended = ok(api, api->SessionOptionsAppendExecutionProvider(so, "OpenVINO", keys, values, 1),
                    &message);
      break;
    }
    case backend::coreml: {
      std::vector<const char*> keys{"ModelFormat", "MLComputeUnits"};
      std::vector<const char*> values{"MLProgram", "ALL"};
      if (!options.cache_dir_utf8.empty()) {
        keys.push_back("ModelCacheDirectory");
        values.push_back(options.cache_dir_utf8.c_str());
      }
      appended = ok(api, api->SessionOptionsAppendExecutionProvider(
                             so, "CoreML", keys.data(), values.data(), keys.size()),
                    &message);
      break;
    }
  }
  if (!appended) {
    if (fault) *fault = classify(message);
    return err(status::unsupported_format);
  }
  const std::vector<char> path = dylib::native_path(model_utf8);
  if (!ok(api, api->CreateSession(rt.get().env, reinterpret_cast<const ORTCHAR_T*>(path.data()), so, &p->s),
          &message)) {
    if (fault) *fault = options.on == backend::cpu ? provider_fault::failed : classify(message);
    return err(options.on == backend::cpu ? status::corrupt : status::unsupported_format);
  }
  if (!ok(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &p->mem))) {
    return err(status::internal);
  }
  OrtAllocator* alloc = nullptr;
  if (!ok(api, api->GetAllocatorWithDefaultOptions(&alloc))) return err(status::internal);
  std::size_t nin = 0, nout = 0;
  (void)ok(api, api->SessionGetInputCount(p->s, &nin));
  (void)ok(api, api->SessionGetOutputCount(p->s, &nout));
  for (std::size_t i = 0; i < nin; ++i) {
    char* name = nullptr;
    if (!ok(api, api->SessionGetInputName(p->s, i, alloc, &name))) return err(status::corrupt);
    p->in_names.emplace_back(name);
    (void)ok(api, api->AllocatorFree(alloc, name));
    OrtTypeInfo* ti = nullptr;
    ONNXTensorElementDataType t = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    if (ok(api, api->SessionGetInputTypeInfo(p->s, i, &ti)) && ti) {
      const OrtTensorTypeAndShapeInfo* tsi = nullptr;
      if (ok(api, api->CastTypeInfoToTensorInfo(ti, &tsi)) && tsi) {
        (void)ok(api, api->GetTensorElementType(tsi, &t));
      }
      api->ReleaseTypeInfo(ti);
    }
    p->in_types.push_back(t);
  }
  for (std::size_t i = 0; i < nout; ++i) {
    char* name = nullptr;
    if (!ok(api, api->SessionGetOutputName(p->s, i, alloc, &name))) return err(status::corrupt);
    p->out_names.emplace_back(name);
    (void)ok(api, api->AllocatorFree(alloc, name));
  }
  if (p->in_names.empty() || p->out_names.empty()) return err(status::corrupt);
  return std::unique_ptr<session>(new session(std::move(p)));
}

namespace {

struct values {
  const OrtApi* api;
  std::vector<OrtValue*> v;
  explicit values(const OrtApi* a) : api(a) {}
  ~values() {
    for (OrtValue* x : v) {
      if (x) api->ReleaseValue(x);
    }
  }
};

result<std::vector<tensor_f32>> collect(const OrtApi* api, values& outs) {
  std::vector<tensor_f32> result_tensors;
  for (OrtValue* v : outs.v) {
    tensor_f32 t;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (!ok(api, api->GetTensorTypeAndShape(v, &info))) return err(status::internal);
    std::size_t dims = 0;
    (void)ok(api, api->GetDimensionsCount(info, &dims));
    t.shape.resize(dims);
    (void)ok(api, api->GetDimensions(info, t.shape.data(), dims));
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    (void)ok(api, api->GetTensorElementType(info, &type));
    std::size_t count = 0;
    (void)ok(api, api->GetTensorShapeElementCount(info, &count));
    api->ReleaseTensorTypeAndShapeInfo(info);
    void* data = nullptr;
    if (!ok(api, api->GetTensorMutableData(v, &data)) || (!data && count)) return err(status::internal);
    t.data.resize(count);
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      std::memcpy(t.data.data(), data, count * sizeof(float));
    } else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
      const auto* h = static_cast<const std::uint16_t*>(data);
      for (std::size_t i = 0; i < count; ++i) t.data[i] = from_half(h[i]);
    } else {
      return err(status::unsupported_format);
    }
    result_tensors.push_back(std::move(t));
  }
  return result_tensors;
}

}  // namespace

result<std::vector<tensor_f32>> session::run(std::span<const tensor_f32> inputs) const {
  const OrtApi* api = p_->api;
  if (inputs.size() != p_->in_names.size()) return err(status::invalid_arg);
  values ins(api);
  std::vector<std::vector<std::uint16_t>> halves(inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const tensor_f32& t = inputs[i];
    OrtValue* v = nullptr;
    OrtStatus* st = nullptr;
    if (p_->in_types[i] == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
      halves[i].resize(t.data.size());
      for (std::size_t k = 0; k < t.data.size(); ++k) halves[i][k] = to_half(t.data[k]);
      st = api->CreateTensorWithDataAsOrtValue(p_->mem, halves[i].data(), halves[i].size() * 2,
                                               t.shape.data(), t.shape.size(),
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &v);
    } else if (p_->in_types[i] == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      st = api->CreateTensorWithDataAsOrtValue(p_->mem, const_cast<float*>(t.data.data()),
                                               t.data.size() * sizeof(float), t.shape.data(),
                                               t.shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v);
    } else {
      return err(status::unsupported_format);
    }
    if (!ok(api, st)) return err(status::invalid_arg);
    ins.v.push_back(v);
  }
  std::vector<const char*> in_names, out_names;
  for (const auto& n : p_->in_names) in_names.push_back(n.c_str());
  for (const auto& n : p_->out_names) out_names.push_back(n.c_str());
  values outs(api);
  outs.v.assign(out_names.size(), nullptr);
  if (!ok(api, api->Run(p_->s, nullptr, in_names.data(), ins.v.data(), ins.v.size(), out_names.data(),
                        out_names.size(), outs.v.data()))) {
    return err(status::internal);
  }
  return collect(api, outs);
}

const std::vector<std::string>& session::input_names() const noexcept { return p_->in_names; }
const std::vector<std::string>& session::output_names() const noexcept { return p_->out_names; }

namespace {
thread_local std::string t_last_error;
}  // namespace

const std::string& session::last_error() noexcept { return t_last_error; }

using named_input_element = session::element;

result<std::vector<tensor_f32>> session::run_named(std::span<const named_input> inputs) const {
  const OrtApi* api = p_->api;
  t_last_error.clear();
  values ins(api);
  std::vector<const char*> in_names;
  std::vector<std::vector<std::uint16_t>> halves;
  halves.reserve(inputs.size());
  for (const named_input& in : inputs) {
    auto it = std::find(p_->in_names.begin(), p_->in_names.end(), in.name);
    if (it == p_->in_names.end()) {
      t_last_error = "no input " + in.name;
      return err(status::invalid_arg);
    }
    const auto type = p_->in_types[static_cast<std::size_t>(it - p_->in_names.begin())];
    OrtValue* v = nullptr;
    OrtStatus* st = nullptr;
    // A zero-length dimension (an empty cache) still needs a valid pointer.
    static float empty_f = 0;
    static std::int64_t empty_i = 0;
    static bool empty_b = false;
    if (in.type == named_input_element::f32) {
      if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
        halves.emplace_back(in.count);
        for (std::size_t k = 0; k < in.count; ++k) halves.back()[k] = to_half(in.f32[k]);
        st = api->CreateTensorWithDataAsOrtValue(p_->mem, in.count ? halves.back().data() : static_cast<void*>(&empty_f),
                                                 in.count * 2, in.shape.data(), in.shape.size(),
                                                 ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &v);
      } else {
        st = api->CreateTensorWithDataAsOrtValue(p_->mem, in.count ? const_cast<float*>(in.f32) : &empty_f,
                                                 in.count * sizeof(float), in.shape.data(), in.shape.size(),
                                                 ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v);
      }
    } else if (in.type == named_input_element::i64) {
      st = api->CreateTensorWithDataAsOrtValue(p_->mem, in.count ? const_cast<std::int64_t*>(in.i64) : &empty_i,
                                               in.count * 8, in.shape.data(), in.shape.size(),
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &v);
    } else if (in.type == named_input_element::flag) {
      st = api->CreateTensorWithDataAsOrtValue(p_->mem, in.count ? const_cast<bool*>(in.flag) : &empty_b,
                                               in.count * sizeof(bool), in.shape.data(), in.shape.size(),
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL, &v);
    } else {
      return err(status::invalid_arg);
    }
    if (!ok(api, st, &t_last_error)) return err(status::invalid_arg);
    ins.v.push_back(v);
    in_names.push_back(it->c_str());
  }
  std::vector<const char*> out_names;
  for (const auto& n : p_->out_names) out_names.push_back(n.c_str());
  values outs(api);
  outs.v.assign(out_names.size(), nullptr);
  if (!ok(api, api->Run(p_->s, nullptr, in_names.data(), ins.v.data(), ins.v.size(), out_names.data(),
                        out_names.size(), outs.v.data()), &t_last_error)) {
    return err(status::internal);
  }
  return collect(api, outs);
}

result<std::vector<tensor_f32>> session::run_ids(const tensor_i64& ids) const {
  const OrtApi* api = p_->api;
  if (p_->in_names.empty()) return err(status::invalid_arg);
  values ins(api);
  std::vector<std::int32_t> narrow;
  OrtValue* v = nullptr;
  OrtStatus* st = nullptr;
  if (p_->in_types[0] == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
    narrow.reserve(ids.data.size());
    for (std::int64_t id : ids.data) narrow.push_back(static_cast<std::int32_t>(id));
    st = api->CreateTensorWithDataAsOrtValue(p_->mem, narrow.data(), narrow.size() * 4, ids.shape.data(),
                                             ids.shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &v);
  } else {
    st = api->CreateTensorWithDataAsOrtValue(p_->mem, const_cast<std::int64_t*>(ids.data.data()),
                                             ids.data.size() * 8, ids.shape.data(), ids.shape.size(),
                                             ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &v);
  }
  if (!ok(api, st)) return err(status::invalid_arg);
  ins.v.push_back(v);
  // A text tower exported with an attention mask takes ones for every id.
  std::vector<std::int64_t> mask;
  if (p_->in_names.size() == 2) {
    mask.assign(ids.data.size(), 1);
    OrtValue* m = nullptr;
    if (!ok(api, api->CreateTensorWithDataAsOrtValue(p_->mem, mask.data(), mask.size() * 8, ids.shape.data(),
                                                     ids.shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                     &m))) {
      return err(status::invalid_arg);
    }
    ins.v.push_back(m);
  } else if (p_->in_names.size() != 1) {
    return err(status::unsupported_format);
  }
  std::vector<const char*> in_names, out_names;
  for (const auto& n : p_->in_names) in_names.push_back(n.c_str());
  for (const auto& n : p_->out_names) out_names.push_back(n.c_str());
  values outs(api);
  outs.v.assign(out_names.size(), nullptr);
  if (!ok(api, api->Run(p_->s, nullptr, in_names.data(), ins.v.data(), ins.v.size(), out_names.data(),
                        out_names.size(), outs.v.data()))) {
    return err(status::internal);
  }
  return collect(api, outs);
}

}  // namespace mv::infer
