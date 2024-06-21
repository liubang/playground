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

#include "cpp/pl/fs/fs.h"
#include "cpp/pl/sst/block_builder.h"
#include "cpp/pl/sst/filter_block_builder.h"
#include "cpp/pl/sst/options.h"
#include "cpp/pl/sst/sstable_format.h"
#include "cpp/pl/binary/binary.h"
#include "cpp/pl/status/status.h"

namespace pl {

class SSTableBuilder {
public:
    SSTableBuilder(const BuildOptionsRef& options, const FsWriterRef& writer);

    SSTableBuilder(const SSTableBuilder&) = delete;

    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    ~SSTableBuilder();

    void add(const Binary& key, const Binary& value);

    void flush();

    Status finish();

    uint64_t entriesCount();

    uint64_t fileSize();

    [[nodiscard]] Status status() const { return status_; }

    [[nodiscard]] bool ok() const { return status().isOk(); }

private:
    void writeBlock(BlockBuilder* block, BlockHandle* handle);
    void writeBlockRaw(const Binary& content, CompressionType type, BlockHandle* handle);

private:
    const BuildOptionsRef options_;
    const FsWriterRef writer_;
    BlockBuilder data_block_;
    BlockBuilder index_block_;
    BlockHandle pending_handler_;
    FilterBlockBuilderPtr filter_block_;
    std::string first_key_; // min_key
    std::string last_key_;
    uint64_t key_nums_{0};
    uint64_t offset_{0};
    Status status_;
    bool pending_index_entry_{false};
    bool closed_{false};
};

} // namespace pl
