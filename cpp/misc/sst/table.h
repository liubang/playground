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

#include "cpp/misc/fs/fs.h"
#include "cpp/misc/sst/block.h"
#include "cpp/misc/sst/filter_block_reader.h"
#include "cpp/misc/sst/options.h"
#include "cpp/misc/sst/sstable_format.h"
#include "cpp/tools/status.h"

#include <functional>

namespace pl {

using HandleResult = std::function<void(void* arg, const Binary& k, const Binary& v)>;

class Table {
public:
    Table(const Table&) = delete;

    Table& operator=(const Table&) = delete;

    ~Table();

    static Status open(const Options* options, FsReader* reader, uint64_t size, Table** table);

    Status get(const Binary& key, void* arg, HandleResult&& handle_result);

private:
    Table(const Options* options, FsReader* reader, Block* index_block);

    void readMeta(const Footer& footer);
    void readFilter(const Binary& filter_handle_content);
    Iterator* blockReader(const Binary& index_value);

private:
    const Options* options_;
    FsReader* reader_;
    Status status_;
    FilterBlockReader* filter_{nullptr};
    const char* filter_data_{nullptr};
    // BlockHandle metaindex_Handle_;
    Block* index_block_{nullptr};
};

} // namespace pl
