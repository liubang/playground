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

struct DfsOutputStreamOptions {
    uint64_t block_size = 0;
    uint64_t chunk_size = 0;
    uint32_t replication = 0;
    int32_t rpc_timeout_ms = 0;
    uint32_t starting_block_index = 0;
};

// DfsOutputStream — 流式写入 DFS 文件
//
// 内部按 block_size 分块，每满一个 block 即向 NameNode AllocateBlock 后
// 通过 pipeline 写入 DataNode。close() 时提交最后一个 block 并 CompleteFile。
class DfsOutputStream {
public:
    ~DfsOutputStream();

    DfsOutputStream(const DfsOutputStream&) = delete;
    DfsOutputStream& operator=(const DfsOutputStream&) = delete;
    DfsOutputStream(DfsOutputStream&& other) noexcept;
    DfsOutputStream& operator=(DfsOutputStream&& other) = delete;

    /// 创建输出流（由 DfsClient 内部调用）
    /// namenode_channel: 连接 NameNode 的 brpc channel
    /// inode_id: 已创建的 under_construction 文件
    /// options: 写入参数
    static pl::Result<DfsOutputStream> create(brpc::Channel* namenode_channel,
                                              uint64_t inode_id,
                                              std::string_view client_id,
                                              DfsOutputStreamOptions options);

    /// 写入数据，可以任意长度，内部自动按 block/chunk 分片
    pl::Result<pl::Void> write(const void* data, uint64_t len);

    /// 刷新当前缓冲区中的数据到 DataNode（不一定填满一个 block）
    pl::Result<pl::Void> flush();

    /// 关闭流：提交最后一个 block，调用 CompleteFile
    pl::Result<pl::Void> close();

    /// 已写入的总字节数
    [[nodiscard]] uint64_t bytes_written() const { return total_bytes_written_; }

    /// 是否已关闭
    [[nodiscard]] bool closed() const { return closed_; }

private:
    DfsOutputStream(brpc::Channel* namenode_channel,
                    uint64_t inode_id,
                    std::string client_id,
                    DfsOutputStreamOptions options);

    /// 将当前 buffer 作为一个完整 block 写入 DataNode
    pl::Result<pl::Void> flush_block();

    /// 续租，避免长时间流式写入被 lease recovery 回收
    pl::Result<pl::Void> renew_lease();

    /// 通过 pipeline 写入一个 block 的数据到 DataNode
    pl::Result<pl::Void> write_block_pipeline(uint32_t block_index, const void* data, uint64_t len);

    brpc::Channel* namenode_channel_ = nullptr;
    uint64_t inode_id_ = 0;
    std::string client_id_;
    DfsOutputStreamOptions options_;

    std::string buffer_; // 当前 block 的数据缓冲区
    uint32_t current_block_index_ = 0;
    uint64_t total_bytes_written_ = 0;
    bool closed_ = false;
};

} // namespace pl::minidfs
