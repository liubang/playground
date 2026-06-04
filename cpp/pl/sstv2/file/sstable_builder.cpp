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

#include "cpp/pl/sstv2/file/sstable_builder.h"

#include <fstream>

#include "absl/crc/crc32c.h"
#include "cpp/pl/sstv2/file/locator.h"
#include "cpp/pl/sstv2/file/tail.h"

namespace pl::sstv2::file {

SSTableBuilder::SSTableBuilder(std::string_view output_path, Options opts)
    : opts_(std::move(opts)), output_path_(output_path) {}

absl::Status SSTableBuilder::add(std::string_view key, std::string_view value) {
    if (finished_ || aborted_) {
        return absl::FailedPreconditionError("builder is already finished or aborted");
    }

    // Simplified: store key-value as [key_len(4B)][key][value_len(4B)][value]
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());

    buffer_.append(reinterpret_cast<const char*>(&key_len), 4);
    buffer_.append(key);
    buffer_.append(reinterpret_cast<const char*>(&val_len), 4);
    buffer_.append(value);

    ++total_rows_;
    data_size_ += key.size() + value.size() + 8;
    return absl::OkStatus();
}

absl::Status SSTableBuilder::finish() {
    if (finished_) {
        return absl::FailedPreconditionError("already finished");
    }
    if (aborted_) {
        return absl::FailedPreconditionError("builder was aborted");
    }
    finished_ = true;

    // Build locator with the data block section
    Locator locator;
    uint64_t data_offset = 0;
    uint64_t data_len = buffer_.size();
    locator.add(kSectionDataBlocks, data_offset, data_len);

    // Serialize locator and append
    std::string locator_data = locator.serialize();
    uint64_t locator_offset = buffer_.size();
    buffer_.append(locator_data);

    // Compute locator checksum
    auto locator_crc = absl::ComputeCrc32c(locator_data);

    // Compute file checksum (everything before the tail)
    auto file_crc = absl::ComputeCrc32c(std::string_view(buffer_.data(), buffer_.size()));

    // Build and append tail
    Tail tail;
    tail.locator_offset = locator_offset;
    tail.locator_size = static_cast<uint32_t>(locator_data.size());
    tail.locator_checksum = static_cast<uint32_t>(locator_crc);
    tail.file_checksum = static_cast<uint64_t>(static_cast<uint32_t>(file_crc));

    std::byte tail_buf[Tail::kSize];
    tail.encode_to(tail_buf);
    buffer_.append(reinterpret_cast<const char*>(tail_buf), Tail::kSize);

    // Write to file
    std::ofstream out(output_path_, std::ios::binary);
    if (!out) {
        return absl::InternalError("failed to open output file: " + output_path_);
    }
    out.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    if (!out) {
        return absl::InternalError("failed to write output file: " + output_path_);
    }

    return absl::OkStatus();
}

void SSTableBuilder::abort() {
    aborted_ = true;
}

uint64_t SSTableBuilder::total_rows() const {
    return total_rows_;
}

uint64_t SSTableBuilder::data_size() const {
    return data_size_;
}

} // namespace pl::sstv2::file
