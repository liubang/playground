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
// Created: 2026/06/04 15:23

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace pl::sstv2::file {

struct ValueHandle {
    uint64_t file_id = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
    uint32_t checksum = 0; // CRC32C

    static constexpr size_t kSerializedSize = 24; // 8+8+4+4
    void encode(std::byte* dst) const;
    static ValueHandle decode(const std::byte* src);
};

class ValueFileWriter {
public:
    explicit ValueFileWriter(uint64_t file_id, std::string_view path);

    absl::StatusOr<ValueHandle> append(std::span<const std::byte> value);
    absl::Status finish();
    uint64_t current_size() const;

private:
    uint64_t file_id_;
    std::string path_;
    std::string buffer_; // in-memory buffer, flushed on finish
    uint64_t offset_ = 0;
};

} // namespace pl::sstv2::file
