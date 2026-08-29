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

#include <array>
#include <cstring>
#include <span>

#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/shape.h"
#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm {

// Non-owning, typed view over a tensor storage.
// The caller is responsible for keeping the owner (GGUFFile / OwnedBuffer /
// ScratchArena / KVCache) alive for the whole lifetime of the view.
class TensorView {
public:
    TensorView() = default;
    TensorView(void* data, DType dtype, Shape shape) : data_(data), dtype_(dtype), shape_(shape) {
        byte_size_ = dtype_nbytes(dtype_, shape_.numel());
        set_contiguous_strides();
    }

    [[nodiscard]] void* data() noexcept { return data_; }
    [[nodiscard]] const void* data() const noexcept { return data_; }
    [[nodiscard]] size_t byte_size() const noexcept { return byte_size_; }
    [[nodiscard]] DType dtype() const noexcept { return dtype_; }
    [[nodiscard]] const Shape& shape() const noexcept { return shape_; }
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr && byte_size_ > 0; }
    // MVP kernels only accept contiguous tensors.
    [[nodiscard]] bool is_contiguous() const noexcept { return contiguous_; }

    [[nodiscard]] std::span<const int64_t> strides() const noexcept {
        return {strides_.data(), static_cast<size_t>(shape_.rank())};
    }

    // Low-level typed access for backend internals; caller must guarantee the
    // element layout matches T. Bounds-checked against byte_size_.
    template <typename T> [[nodiscard]] T* data_as() noexcept { return static_cast<T*>(data_); }
    template <typename T> [[nodiscard]] const T* data_as() const noexcept {
        return static_cast<const T*>(data_);
    }
    template <typename T> [[nodiscard]] std::span<T> span_as() noexcept {
        return {data_as<T>(), byte_size_ / sizeof(T)};
    }
    template <typename T> [[nodiscard]] std::span<const T> span_as() const noexcept {
        return {data_as<T>(), byte_size_ / sizeof(T)};
    }

    // Same storage, new contiguous shape; numel must match.
    [[nodiscard]] Result<TensorView> reshape(Shape shape) const noexcept {
        if (shape.numel() != shape_.numel() || shape.numel() < 0) {
            return Status::Error(ErrorCode::kInvalidArgument, "reshape: numel mismatch");
        }
        TensorView out = *this;
        out.shape_ = shape;
        out.set_contiguous_strides();
        return out;
    }

    // Contiguous slice along `dim`; empty if out of range.
    [[nodiscard]] Result<TensorView> slice(int dim, int64_t begin, int64_t end) const noexcept {
        if (dim < 0 || dim >= shape_.rank() || begin < 0 || end < begin || end > shape_.dim(dim)) {
            return Status::Error(ErrorCode::kInvalidArgument, "slice: bad range");
        }
        if (!contiguous_) {
            return Status::Error(ErrorCode::kUnsupported, "slice: non-contiguous view");
        }
        std::array<int64_t, Shape::kMaxRank> dims{};
        int rank = shape_.rank();
        for (int i = 0; i < rank; ++i) {
            dims[static_cast<size_t>(i)] = shape_.dim(i);
        }
        dims[static_cast<size_t>(dim)] = end - begin;
        Shape new_shape(std::span<const int64_t>(dims.data(), static_cast<size_t>(rank)));

        int64_t inner = 1; // elements per step along `dim`
        for (int i = dim + 1; i < rank; ++i) {
            inner *= shape_.dim(i);
        }
        const size_t elem = dtype_type_size(dtype_);
        const size_t offset = static_cast<size_t>(begin) * static_cast<size_t>(inner) * elem;
        if (is_quantized(dtype_) && begin != 0) {
            return Status::Error(ErrorCode::kUnsupported, "slice: quantized offset");
        }
        TensorView out(static_cast<char*>(data_) + offset, dtype_, new_shape);
        return out;
    }

private:
    void set_contiguous_strides() noexcept {
        contiguous_ = true;
        int64_t stride = 1;
        for (int i = shape_.rank() - 1; i >= 0; --i) {
            strides_[static_cast<size_t>(i)] = stride;
            stride *= shape_.dim(i);
        }
    }

    void* data_ = nullptr;
    size_t byte_size_ = 0;
    DType dtype_ = DType::kF32;
    Shape shape_{};
    std::array<int64_t, Shape::kMaxRank> strides_{};
    bool contiguous_ = true;
};

} // namespace pl::mllm
