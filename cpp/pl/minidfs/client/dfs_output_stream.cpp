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
// Created: 2026/06/01 23:16

#include "cpp/pl/minidfs/client/dfs_output_stream.h"

#include <algorithm>
#include <brpc/controller.h>
#include <cstring>
#include <utility>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {

namespace {

pl::Result<pl::Void> check_status(const protocol::StatusProto& status) {
    if (status.code() == 0) {
        return pl::Void{};
    }
    return pl::makeError(static_cast<pl::status_code_t>(status.code()), status.message());
}

} // namespace

DfsOutputStream::DfsOutputStream(brpc::Channel* namenode_channel,
                                 uint64_t inode_id,
                                 std::string client_id,
                                 DfsOutputStreamOptions options)
    : namenode_channel_(namenode_channel),
      inode_id_(inode_id),
      client_id_(std::move(client_id)),
      options_(options),
      current_block_index_(options.starting_block_index) {
    buffer_.reserve(options_.block_size);
}

DfsOutputStream::~DfsOutputStream() = default;

DfsOutputStream::DfsOutputStream(DfsOutputStream&& other) noexcept
    : namenode_channel_(std::exchange(other.namenode_channel_, nullptr)),
      inode_id_(std::exchange(other.inode_id_, 0)),
      client_id_(std::move(other.client_id_)),
      options_(other.options_),
      buffer_(std::move(other.buffer_)),
      current_block_index_(std::exchange(other.current_block_index_, 0)),
      total_bytes_written_(std::exchange(other.total_bytes_written_, 0)),
      closed_(std::exchange(other.closed_, true)) {}

pl::Result<DfsOutputStream> DfsOutputStream::create(brpc::Channel* namenode_channel,
                                                    uint64_t inode_id,
                                                    std::string_view client_id,
                                                    DfsOutputStreamOptions options) {
    if (namenode_channel == nullptr || inode_id == 0 || options.block_size == 0 ||
        options.chunk_size == 0 || options.replication == 0 || options.rpc_timeout_ms <= 0) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                             "invalid output stream configuration");
    }
    return DfsOutputStream(namenode_channel, inode_id, std::string(client_id), options);
}

pl::Result<pl::Void> DfsOutputStream::write(const void* data, uint64_t len) {
    if (closed_) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIOError),
                             "output stream is closed");
    }
    if (data == nullptr && len > 0) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                             "write data must not be null");
    }
    const auto* bytes = static_cast<const char*>(data);
    while (len > 0) {
        uint64_t writable = std::min<uint64_t>(len, options_.block_size - buffer_.size());
        buffer_.append(bytes, writable);
        bytes += writable;
        len -= writable;
        total_bytes_written_ += writable;
        if (buffer_.size() == options_.block_size) {
            auto flushed = flush_block();
            if (flushed.hasError()) {
                return folly::makeUnexpected(flushed.error());
            }
        }
    }
    return pl::Void{};
}

pl::Result<pl::Void> DfsOutputStream::flush() {
    if (closed_) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kIOError),
                             "output stream is closed");
    }
    return flush_block();
}

pl::Result<pl::Void> DfsOutputStream::renew_lease() {
    protocol::NameNodeService_Stub stub(namenode_channel_);
    brpc::Controller controller;
    protocol::RenewLeaseRequest request;
    request.set_inode_id(inode_id_);
    request.set_client_id(client_id_);
    protocol::RenewLeaseResponse response;
    stub.RenewLease(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCError),
                             controller.ErrorText());
    }
    return check_status(response.status());
}

pl::Result<pl::Void> DfsOutputStream::flush_block() {
    if (buffer_.empty()) {
        return pl::Void{};
    }
    auto renewed = renew_lease();
    if (renewed.hasError()) {
        return renewed;
    }
    auto result = write_block_pipeline(current_block_index_, buffer_.data(), buffer_.size());
    if (result.hasError()) {
        return result;
    }
    buffer_.clear();
    ++current_block_index_;
    return pl::Void{};
}

pl::Result<pl::Void> DfsOutputStream::write_block_pipeline(uint32_t block_index,
                                                           const void* data,
                                                           uint64_t len) {
    protocol::NameNodeService_Stub namenode(namenode_channel_);
    brpc::Controller controller;
    protocol::AllocateBlockRequest allocate;
    allocate.set_inode_id(inode_id_);
    allocate.set_block_index(block_index);
    allocate.set_replication(options_.replication);
    allocate.set_client_id(client_id_);
    protocol::AllocateBlockResponse allocated;
    namenode.AllocateBlock(&controller, &allocate, &allocated, nullptr);
    if (controller.Failed()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCError),
                             controller.ErrorText());
    }
    auto status = check_status(allocated.status());
    if (status.hasError()) {
        return status;
    }
    const auto& block = allocated.block();
    if (block.locations().empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNoAvailableDataNode),
                             "no DataNode available for block");
    }

    const auto& primary = block.locations(0);
    brpc::Channel datanode_channel;
    brpc::ChannelOptions options;
    options.timeout_ms = options_.rpc_timeout_ms * 2;
    options.max_retry = 1;
    std::string address = primary.host() + ":" + std::to_string(primary.data_port());
    if (datanode_channel.Init(address.c_str(), &options) != 0) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCConnectFailed),
                             "failed to connect to DataNode");
    }

    protocol::DataTransferService_Stub datanode(&datanode_channel);
    const auto* bytes = static_cast<const char*>(data);
    uint64_t offset = 0;
    uint32_t chunk_index = 0;
    while (offset < len) {
        uint32_t size =
            static_cast<uint32_t>(std::min<uint64_t>(options_.chunk_size, len - offset));
        brpc::Controller write_controller;
        protocol::WriteBlockRequest request;
        request.set_block_id(block.block_id());
        request.set_inode_id(inode_id_);
        request.set_block_index(block_index);
        request.set_generation_stamp(block.generation_stamp());
        for (int i = 1; i < block.locations_size(); ++i) {
            *request.add_pipeline() = block.locations(i);
        }
        request.set_data(bytes + offset, size);
        request.set_chunk_index(chunk_index);
        request.set_checksum(compute_crc32c(bytes + offset, size));
        request.set_is_last_chunk(offset + size == len);
        protocol::WriteBlockResponse response;
        datanode.WriteBlock(&write_controller, &request, &response, nullptr);
        if (write_controller.Failed()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCError),
                                 write_controller.ErrorText());
        }
        auto write_status = check_status(response.status());
        if (write_status.hasError()) {
            return write_status;
        }
        offset += size;
        ++chunk_index;
    }

    controller.Reset();
    protocol::DataNodeProtocolService_Stub datanode_protocol(namenode_channel_);
    protocol::CommitBlockRequest commit;
    commit.set_block_id(block.block_id());
    commit.set_length(len);
    commit.set_generation_stamp(block.generation_stamp());
    for (const auto& location : block.locations()) {
        commit.add_finalized_datanode_ids(location.datanode_id());
    }
    protocol::CommitBlockResponse committed;
    datanode_protocol.CommitBlock(&controller, &commit, &committed, nullptr);
    if (controller.Failed()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCError),
                             controller.ErrorText());
    }
    return check_status(committed.status());
}

pl::Result<pl::Void> DfsOutputStream::close() {
    if (closed_) {
        return pl::Void{};
    }
    auto flushed = flush_block();
    if (flushed.hasError()) {
        return flushed;
    }
    protocol::NameNodeService_Stub stub(namenode_channel_);
    brpc::Controller controller;
    protocol::CompleteFileRequest request;
    request.set_inode_id(inode_id_);
    request.set_client_id(client_id_);
    protocol::CompleteFileResponse response;
    stub.CompleteFile(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCError),
                             controller.ErrorText());
    }
    auto status = check_status(response.status());
    if (status.hasError()) {
        return status;
    }
    closed_ = true;
    return pl::Void{};
}

} // namespace pl::minidfs
