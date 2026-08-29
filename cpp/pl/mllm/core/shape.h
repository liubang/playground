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
#include <cstdint>
#include <span>

namespace pl::mllm {

// Value-type tensor shape, rank <= kMaxRank, contiguous row-major layout.
class Shape {
public:
    static constexpr int kMaxRank = 4;

    Shape() = default;
    // Ranks above kMaxRank are rejected (empty shape). Negative dims are rejected.
    explicit Shape(std::span<const int64_t> dims) {
        if (dims.size() > kMaxRank) {
            return;
        }
        for (int64_t d : dims) {
            if (d < 0) {
                rank_ = 0;
                dims_ = {};
                return;
            }
        }
        rank_ = static_cast<int>(dims.size());
        for (size_t i = 0; i < dims.size(); ++i) {
            dims_[i] = dims[i];
        }
    }
    Shape(std::initializer_list<int64_t> dims) : Shape(std::span<const int64_t>(dims)) {}

    [[nodiscard]] int rank() const noexcept { return rank_; }
    [[nodiscard]] int64_t dim(int i) const noexcept {
        return (i >= 0 && i < rank_) ? dims_[static_cast<size_t>(i)] : 0;
    }
    [[nodiscard]] std::span<const int64_t> dims() const noexcept {
        return {dims_.data(), static_cast<size_t>(rank_)};
    }

    // Element count; -1 on int64 overflow (dims are huge only in malformed inputs).
    [[nodiscard]] int64_t numel() const noexcept {
        int64_t n = 1;
        for (int i = 0; i < rank_; ++i) {
            const int64_t d = dims_[static_cast<size_t>(i)];
            if (d != 0 && n > INT64_MAX / d) {
                return -1;
            }
            n *= d;
        }
        return n;
    }

    // rank 0 marks a rejected construction, not a scalar tensor.
    [[nodiscard]] bool empty() const noexcept { return rank_ == 0 || numel() <= 0; }

    bool operator==(const Shape&) const = default;

private:
    std::array<int64_t, kMaxRank> dims_{};
    int rank_ = 0;
};

} // namespace pl::mllm
