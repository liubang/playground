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
// Created: 2026/08/30

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/model/config.h"

namespace pl::mllm {

// Abstract decoder model. Implementations own (references to) the resolved
// weight views and expose the two operations the Engine needs:
//   * Forward: push one embedded token through all layers (KV cache append).
//   * ComputeLogits: final norm + output projection.
// Concrete families: DenseDecoderModel (llama / qwen2 / qwen3, ...); future
// non-dense families (MoE, MLA, ...) get their own implementation and a
// branch in the model factory.
class Model {
public:
    virtual ~Model() = default;

    // Named weight view resolved from the weight file (GGUF). The caller must
    // keep the backing storage (mmap) alive for the model lifetime.
    struct WeightEntry {
        std::string name;
        TensorView view;
    };

    // Forward one token through all layers.
    // hidden: [1, hidden_size] — input embedding; modified in-place by
    // residual adds. position: absolute sequence position. cache: KV cache
    // (appends all layers, then advances). scratch: arena (reset per token).
    virtual Status Forward(TensorView hidden,
                           int64_t position,
                           KVCache& cache,
                           Backend& backend,
                           ScratchArena& scratch) const = 0;

    // Final norm + output projection (lm_head).
    // hidden: [1, hidden_size]; logits: [1, vocab_size] — output.
    virtual Status ComputeLogits(TensorView hidden,
                                 TensorView logits,
                                 Backend& backend,
                                 ScratchArena& scratch) const = 0;

    [[nodiscard]] virtual const ModelConfig& config() const noexcept = 0;
    [[nodiscard]] virtual int32_t num_layers() const noexcept = 0;

    // Names of all weight tensors the backend must import.
    [[nodiscard]] virtual std::vector<std::string> weight_names() const = 0;
};

// Build the concrete Model for `config.architecture`. Returns kUnsupported
// for architectures that are not registered (see architecture.h).
// The caller keeps ownership of the weight backing storage.
[[nodiscard]] Result<std::unique_ptr<Model>> CreateModel(
    ModelConfig config, std::span<const Model::WeightEntry> weights);

} // namespace pl::mllm
