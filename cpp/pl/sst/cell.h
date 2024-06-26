// Copyright (c) 2024 The Authors. All rights reserved.
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

#pragma once

#include <cstdint>

namespace pl {

enum class CellType : uint8_t {
    CT_DEL = 0,
    CT_PUT = 1,
    CT_READ = 2,
    CT_NONE = 3,
};

struct CellHeader {
    uint16_t rowkey_len;
    uint8_t cf_len;
    uint8_t col_len;
    uint32_t body_len;
};

struct CellKey {
    const char* rowkey{nullptr};
    const char* qualifier{nullptr};
};

class Cell {
public:
    Cell() = default;

private:
    CellHeader header_;
    CellKey cell_key_;
    const char* value_{nullptr};
    uint64_t timestamp_;
    CellType type_;
};

} // namespace pl
