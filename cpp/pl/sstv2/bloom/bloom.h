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
// Created: 2026/06/05 22:09

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/key.h"

namespace pl::sstv2::bloom {

struct Header {
    static constexpr size_t kSize = 36;
    static constexpr uint32_t kMagic = 0x4D4F4C42; // BLOM
    static constexpr uint32_t kVersion = 1;

    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t hash_count = 0;
    uint64_t bit_count = 0;
    uint64_t row_count = 0;
    uint64_t checksum = 0;
};

class Builder {
public:
    explicit Builder(int bits_per_key = 10);

    [[nodiscard]] absl::Status add(const types::InternalRow& row,
                                   const types::InternalSchema::ConstRef& schema);
    [[nodiscard]] absl::Status add_all_key(const types::EncodedAllKey& all_key);
    [[nodiscard]] std::string finish() const;

private:
    int bits_per_key_;
    std::vector<uint32_t> hashes_;
};

class Reader {
public:
    [[nodiscard]] static absl::StatusOr<Reader> open(std::string_view section);

    [[nodiscard]] bool may_contain_all_key(const types::EncodedAllKey& all_key) const;
    [[nodiscard]] absl::StatusOr<bool> may_contain(const types::InternalRow& row,
                                                   const types::InternalSchema::ConstRef& schema) const;

    [[nodiscard]] const Header& header() const noexcept { return header_; }

private:
    Header header_;
    std::string bits_;
};

} // namespace pl::sstv2::bloom
