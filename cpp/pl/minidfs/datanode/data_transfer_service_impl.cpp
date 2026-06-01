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
// Created: 2026/05/10 18:30

#include "cpp/pl/minidfs/datanode/data_transfer_service_impl.h"

#include <brpc/channel.h>
#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <folly/logging/xlog.h>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

void DataTransferServiceImpl::fill_status(protocol::StatusProto* proto,
                                          uint32_t code,
                                          std::string_view msg) {
    proto->set_code(code);
    if (!msg.empty()) {
        proto->set_message(std::string(msg));
    }
}

DataTransferServiceImpl::DataTransferServiceImpl(LocalBlockStore* store, BlockReporter* reporter)
    : store_(store), reporter_(reporter) {}

// WriteBlock — pipeline write handling.
// First call for a block creates it in tmp/. Subsequent calls append chunks.
// When is_last_chunk is true, the block is finalized.
void DataTransferServiceImpl::WriteBlock(google::protobuf::RpcController* /*controller*/,
                                         const protocol::WriteBlockRequest* request,
                                         protocol::WriteBlockResponse* response,
                                         google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    uint64_t block_id = request->block_id();
    uint64_t generation_stamp = request->generation_stamp();

    // If this is chunk 0, create the block first
    if (request->chunk_index() == 0) {
        auto create_result = store_->create_block(
            block_id, request->inode_id(), request->block_index(), generation_stamp);
        if (create_result.hasError()) {
            fill_status(response->mutable_status(),
                        create_result.error().code(),
                        create_result.error().message());
            response->set_ack_status(static_cast<uint32_t>(AckStatus::kIOError));
            return;
        }
    }

    // Verify checksum
    const auto& data = request->data();
    uint32_t computed_crc = compute_crc32c(data.data(), data.size());
    if (computed_crc != request->checksum()) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kChecksumMismatch),
                    "CRC32C mismatch on received chunk");
        response->set_ack_status(static_cast<uint32_t>(AckStatus::kChecksumError));
        return;
    }

    // Append chunk to local block
    auto append_result = store_->append_chunk(block_id,
                                              generation_stamp,
                                              data.data(),
                                              static_cast<uint32_t>(data.size()),
                                              request->chunk_index());
    if (append_result.hasError()) {
        fill_status(response->mutable_status(),
                    append_result.error().code(),
                    append_result.error().message());
        response->set_ack_status(static_cast<uint32_t>(AckStatus::kIOError));
        return;
    }

    // Forward to the next downstream pipeline target (if any)
    if (request->pipeline_size() > 0) {
        const auto& next_target = request->pipeline(0);
        std::string next_addr = next_target.host() + ":" + std::to_string(next_target.data_port());

        brpc::Channel downstream_channel;
        brpc::ChannelOptions opts;
        opts.timeout_ms = 10000;
        opts.max_retry = 1;

        if (downstream_channel.Init(next_addr.c_str(), &opts) != 0) {
            fill_status(response->mutable_status(),
                        static_cast<uint32_t>(ErrorCode::kRPCConnectFailed),
                        "Cannot connect to downstream DN: " + next_addr);
            response->set_ack_status(static_cast<uint32_t>(AckStatus::kIOError));
            return;
        }

        protocol::DataTransferService_Stub downstream_stub(&downstream_channel);
        brpc::Controller fwd_cntl;

        // Build forwarded request: pop the first pipeline target
        protocol::WriteBlockRequest fwd_req;
        fwd_req.set_block_id(request->block_id());
        fwd_req.set_inode_id(request->inode_id());
        fwd_req.set_block_index(request->block_index());
        fwd_req.set_generation_stamp(request->generation_stamp());
        fwd_req.set_data(request->data());
        fwd_req.set_chunk_index(request->chunk_index());
        fwd_req.set_checksum(request->checksum());
        fwd_req.set_is_last_chunk(request->is_last_chunk());

        // Pass remaining pipeline targets (skip first which is current downstream)
        for (int i = 1; i < request->pipeline_size(); ++i) {
            auto* t = fwd_req.add_pipeline();
            t->set_datanode_id(request->pipeline(i).datanode_id());
            t->set_host(request->pipeline(i).host());
            t->set_data_port(request->pipeline(i).data_port());
        }

        protocol::WriteBlockResponse fwd_resp;
        downstream_stub.WriteBlock(&fwd_cntl, &fwd_req, &fwd_resp, nullptr);

        if (fwd_cntl.Failed() || fwd_resp.status().code() != 0) {
            std::string err_msg =
                fwd_cntl.Failed() ? fwd_cntl.ErrorText() : fwd_resp.status().message();
            fill_status(response->mutable_status(),
                        static_cast<uint32_t>(ErrorCode::kPipelineError),
                        "Pipeline forwarding failed to " + next_addr + ": " + err_msg);
            response->set_ack_status(static_cast<uint32_t>(AckStatus::kIOError));
            return;
        }
    }

    // Finalize if last chunk
    if (request->is_last_chunk()) {
        auto finalize_result = store_->finalize_block(block_id, generation_stamp);
        if (finalize_result.hasError()) {
            fill_status(response->mutable_status(),
                        finalize_result.error().code(),
                        finalize_result.error().message());
            response->set_ack_status(static_cast<uint32_t>(AckStatus::kIOError));
            return;
        }

        // Notify block reporter
        if (reporter_) {
            reporter_->notify_block_finalized(block_id);
            (void)reporter_->send_incremental_report();
        }

        XLOGF(INFO, "block {}:{} finalized via pipeline", block_id, generation_stamp);
    }

    fill_status(response->mutable_status(), 0);
    response->set_ack_status(static_cast<uint32_t>(AckStatus::kSuccess));
}

// ReadBlock — client reads block data from DataNode
void DataTransferServiceImpl::ReadBlock(google::protobuf::RpcController* /*controller*/,
                                        const protocol::ReadBlockRequest* request,
                                        protocol::ReadBlockResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    uint64_t block_id = request->block_id();
    uint64_t generation_stamp = request->generation_stamp();
    uint64_t offset = request->offset();
    uint64_t length = request->length();

    auto data_result = store_->read_block_data(block_id, generation_stamp);
    if (data_result.hasError()) {
        fill_status(
            response->mutable_status(), data_result.error().code(), data_result.error().message());
        return;
    }

    const auto& block_data = data_result.value();

    // Handle offset and length
    if (offset >= block_data.size()) {
        fill_status(response->mutable_status(), 0);
        response->set_length(0);
        return;
    }

    uint64_t available = block_data.size() - offset;
    uint64_t to_return = (length == 0) ? available : std::min(length, available);

    response->set_data(block_data.data() + offset, to_return);
    response->set_length(to_return);
    response->set_checksum(compute_crc32c(block_data.data() + offset, to_return));
    fill_status(response->mutable_status(), 0);
}

// TransferBlock — full-block replication.
// Receives a complete block data payload, creates + writes + finalizes in one shot.
void DataTransferServiceImpl::TransferBlock(google::protobuf::RpcController* /*controller*/,
                                            const protocol::TransferBlockRequest* request,
                                            protocol::TransferBlockResponse* response,
                                            google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    uint64_t block_id = request->block_id();
    uint64_t generation_stamp = request->generation_stamp();
    const auto& data = request->data();

    // Create block in tmp/
    auto create_result = store_->create_block(block_id,
                                              request->inode_id(),
                                              request->block_index(),
                                              generation_stamp);
    if (create_result.hasError()) {
        fill_status(response->mutable_status(),
                    create_result.error().code(),
                    create_result.error().message());
        return;
    }

    // Write data as a single chunk
    if (!data.empty()) {
        auto append_result = store_->append_chunk(block_id,
                                                  generation_stamp,
                                                  data.data(),
                                                  static_cast<uint32_t>(data.size()),
                                                  /*chunk_index=*/0);
        if (append_result.hasError()) {
            fill_status(response->mutable_status(),
                        append_result.error().code(),
                        append_result.error().message());
            return;
        }
    }

    // Finalize
    auto finalize_result = store_->finalize_block(block_id, generation_stamp);
    if (finalize_result.hasError()) {
        fill_status(response->mutable_status(),
                    finalize_result.error().code(),
                    finalize_result.error().message());
        return;
    }

    if (reporter_) {
        reporter_->notify_block_finalized(block_id);
        (void)reporter_->send_incremental_report();
    }

    XLOGF(INFO,
          "block {}:{} received via transfer replication ({} bytes)",
          block_id,
          generation_stamp,
          data.size());

    fill_status(response->mutable_status(), 0);
}

void DataTransferServiceImpl::TruncateBlock(google::protobuf::RpcController* /*controller*/,
                                            const protocol::TruncateBlockRequest* request,
                                            protocol::TruncateBlockResponse* response,
                                            google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    auto truncate =
        store_->truncate_block(request->block_id(), request->generation_stamp(), request->length());
    if (truncate.hasError()) {
        fill_status(response->mutable_status(), truncate.error().code(), truncate.error().message());
        return;
    }
    if (reporter_) {
        reporter_->notify_block_finalized(request->block_id());
        (void)reporter_->send_incremental_report();
    }
    fill_status(response->mutable_status(), 0);
}

} // namespace pl::minidfs
