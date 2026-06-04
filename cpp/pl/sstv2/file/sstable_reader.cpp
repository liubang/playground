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

#include "cpp/pl/sstv2/file/sstable_reader.h"

#include <fstream>
#include <sstream>

#include "absl/crc/crc32c.h"

namespace pl::sstv2::file {

absl::StatusOr<SSTableReader> SSTableReader::open(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return absl::NotFoundError("failed to open SST file: " + std::string(path));
    }

    SSTableReader reader;
    std::ostringstream ss;
    ss << in.rdbuf();
    reader.data_ = ss.str();

    if (reader.data_.size() < Tail::kSize) {
        return absl::InvalidArgumentError("file too small to contain tail");
    }

    // Decode tail from the last 32 bytes
    auto tail_span = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(reader.data_.data() + reader.data_.size() - Tail::kSize),
        Tail::kSize);
    auto tail_result = Tail::decode_from(tail_span);
    if (!tail_result.ok()) {
        return tail_result.status();
    }
    reader.tail_ = *tail_result;

    // Verify file checksum (everything before tail)
    size_t content_size = reader.data_.size() - Tail::kSize;
    auto file_crc = absl::ComputeCrc32c(std::string_view(reader.data_.data(), content_size));
    if (static_cast<uint64_t>(static_cast<uint32_t>(file_crc)) != reader.tail_.file_checksum) {
        return absl::DataLossError("file checksum mismatch");
    }

    // Verify locator checksum
    if (reader.tail_.locator_offset + reader.tail_.locator_size > content_size) {
        return absl::InvalidArgumentError("locator extends beyond file content");
    }
    std::string_view locator_view(reader.data_.data() + reader.tail_.locator_offset,
                                  reader.tail_.locator_size);
    auto locator_crc = absl::ComputeCrc32c(locator_view);
    if (static_cast<uint32_t>(locator_crc) != reader.tail_.locator_checksum) {
        return absl::DataLossError("locator checksum mismatch");
    }

    // Deserialize locator
    auto locator_span = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(locator_view.data()), locator_view.size());
    auto locator_result = Locator::deserialize(locator_span);
    if (!locator_result.ok()) {
        return locator_result.status();
    }
    reader.locator_ = *locator_result;
    reader.valid_ = true;

    return reader;
}

bool SSTableReader::is_valid() const {
    return valid_;
}

const Tail& SSTableReader::tail() const {
    return tail_;
}

const Locator& SSTableReader::locator() const {
    return locator_;
}

std::span<const std::byte> SSTableReader::file_data() const {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data_.data()),
                                      data_.size());
}

} // namespace pl::sstv2::file
