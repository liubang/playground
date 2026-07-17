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

#include "cpp/pl/minitable/core/sst_read_source.h"

#include <new>
#include <utility>

#include "absl/status/status.h"
#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/key_prefix.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {
namespace {

class SstCursor final : public sstv2::merge::ForwardCursor {
public:
    explicit SstCursor(sstv2::file::Reader::Iterator iterator) : iterator_(std::move(iterator)) {}

    absl::Status seek_to_first() override {
        auto status = iterator_.SeekToFirst();
        return status.ok() ? validate_current() : status;
    }

    absl::Status seek(std::string_view encoded_key) override {
        try {
            sstv2::file::KeyPrefix target{
                .key_columns = {
                    sstv2::types::Value::make<sstv2::types::DataType::kBinary>(encoded_key)}};
            auto status = iterator_.Seek(target);
            return status.ok() ? validate_current() : status;
        } catch (const std::bad_alloc&) {
            return absl::ResourceExhaustedError("SST seek key allocation failed");
        }
    }

    absl::Status next() override {
        auto status = iterator_.Next();
        return status.ok() ? validate_current() : status;
    }
    bool valid() const override { return iterator_.Valid(); }

    std::string_view key() const override {
        if (!valid()) {
            return {};
        }
        return iterator_.row().row_key().column(0).as_binary();
    }

    std::string_view value() const override {
        return valid() ? iterator_.row().value.as_binary() : std::string_view{};
    }

private:
    absl::Status validate_current() const {
        if (!iterator_.Valid()) {
            return absl::OkStatus();
        }
        const auto& row = iterator_.row();
        const auto system_key = row.system_key();
        if (row.row_key().column_count() != 1 ||
            row.row_key().column(0).type() != sstv2::types::DataType::kBinary ||
            row.value.type() != sstv2::types::DataType::kBinary ||
            system_key.version != sstv2::types::Version{} ||
            system_key.op_type != sstv2::types::OpType::kPut) {
            return absl::DataLossError("SST row is outside the minitable flush format domain");
        }
        return absl::OkStatus();
    }

    sstv2::file::Reader::Iterator iterator_;
};

bool ValidFinalizedIdentity(const sstv2::io::FileIdentity& identity) {
    return identity.file_id != 0 && identity.checksum_valid;
}

} // namespace

absl::StatusOr<std::shared_ptr<const SstReadSource>> SstReadSource::Open(
    std::shared_ptr<sstv2::io::FileSystem> filesystem, SstIdentity identity) {
    if (filesystem == nullptr || identity.key_path.empty() || identity.value_path.empty() ||
        !ValidFinalizedIdentity(identity.key_file) ||
        !ValidFinalizedIdentity(identity.value_file) ||
        identity.sst_format_version != kMinitableSstFormatVersion ||
        identity.comparator_domain != kMinitableComparatorDomain ||
        identity.checksum_algorithm != kCrc32cChecksumAlgorithm) {
        return absl::InvalidArgumentError("invalid finalized SST identity");
    }

    auto key = filesystem->open(identity.key_path, identity.key_file);
    if (!key.ok()) {
        return key.status();
    }
    auto value = filesystem->open(identity.value_path, identity.value_file);
    if (!value.ok()) {
        (void)filesystem->close(*key);
        return value.status();
    }
    // Reader::open consumes both handles on success and failure.
    auto reader = sstv2::file::Reader::open(filesystem, *key, *value);
    if (!reader.ok()) {
        return reader.status();
    }
    const auto& configuration = (*reader).configuration();
    if ((*reader).schema()->row_key_column_count() != 1 ||
        (*reader).schema()->column_type(0) != sstv2::types::DataType::kBinary ||
        (*reader).statistics().total_row_count != identity.row_count ||
        configuration.sst_format_version != identity.sst_format_version ||
        configuration.key_format_version != identity.comparator_domain.key_format_version ||
        configuration.row_key_schema_fingerprint !=
            identity.comparator_domain.row_key_schema_fingerprint ||
        configuration.comparator_domain_fingerprint != identity.comparator_domain.fingerprint ||
        configuration.checksum_algorithm != identity.checksum_algorithm) {
        return absl::DataLossError(
            "SST schema, row count, or comparator domain does not match flush identity");
    }
    try {
        return std::shared_ptr<const SstReadSource>(
            new SstReadSource(std::move(identity), std::move(*reader)));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SST read source allocation failed");
    }
}

absl::StatusOr<std::unique_ptr<sstv2::merge::ForwardCursor>> SstReadSource::new_cursor() const {
    auto iterator = reader_.new_iterator();
    if (!iterator.ok()) {
        return iterator.status();
    }
    try {
        return std::make_unique<SstCursor>(std::move(*iterator));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("SST cursor allocation failed");
    }
}

} // namespace pl::minitable
