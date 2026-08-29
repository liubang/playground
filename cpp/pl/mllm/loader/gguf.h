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
// Created: 2026/08/29 22:15

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/shape.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/loader/mapped_file.h"
#include "cpp/pl/mllm/model/config.h"

namespace pl::mllm {

// One tensor entry of the GGUF directory.
// `shape` is row-major: GGUF stores dims in ggml order (ne0 contiguous),
// they are reversed here so dim(rank-1) is the contiguous axis.
struct TensorInfo {
    std::string name;
    DType dtype = DType::kF32;
    Shape shape;
    size_t file_offset = 0; // absolute offset into the mapped file
    size_t byte_size = 0;
};

using MetadataValue = std::variant<uint8_t,
                                   int8_t,
                                   uint16_t,
                                   int16_t,
                                   uint32_t,
                                   int32_t,
                                   uint64_t,
                                   int64_t,
                                   float,
                                   double,
                                   bool,
                                   std::string,
                                   std::vector<std::string>,
                                   std::vector<float>,
                                   std::vector<double>,
                                   std::vector<int32_t>,
                                   std::vector<uint32_t>,
                                   std::vector<int64_t>,
                                   std::vector<uint64_t>,
                                   std::vector<uint8_t>>;

// Parsed GGUF v3 file. Owns the mmap; every TensorView handed out points into
// it, so the GGUFFile must outlive all views (Model holds a shared_ptr).
class GGUFFile {
public:
    [[nodiscard]] static Result<std::shared_ptr<GGUFFile>> Open(std::string path);

    GGUFFile(const GGUFFile&) = delete;
    GGUFFile& operator=(const GGUFFile&) = delete;

    [[nodiscard]] std::string_view architecture() const noexcept { return architecture_; }
    [[nodiscard]] Result<ModelConfig> model_config() const;

    [[nodiscard]] std::span<const TensorInfo> tensors() const noexcept { return tensors_; }
    [[nodiscard]] bool has_tensor(std::string_view name) const noexcept;
    [[nodiscard]] Result<TensorInfo> tensor_info(std::string_view name) const;
    // View into the mmap; owner is this GGUFFile.
    [[nodiscard]] Result<TensorView> tensor(std::string_view name) const;

    // Typed metadata accessors; kNotFound / kInvalidFormat on mismatch.
    [[nodiscard]] const MetadataValue* metadata(std::string_view key) const noexcept;
    [[nodiscard]] Result<std::string> string_meta(std::string_view key) const;
    [[nodiscard]] Result<uint32_t> u32_meta(std::string_view key) const;
    [[nodiscard]] Result<int32_t> i32_meta(std::string_view key) const;
    [[nodiscard]] Result<float> f32_meta(std::string_view key) const;
    [[nodiscard]] Result<bool> bool_meta(std::string_view key) const;
    [[nodiscard]] Result<std::span<const std::string>> str_array_meta(std::string_view key) const;
    [[nodiscard]] Result<std::span<const float>> f32_array_meta(std::string_view key) const;
    [[nodiscard]] Result<std::span<const int32_t>> i32_array_meta(std::string_view key) const;

private:
    GGUFFile() = default;

    MappedFile mapped_;
    std::string architecture_;
    std::unordered_map<std::string, MetadataValue> metadata_;
    std::vector<TensorInfo> tensors_;
    std::unordered_map<std::string_view, size_t> name_to_index_; // into tensors_
};

} // namespace pl::mllm
