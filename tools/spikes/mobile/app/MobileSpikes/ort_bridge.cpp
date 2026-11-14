// M1 spike S1 (throwaway). See ort_bridge.h. Session settings follow src/infer/ort.cpp.
#include "ort_bridge.h"

#include <onnxruntime/onnxruntime_c_api.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct mv_ort {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtSession* session = nullptr;
  OrtMemoryInfo* memory = nullptr;
  std::vector<std::string> inputs;
  std::string inputs_joined;
  std::string output;
};

namespace {

const OrtApi* api() { return OrtGetApiBase()->GetApi(ORT_API_VERSION); }

bool failed(const OrtApi* a, OrtStatus* status, char* error, size_t cap) {
  if (!status) return false;
  if (error && cap) std::snprintf(error, cap, "%s", a->GetErrorMessage(status));
  a->ReleaseStatus(status);
  return true;
}

}  // namespace

const char* mv_ort_version(void) { return OrtGetApiBase()->GetVersionString(); }

void mv_ort_close(mv_ort* o) {
  if (!o) return;
  if (o->memory) o->api->ReleaseMemoryInfo(o->memory);
  if (o->session) o->api->ReleaseSession(o->session);
  if (o->env) o->api->ReleaseEnv(o->env);
  delete o;
}

mv_ort* mv_ort_open(const char* path, int threads, int optimization, char* error, size_t cap) {
  auto* o = new mv_ort();
  o->api = api();
  const OrtApi* a = o->api;
  OrtSessionOptions* so = nullptr;
  OrtAllocator* allocator = nullptr;
  const auto bail = [&]() -> mv_ort* {
    if (so) a->ReleaseSessionOptions(so);
    mv_ort_close(o);
    return nullptr;
  };

  if (failed(a, a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "m1", &o->env), error, cap)) return bail();
  if (failed(a, a->CreateSessionOptions(&so), error, cap)) return bail();
  if (failed(a, a->SetIntraOpNumThreads(so, std::max(1, threads)), error, cap)) return bail();
  if (failed(a, a->SetInterOpNumThreads(so, 1), error, cap)) return bail();
  if (failed(a, a->SetSessionGraphOptimizationLevel(so, static_cast<GraphOptimizationLevel>(optimization)), error, cap)) return bail();
  if (failed(a, a->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "0"), error, cap)) return bail();
  if (failed(a, a->AddSessionConfigEntry(so, "session.inter_op.allow_spinning", "0"), error, cap)) return bail();
  if (failed(a, a->CreateSession(o->env, path, so, &o->session), error, cap)) return bail();
  a->ReleaseSessionOptions(so);
  so = nullptr;

  if (failed(a, a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &o->memory), error, cap)) return bail();
  if (failed(a, a->GetAllocatorWithDefaultOptions(&allocator), error, cap)) return bail();

  size_t count = 0;
  if (failed(a, a->SessionGetInputCount(o->session, &count), error, cap)) return bail();
  for (size_t i = 0; i < count; ++i) {
    char* name = nullptr;
    if (failed(a, a->SessionGetInputName(o->session, i, allocator, &name), error, cap)) return bail();
    o->inputs.emplace_back(name);
    if (i) o->inputs_joined += ",";
    o->inputs_joined += name;
    (void)a->AllocatorFree(allocator, name);
  }

  if (failed(a, a->SessionGetOutputCount(o->session, &count), error, cap)) return bail();
  for (size_t i = 0; i < count; ++i) {
    char* name = nullptr;
    if (failed(a, a->SessionGetOutputName(o->session, i, allocator, &name), error, cap)) return bail();
    const std::string found(name);
    (void)a->AllocatorFree(allocator, name);
    if (i == 0 || found == "text_embeds") o->output = found;
  }
  return o;
}

const char* mv_ort_inputs(const mv_ort* o) { return o->inputs_joined.c_str(); }
const char* mv_ort_output(const mv_ort* o) { return o->output.c_str(); }

long mv_ort_encode(mv_ort* o, const int64_t* ids, size_t context, size_t used, float* out, size_t cap,
                   char* error, size_t error_cap) {
  const OrtApi* a = o->api;
  std::vector<int64_t> mask(context, 0);
  std::fill_n(mask.begin(), std::min(used, context), 1);
  const std::vector<int64_t> zeros(context, 0);
  const int64_t shape[2] = {1, static_cast<int64_t>(context)};

  std::vector<const char*> names;
  std::vector<OrtValue*> values;
  const auto release = [&] {
    for (OrtValue* v : values) a->ReleaseValue(v);
  };
  for (const std::string& name : o->inputs) {
    const int64_t* data = name == "attention_mask" ? mask.data() : name == "token_type_ids" ? zeros.data() : ids;
    OrtValue* v = nullptr;
    OrtStatus* s = a->CreateTensorWithDataAsOrtValue(o->memory, const_cast<int64_t*>(data), context * sizeof(int64_t),
                                                     shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &v);
    if (failed(a, s, error, error_cap)) {
      release();
      return -1;
    }
    names.push_back(name.c_str());
    values.push_back(v);
  }

  const char* output_name = o->output.c_str();
  OrtValue* output = nullptr;
  OrtStatus* s = a->Run(o->session, nullptr, names.data(), values.data(), values.size(), &output_name, 1, &output);
  release();
  if (failed(a, s, error, error_cap)) return -1;

  float* data = nullptr;
  OrtTensorTypeAndShapeInfo* info = nullptr;
  size_t elements = 0;
  long result = -1;
  if (!failed(a, a->GetTensorMutableData(output, reinterpret_cast<void**>(&data)), error, error_cap) &&
      !failed(a, a->GetTensorTypeAndShape(output, &info), error, error_cap) &&
      !failed(a, a->GetTensorShapeElementCount(info, &elements), error, error_cap)) {
    const size_t n = std::min(elements, cap);
    std::memcpy(out, data, n * sizeof(float));
    result = static_cast<long>(n);
  }
  if (info) a->ReleaseTensorTypeAndShapeInfo(info);
  a->ReleaseValue(output);
  return result;
}
