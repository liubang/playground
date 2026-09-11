// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/09/11

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/vision/config.h"

namespace pl::mllm {

class GGUFFile;

namespace media {
struct Image;
}

namespace vision {

// Named weight view resolved from an mmproj weight file. The caller keeps
// the backing storage (mmap) alive for the tower lifetime. Mirrors
// Model::WeightEntry on the text side.
struct WeightEntry {
    std::string name;
    TensorView view;
};

// Result of encoding one image: the visual token sequence in the LM's
// embedding space plus the patch-grid geometry (needed by the caller to
// build multimodal rope positions).
struct VisionOutput {
    OwnedBuffer storage;   // owns the f32 embedding matrix
    TensorView embeddings; // [n_tokens, output_dim] view into `storage`
    int32_t n_tokens = 0;  // tokens after spatial merge
    int32_t grid_h = 0;    // patch grid (before merge)
    int32_t grid_w = 0;
};

// A vision tower maps an image to LM-space token embeddings. It is the
// visual counterpart of the text Model: weights resolved by name, compute
// dispatched through the same Backend, and the Engine treats its output as
// just another embedding source (see engine input sequences).
//
// Concrete families: PaddleOcrTower (SigLIP-derived NaViT + MLP projector).
class VisionTower {
public:
    virtual ~VisionTower() = default;

    // Encode `image` into LM-space embeddings. The backend must already have
    // the tower's weights imported (Engine imports text + vision weights
    // together; vision tensors carry the `v.` prefix so namespaces never
    // collide).
    [[nodiscard]] virtual Result<VisionOutput> Encode(const media::Image& image,
                                                      Backend& backend) const = 0;

    // Number of visual tokens an image of the given size would produce
    // (after spatial merge). Used to size the input sequence before Encode.
    [[nodiscard]] virtual Result<int32_t> TokenCount(int32_t width, int32_t height) const = 0;

    [[nodiscard]] virtual const VisionConfig& config() const noexcept = 0;

    // Names of all weight tensors the backend must import.
    [[nodiscard]] virtual std::vector<std::string> weight_names() const = 0;
};

// Build the vision tower described by an mmproj GGUF file
// (`clip.projector_type` metadata selects the family). Returns kUnsupported
// for unknown projector types.
[[nodiscard]] Result<std::unique_ptr<VisionTower>> CreateVisionTower(
    const GGUFFile& mmproj, std::span<const WeightEntry> weights);

} // namespace vision
} // namespace pl::mllm
