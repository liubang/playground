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
// Created: 2026/06/04 13:06

#include "cpp/pl/sstv2/block/data_table.h"

namespace pl::sstv2::block {

uint32_t DataTableBuilder::append(std::string_view data) {
    uint32_t offset = static_cast<uint32_t>(buffer_.size());
    buffer_.append(data);
    return offset;
}

std::string DataTableBuilder::build() const {
    return buffer_;
}

size_t DataTableBuilder::current_size() const {
    return buffer_.size();
}

DataTableReader::DataTableReader(std::span<const std::byte> data) : data_(data) {}

std::string_view DataTableReader::get(uint32_t offset, uint32_t length) const {
    return {reinterpret_cast<const char*>(data_.data() + offset), length};
}

} // namespace pl::sstv2::block
