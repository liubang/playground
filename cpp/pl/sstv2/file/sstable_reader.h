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
// Created: 2026/06/04 22:27

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/file/locator.h"
#include "cpp/pl/sstv2/file/tail.h"
#include "cpp/pl/sstv2/types/variant.h"

namespace pl::sstv2::file {

struct StoredRow {
    std::string all_key;
    uint64_t version = 0;
    uint8_t op_type = 0;
    types::DataType value_type = types::DataType::kNone;
    bool embedded = false;
    std::string filename;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t checksum = 0;
    std::string embedded_value;
};

class SSTableReader {
public:
    static absl::StatusOr<SSTableReader> open(std::string_view path);

    [[nodiscard]] bool is_valid() const;
    [[nodiscard]] const Tail& tail() const;
    [[nodiscard]] const Locator& locator() const;
    [[nodiscard]] std::span<const std::byte> file_data() const;

    [[nodiscard]] std::vector<StoredRow> scan() const;
    absl::StatusOr<std::string> read_value_bytes(const StoredRow& row) const;
    absl::StatusOr<bool> may_contain_encoded_key(std::string_view all_key) const;

private:
    SSTableReader() = default;

    std::string key_file_path_;
    std::string data_;
    Tail tail_;
    Locator locator_;
    bool valid_ = false;
};

} // namespace pl::sstv2::file
