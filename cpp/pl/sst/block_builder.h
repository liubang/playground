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

#include "cpp/pl/sst/cell.h"
#include "cpp/pl/sst/comparator.h"
#include "cpp/pl/sst/options.h"
#include <string>
#include <vector>

namespace pl {

/**
 * data block and index block builder
 */
class BlockBuilder {
public:
    BlockBuilder(const BuildOptionsRef& options);

    BlockBuilder(const BlockBuilder&) = delete;

    BlockBuilder& operator=(const BlockBuilder&) = delete;

    void add(const Cell& cell);

    std::string_view finish();

    [[nodiscard]] bool empty() const { return buffer_.empty(); }

    [[nodiscard]] std::size_t sizeEstimate() const;

    void reset();

private:
    const ComparatorRef comparator_;
    bool finished_{false};
    int counter_{0};
    std::string buffer_;
    std::string last_key_;        // last cell key
    uint32_t last_rowkey_len_{0}; // last rowkey len
    int block_restart_interval_{0};
    std::vector<uint32_t> restarts_;
};

using BlockBuilderPtr = std::unique_ptr<BlockBuilder>;
using BlockBuilderRef = std::shared_ptr<BlockBuilder>;

} // namespace pl
