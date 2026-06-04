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

#include "cpp/pl/sstv2/file/value_file_reader.h"

#include <fstream>
#include <sstream>

#include "absl/crc/crc32c.h"

namespace pl::sstv2::file {

absl::StatusOr<ValueFileReader> ValueFileReader::open(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return absl::NotFoundError("failed to open value file: " + std::string(path));
    }

    ValueFileReader reader;
    std::ostringstream ss;
    ss << in.rdbuf();
    reader.data_ = ss.str();
    return reader;
}

absl::StatusOr<std::string> ValueFileReader::read(const ValueHandle& handle) const {
    if (handle.offset + handle.size > data_.size()) {
        return absl::OutOfRangeError("value handle exceeds file bounds");
    }

    std::string_view slice(data_.data() + handle.offset, handle.size);

    // Verify checksum
    auto crc = absl::ComputeCrc32c(slice);
    if (static_cast<uint32_t>(crc) != handle.checksum) {
        return absl::DataLossError("value checksum mismatch");
    }

    return std::string(slice);
}

} // namespace pl::sstv2::file
