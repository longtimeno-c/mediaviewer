// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ONNX Runtime behind our own interface (plan/17 "Runtime"): the library is
// loaded at run time from the AI pack (never linked, never in the base
// install), its C API is reached through OrtGetApiBase, and no ORT type
// crosses this header. The same discipline as IVideoSource (D9): headers
// here include no d3d11.h / Metal / DirectML; the platform TUs load the
// library.
//
// One process holds one ORT build: the Core pack's CPU build, or a vendor
// piece's build (CUDA) which also runs CPU. Changing the compute toggle
// re-creates sessions on the loaded build; a newly installed vendor piece
// takes effect the next time the add-on loads.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

namespace mv::infer {

enum class backend : std::uint8_t {
  cpu = 0,
  cuda = 1,      // Windows vendor piece (NVIDIA)
  openvino = 2,  // Windows vendor piece (Intel), when that ORT build is installed
  coreml = 3,    // macOS: GPU / Neural Engine, part of the Mac Core pack
};

[[nodiscard]] const char* backend_name(backend b) noexcept;

// What loading a provider found, for the Settings status line. Never a path.
enum class provider_fault : std::uint8_t {
  none = 0,
  not_in_build,      // this ORT build has no such provider
  runtime_missing,   // the provider needs a runtime the machine lacks (CUDA / cuDNN)
  failed,            // it loaded but a session would not run
  mismatch,          // its output differs from CPU beyond tolerance
  slower,            // it ran slower than CPU in the self-test
};

[[nodiscard]] const char* fault_name(provider_fault f) noexcept;

// An element tensor on the CPU, float32 in and out: fp16 models are
// converted at the edge, so callers never see half floats.
struct tensor_f32 {
  std::vector<std::int64_t> shape;
  std::vector<float> data;
};

struct tensor_i64 {
  std::vector<std::int64_t> shape;
  std::vector<std::int64_t> data;
};

class runtime {
 public:
  ~runtime();
  runtime(const runtime&) = delete;
  runtime& operator=(const runtime&) = delete;

  // Loads onnxruntime from `dir` (the folder holding onnxruntime.dll /
  // libonnxruntime.dylib), creates the environment with telemetry off
  // (rule 6: ORT's own ETW events never fire for us).
  [[nodiscard]] static result<std::unique_ptr<runtime>> load(const std::string& dir_utf8);

  [[nodiscard]] bool has_provider(backend b) const noexcept;
  [[nodiscard]] const std::string& version() const noexcept;

  struct impl;
  [[nodiscard]] impl& get() const noexcept { return *p_; }

 private:
  explicit runtime(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

struct session_options {
  backend on = backend::cpu;
  int threads = 1;                 // intra-op; the caller's worker is one of them
  std::string cache_dir_utf8;      // Core ML compiled-model cache (Mac); may be empty
};

// One model file on one provider. Run is safe from several threads at once.
class session {
 public:
  ~session();
  session(const session&) = delete;
  session& operator=(const session&) = delete;

  // `fault` (optional) explains a failure to open on a provider.
  [[nodiscard]] static result<std::unique_ptr<session>> open(const runtime& rt,
                                                             const std::string& model_utf8,
                                                             const session_options& options,
                                                             provider_fault* fault = nullptr);

  [[nodiscard]] backend on() const noexcept;
  [[nodiscard]] std::size_t input_count() const noexcept;
  [[nodiscard]] std::size_t output_count() const noexcept;

  // Runs with named or positional inputs; returns every output as float32.
  // An int64 input (token ids) goes through run_ids.
  [[nodiscard]] result<std::vector<tensor_f32>> run(std::span<const tensor_f32> inputs) const;
  [[nodiscard]] result<std::vector<tensor_f32>> run_ids(const tensor_i64& ids) const;

  // Inputs by name, of mixed element types (Whisper's decoder: int64 ids,
  // float states and caches, a bool flag). Float data converts to the
  // model's own input type (fp16 exports). Outputs come back as float32, in
  // the model's output order; output_names() names them.
  enum class element : std::uint8_t { f32, i64, flag };
  struct named_input {
    std::string name;
    std::vector<std::int64_t> shape;
    element type = element::f32;          // explicit: an empty cache has no data pointer
    const float* f32 = nullptr;           // the one `type` names
    const std::int64_t* i64 = nullptr;
    const bool* flag = nullptr;
    std::size_t count = 0;
  };
  [[nodiscard]] result<std::vector<tensor_f32>> run_named(std::span<const named_input> inputs) const;
  [[nodiscard]] const std::vector<std::string>& input_names() const noexcept;
  [[nodiscard]] const std::vector<std::string>& output_names() const noexcept;
  // ORT's message for this thread's last run_named failure (tensor shapes and
  // types; never a file name). For tests and a debugger, never logged.
  [[nodiscard]] static const std::string& last_error() noexcept;

  struct impl;

 private:
  explicit session(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

// ---- the platform half (ort_dylib_win.cpp / ort_dylib_posix.cpp) ----------

namespace dylib {
// Loads a shared library by absolute path so its own dependencies resolve
// from its folder; returns an opaque handle or nullptr.
[[nodiscard]] void* open(const std::string& path_utf8) noexcept;
[[nodiscard]] void* symbol(void* handle, const char* name) noexcept;
void close(void* handle) noexcept;
// ORT's file-name type: UTF-16 on Windows, UTF-8 elsewhere, NUL-terminated.
[[nodiscard]] std::vector<char> native_path(const std::string& path_utf8);
[[nodiscard]] const char* library_name() noexcept;  // "onnxruntime.dll" / "libonnxruntime.dylib"
}  // namespace dylib

// IEEE half <-> float, round-to-nearest-even.
[[nodiscard]] std::uint16_t to_half(float f) noexcept;
[[nodiscard]] float from_half(std::uint16_t h) noexcept;

}  // namespace mv::infer
