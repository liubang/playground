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

#include "cpp/pl/mllm/model/architecture.h"
#include "cpp/pl/mllm/model/dense_decoder.h"
#include "cpp/pl/mllm/model/model.h"

namespace pl::mllm {

Result<std::unique_ptr<Model>> CreateModel(ModelConfig config,
                                           std::span<const Model::WeightEntry> weights) {
    const ArchSpec* spec = find_architecture(config.architecture);
    if (spec == nullptr) {
        return Status::Error(ErrorCode::kUnsupported,
                             "unsupported architecture: " + config.architecture);
    }

    // Dense decoder families (llama / qwen2 / qwen3) share one implementation;
    // families that need a different compute graph (MoE, MLA, ...) register
    // with dense_decoder == false and get their own branch here.
    if (spec->dense_decoder) {
        auto model = DenseDecoderModel::Create(std::move(config), weights);
        if (!model.ok()) {
            return model.status();
        }
        return Result<std::unique_ptr<Model>>(std::move(model).value());
    }

    return Status::Error(ErrorCode::kUnsupported,
                         "no model implementation for architecture: " + config.architecture);
}

} // namespace pl::mllm
