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

#include "cpp/pl/arena/arena.h"
#include "cpp/pl/fs/fs.h"
#include "cpp/pl/sst/block.h"
#include "cpp/pl/sst/filter_block_reader.h"
#include "cpp/pl/sst/options.h"
#include "cpp/pl/sst/sstable_format.h"
#include "cpp/pl/status/result.h"

#include <cassert>
#include <filesystem>

namespace pl {

class SSTable;
using SSTablePtr = std::unique_ptr<SSTable>;
using SSTableRef = std::shared_ptr<SSTable>;

class SSTable {
public:
    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;
    ~SSTable() = default;

    static Result<SSTablePtr> open(const ReadOptionsRef& options,
                                   const std::filesystem::path& sst_file);

    [[nodiscard]] const FileMetaRef& fileMeta() const { return file_meta_; }

    [[nodiscard]] SSTId sstId() const {
        assert(file_meta_ != nullptr);
        return file_meta_->sstId();
    }

    [[nodiscard]] PatchId patchId() const {
        assert(file_meta_ != nullptr);
        return file_meta_->patchId();
    }

    Result<CellVecRef> get(std::string_view rowkey, Arena* buf);

    IteratorPtr iterator();

private:
    SSTable(ReadOptionsRef options,
            FileDescriptorRef fd,
            FileSystemRef reader,
            FileMetaRef file_meta,
            BlockRef index_block);

    Result<Void> readFilter(const Footer& footer);
    IteratorPtr blockReader(std::string_view index_value);

private:
    const ReadOptionsRef options_;
    FileDescriptorRef fd_;
    FileSystemRef reader_;
    FilterBlockReaderRef filter_;
    std::unique_ptr<const char[]> filter_data_;
    FileMetaRef file_meta_{nullptr};
    BlockRef index_block_{nullptr};
};

} // namespace pl
