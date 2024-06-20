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

class SSTable {
public:
    SSTable(const SSTable&) = delete;

    SSTable& operator=(const SSTable&) = delete;

    ~SSTable() = default;

    static std::unique_ptr<SSTable> open(const ReadOptionsRef& options,
                                         const FsReaderRef& reader,
                                         uint64_t size,
                                         Status* status);

    [[nodiscard]] const FileMetaRef& fileMeta() const { return file_meta_; }

    SSTId sstId() const {
        assert(file_meta_ != nullptr);
        return file_meta_->sstId();
    }

    PatchId patchId() const {
        assert(file_meta_ != nullptr);
        return file_meta_->patchId();
    }

    Status get(const Binary& key, void* arg, HandleResult&& handle_result);

    IteratorPtr iterator();

private:
    SSTable(ReadOptionsRef options,
            FsReaderRef reader,
            FileMetaRef file_meta,
            BlockRef index_block);

    void readFilter(const Footer& footer);
    IteratorPtr blockReader(const Binary& index_value);

private:
    const ReadOptionsRef options_;
    const FsReaderRef reader_;
    Status status_;
    FilterBlockReaderPtr filter_{nullptr};
    std::unique_ptr<const char[]> filter_data_;
    FileMetaRef file_meta_{nullptr};
    BlockRef index_block_{nullptr};
};

using SSTablePtr = std::unique_ptr<SSTable>;
using SSTableRef = std::shared_ptr<SSTable>;

} // namespace pl
