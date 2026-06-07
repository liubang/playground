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
// Created: 2026/06/06 14:15

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/format/section.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::format {

struct Configuration {
    uint64_t max_embedded_value_size = 0;
    uint64_t max_data_block_size_soft_limit = 64 * 1024;
    uint64_t max_data_block_size_hard_limit = 128 * 1024;
    uint64_t max_data_block_row_count = 4096;
    uint64_t max_index_block_size_soft_limit = 64 * 1024;
    uint64_t max_index_block_size_hard_limit = 128 * 1024;
    uint64_t max_index_block_row_count = 4096;
};

struct Statistics {
    uint64_t total_row_count = 0;
    uint64_t data_block_count = 0;
    uint64_t index_block_count = 0;
    uint64_t key_file_size = 0;
    uint64_t value_file_size = 0;
};

[[nodiscard]] SectionMap configuration_entries(const Configuration& configuration);
[[nodiscard]] absl::StatusOr<Configuration> configuration_from_entries(const SectionMap& entries);

[[nodiscard]] SectionMap schema_entries(const types::Schema& schema);
[[nodiscard]] absl::StatusOr<types::Schema::ConstRef> schema_from_entries(
    const SectionMap& entries);

[[nodiscard]] SectionMap statistics_entries(const Statistics& statistics);
[[nodiscard]] absl::StatusOr<Statistics> statistics_from_entries(const SectionMap& entries);

[[nodiscard]] std::string encode_configuration(const Configuration& configuration);
[[nodiscard]] std::string encode_schema(const types::Schema& schema);
[[nodiscard]] std::string encode_statistics(const Statistics& statistics);

[[nodiscard]] absl::StatusOr<Configuration> decode_configuration(std::string_view input);
[[nodiscard]] absl::StatusOr<types::Schema::ConstRef> decode_schema(std::string_view input);
[[nodiscard]] absl::StatusOr<Statistics> decode_statistics(std::string_view input);

} // namespace pl::sstv2::format
