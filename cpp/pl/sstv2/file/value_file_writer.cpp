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

#include "cpp/pl/sstv2/file/value_file_writer.h"

#include <fstream>

#include "absl/crc/crc32c.h"
#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::file {

void ValueHandle::encode(std::byte* dst) const {
    using pl::sstv2::encode::encode_fixed;
    size_t off = 0;
    encode_fixed<uint64_t>(file_id, dst + off);
    off += 8;
    encode_fixed<uint64_t>(offset, dst + off);
    off += 8;
    encode_fixed<uint32_t>(size, dst + off);
    off += 4;
    encode_fixed<uint32_t>(checksum, dst + off);
}

ValueHandle ValueHandle::decode(const std::byte* src) {
    using pl::sstv2::encode::decode_fixed;
    ValueHandle h;
    size_t off = 0;
    h.file_id = decode_fixed<uint64_t>(src + off);
    off += 8;
    h.offset = decode_fixed<uint64_t>(src + off);
    off += 8;
    h.size = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.checksum = decode_fixed<uint32_t>(src + off);
    return h;
}

ValueFileWriter::ValueFileWriter(uint64_t file_id, std::string_view path)
    : file_id_(file_id), path_(path) {}

absl::StatusOr<ValueHandle> ValueFileWriter::append(std::span<const std::byte> value) {
    ValueHandle handle;
    handle.file_id = file_id_;
    handle.offset = offset_;
    handle.size = static_cast<uint32_t>(value.size());

    auto crc = absl::ComputeCrc32c(
        std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
    handle.checksum = static_cast<uint32_t>(crc);

    buffer_.append(reinterpret_cast<const char*>(value.data()), value.size());
    offset_ += value.size();

    return handle;
}

absl::Status ValueFileWriter::finish() {
    std::ofstream out(path_, std::ios::binary);
    if (!out) {
        return absl::InternalError("failed to open value file for writing: " + path_);
    }
    out.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    if (!out) {
        return absl::InternalError("failed to write value file: " + path_);
    }
    return absl::OkStatus();
}

uint64_t ValueFileWriter::current_size() const {
    return offset_;
}

} // namespace pl::sstv2::file
