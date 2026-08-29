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

namespace pl::mllm {

// GGUF element types supported by the MVP.
enum class DType : uint8_t {
    kF32,
    kF16,
    kQ8_0,
    kQ4_0,
};

// Quantized block layout (ggml-compatible, little-endian).
inline constexpr int64_t kQ8_0BlockSize = 32;
inline constexpr size_t kQ8_0TypeSize = 2 + kQ8_0BlockSize; // fp16 scale + 32 x int8
inline constexpr int64_t kQ4_0BlockSize = 32;
inline constexpr size_t kQ4_0TypeSize = 2 + kQ4_0BlockSize / 2; // fp16 scale + 16 packed nibbles

[[nodiscard]] constexpr bool is_quantized(DType dtype) noexcept {
    return dtype == DType::kQ8_0 || dtype == DType::kQ4_0;
}

// Elements per quantization block (1 for plain types).
[[nodiscard]] constexpr int64_t dtype_block_size(DType dtype) noexcept {
    switch (dtype) {
        case DType::kQ8_0:
            return kQ8_0BlockSize;
        case DType::kQ4_0:
            return kQ4_0BlockSize;
        default:
            return 1;
    }
}

// Bytes per quantization block (element size for plain types).
[[nodiscard]] constexpr size_t dtype_type_size(DType dtype) noexcept {
    switch (dtype) {
        case DType::kF32:
            return 4;
        case DType::kF16:
            return 2;
        case DType::kQ8_0:
            return kQ8_0TypeSize;
        case DType::kQ4_0:
            return kQ4_0TypeSize;
    }
    return 0;
}

// Byte size of `numel` elements; 0 when `numel` is not block-aligned.
[[nodiscard]] constexpr size_t dtype_nbytes(DType dtype, int64_t numel) noexcept {
    const int64_t block = dtype_block_size(dtype);
    if (numel < 0 || numel % block != 0) {
        return 0;
    }
    return static_cast<size_t>(numel / block) * dtype_type_size(dtype);
}

// Portable IEEE-754 half <-> float conversion (bit-exact, no HW dependency).
[[nodiscard]] float fp16_to_fp32(uint16_t h) noexcept;
[[nodiscard]] uint16_t fp32_to_fp16(float f) noexcept;

} // namespace pl::mllm
