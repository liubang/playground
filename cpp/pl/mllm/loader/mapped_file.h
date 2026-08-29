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
#include <span>
#include <string>

#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm {

// RAII owner of a read-only mmap of a whole file. Move-only.
class MappedFile {
public:
    [[nodiscard]] static Result<MappedFile> Open(const std::string& path);

    MappedFile() = default;
    ~MappedFile() { Close(); }

    MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    void Close() noexcept;

    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return {data_, size_}; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

private:
    int fd_ = -1;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::string path_;
};

} // namespace pl::mllm
