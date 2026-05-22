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

#pragma once

#include <brpc/channel.h>
#include <cstdint>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// ============================================================================
// DfsInputStream — 流式读取 DFS 文件
//
// 支持顺序读取和 seek 定位，按 block 粒度从 DataNode 拉取数据。
// 自动处理副本容错：当前副本读取失败时切换到下一个副本。
// ============================================================================

class DfsInputStream {
public:
    ~DfsInputStream() = default;

    DfsInputStream(const DfsInputStream&) = delete;
    DfsInputStream& operator=(const DfsInputStream&) = delete;
    DfsInputStream(DfsInputStream&&) noexcept = default;
    DfsInputStream& operator=(DfsInputStream&&) noexcept = default;

    /// 从 LocatedBlocks 构造（由 DfsClient 内部调用）
    static pl::Result<DfsInputStream> open(std::vector<LocatedBlock> blocks, uint64_t file_length);

    /// 读取至多 len 字节到 buf，返回实际读取字节数，0 表示 EOF
    pl::Result<uint64_t> read(void* buf, uint64_t len);

    /// 定位到文件内指定偏移量
    pl::Result<pl::Void> seek(uint64_t offset);

    /// 当前读取位置
    [[nodiscard]] uint64_t position() const { return pos_; }

    /// 文件总长度
    [[nodiscard]] uint64_t length() const { return file_length_; }

    /// 是否已到达文件末尾
    [[nodiscard]] bool eof() const { return pos_ >= file_length_; }

    /// 剩余可读字节数
    [[nodiscard]] uint64_t remaining() const { return file_length_ - pos_; }

private:
    DfsInputStream(std::vector<LocatedBlock> blocks, uint64_t file_length);

    /// 从 DataNode 读取指定 block 的数据
    pl::Result<std::string> read_block_from_datanode(const LocatedBlock& block,
                                                     uint64_t offset_in_block,
                                                     uint64_t length);

    std::vector<LocatedBlock> blocks_;
    uint64_t file_length_ = 0;
    uint64_t pos_ = 0;

    // 当前 block 缓存
    uint32_t current_block_index_ = 0;
    std::string current_block_data_;
    uint64_t current_block_offset_ = 0; // block 在文件中的起始 offset
};

} // namespace pl::minidfs
