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
// Created: 2026/07/17 22:27

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minitable/core/key.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::minitable::codec {

// Hard limits are part of the persisted table key format. They must be loaded from
// table metadata and must not be changed by runtime admission-control settings.
struct KeyFormat {
    static constexpr uint32_t kCurrentVersion = 1;
    static constexpr uint32_t kReservedColumnFamilyId = 0;
    static constexpr size_t kVirtualBucketWidth = 4;

    uint32_t version = kCurrentVersion;
    PartitionMode partition_mode = PartitionMode::kGlobalOrder;
    HashAlgorithm hash_algorithm = HashAlgorithm::kNone;
    uint32_t virtual_bucket_count = 0;
    size_t max_encoded_key_bytes = 64 * 1024;
    size_t max_dynamic_qualifier_bytes = 16 * 1024;
    size_t max_row_key_columns = 32;
};

class CellKeyCodec final {
public:
    [[nodiscard]] static absl::StatusOr<CellKeyCodec> Create(
        KeyFormat format, sstv2::types::Schema::ConstRef row_key_schema);

    [[nodiscard]] absl::StatusOr<std::string> EncodeLogicalRowKey(
        const std::vector<sstv2::types::Value>& row_key) const;

    [[nodiscard]] absl::StatusOr<std::string> EncodeStorageKey(const StorageKey& key) const;

    [[nodiscard]] absl::StatusOr<std::string> EncodeVersionedStorageKey(
        const VersionedStorageKey& key) const;

    // Strict decoders for persisted keys. They reject malformed, non-canonical,
    // truncated, and trailing input with DATA_LOSS.
    [[nodiscard]] absl::StatusOr<StorageKey> DecodeStorageKey(std::string_view encoded) const;
    [[nodiscard]] absl::StatusOr<VersionedStorageKey> DecodeVersionedStorageKey(
        std::string_view encoded) const;

private:
    CellKeyCodec(KeyFormat format, sstv2::types::Schema::ConstRef row_key_schema)
        : format_(format), row_key_schema_(std::move(row_key_schema)) {}

    [[nodiscard]] absl::Status ValidateRowKey(
        const std::vector<sstv2::types::Value>& row_key) const;
    [[nodiscard]] absl::Status AppendRecordTarget(const RecordTarget& target,
                                                  std::string* encoded) const;
    [[nodiscard]] absl::Status CheckEncodedSize(size_t size) const;
    [[nodiscard]] absl::StatusOr<StorageKey> DecodeStorageKeyPrefix(std::string_view encoded,
                                                                    size_t* consumed) const;

    KeyFormat format_;
    sstv2::types::Schema::ConstRef row_key_schema_;
};

} // namespace pl::minitable::codec
