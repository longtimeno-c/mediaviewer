// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The installed AI pack on disk (docs/design/17 "The AI pack"), as the engine's
// dependencies: ONNX Runtime from the Core pack's folder (or a vendor piece's
// own ORT build, which also runs CPU), the CLIP towers under models/, and the
// face models from the ai-faces piece.
//
//   <core>/onnxruntime.dll | libonnxruntime.dylib    (+ providers_shared)
//   <core>/models/tiers.json                     which folder fills each role (tiers.h)
//   <core>/models/clip-tokenizer/{vocab.json, merges.txt}
//   <core>/models/clip-b32/{model.json, image.onnx, text.onnx}   Default's fast tower (quality 1)
//   <core>/models/clip-l14/{model.json, image.onnx, text.onnx}   Default's high tower (quality 2)
//   <ai-cuda>/{onnxruntime.dll, onnxruntime_providers_shared.dll, onnxruntime_providers_cuda.dll}
//   <ai-faces>/models/faces/{model.json, yunet.onnx, adaface.onnx}
//
// The folder names are the Default tier's; the code takes them from the
// tier profile, never from here.
//
// Loading is lazy and happens on the engine's control thread, never inside
// mv_addon_get (which the host may call from its UI thread).
#pragma once

#include <string>

#include "addons/ai/engine.h"
#include "addons/ai/host.h"

namespace mv::ai {

// `self_dir`: the Core pack's folder (platform::self_dir()); `data_dir`:
// the add-on's data folder (Core ML's compiled-model cache lives there).
[[nodiscard]] engine_deps pack_deps(const host& h, const std::string& self_dir,
                                    const std::string& data_dir);

}  // namespace mv::ai
