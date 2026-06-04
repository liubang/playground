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

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/file/locator.h"
#include "cpp/pl/sstv2/file/tail.h"

namespace pl::sstv2::file {

class SSTableReader {
public:
    static absl::StatusOr<SSTableReader> open(std::string_view path);

    bool is_valid() const;
    const Tail& tail() const;
    const Locator& locator() const;
    std::span<const std::byte> file_data() const;

private:
    SSTableReader() = default;

    std::string data_;
    Tail tail_;
    Locator locator_;
    bool valid_ = false;
};

} // namespace pl::sstv2::file
