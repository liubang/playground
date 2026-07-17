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
// Created: 2026/07/18

#pragma once

#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/file/sstable.h"
#include "cpp/pl/sstv2/io/filesystem.h"
#include "cpp/pl/sstv2/merge/merge_iterator.h"

namespace pl::minitable {

struct ComparatorDomain {
    uint64_t key_format_version = 1;
    uint64_t row_key_schema_fingerprint = 0x4D54424C42494E31ULL; // MTBLBIN1
    uint64_t partition_mode = 1; // GLOBAL_ORDER
    uint64_t hash_algorithm_version = 0;
    uint64_t virtual_bucket_count = 0;
    uint64_t fingerprint = 0x9A0C9ED4382FD4A1ULL;

    bool operator==(const ComparatorDomain&) const = default;
};

inline constexpr uint64_t kMinitableSstFormatVersion = 1;
inline constexpr uint64_t kCrc32cChecksumAlgorithm = 1;
inline constexpr ComparatorDomain kMinitableComparatorDomain;

struct SstIdentity {
    std::string key_path;
    std::string value_path;
    sstv2::io::FileIdentity key_file;
    sstv2::io::FileIdentity value_file;
    uint64_t row_count = 0;
    uint64_t sst_format_version = kMinitableSstFormatVersion;
    ComparatorDomain comparator_domain = kMinitableComparatorDomain;
    uint64_t checksum_algorithm = kCrc32cChecksumAlgorithm;

    bool operator==(const SstIdentity&) const = default;
};

// Immutable, identity-verified SST object pinned by SliceVersion/read views.
class SstReadSource final {
public:
    [[nodiscard]] static absl::StatusOr<std::shared_ptr<const SstReadSource>> Open(
        std::shared_ptr<sstv2::io::FileSystem> filesystem, SstIdentity identity);

    [[nodiscard]] const SstIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] absl::StatusOr<std::unique_ptr<sstv2::merge::ForwardCursor>> new_cursor() const;

private:
    SstReadSource(SstIdentity identity, sstv2::file::Reader reader)
        : identity_(std::move(identity)), reader_(std::move(reader)) {}

    SstIdentity identity_;
    sstv2::file::Reader reader_;
};

} // namespace pl::minitable
