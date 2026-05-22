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
// Created: 2026/05/10 17:45

#include "cpp/pl/minidfs/datanode/pipeline_receiver.h"

#include <fmt/format.h>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

// ============================================================================
// Construction
// ============================================================================

PipelineReceiver::PipelineReceiver(LocalBlockStore* store) : store_(store) {}

// ============================================================================
// Pipeline Session Management
// ============================================================================

pl::Result<pl::Void> PipelineReceiver::setup(uint64_t block_id,
                                             uint64_t inode_id,
                                             uint32_t block_index,
                                             uint64_t generation_stamp,
                                             std::vector<PipelineTarget> downstream_targets) {
    // Create block in tmp/
    auto result = store_->create_block(block_id, inode_id, block_index, generation_stamp);
    if (result.hasError()) {
        return pl::makeError(std::move(result.error()));
    }

    active_block_id_ = block_id;
    active_generation_stamp_ = generation_stamp;
    downstream_targets_ = std::move(downstream_targets);

    RETURN_VOID;
}

pl::Result<AckStatus> PipelineReceiver::receive_packet(const PacketHeader& header,
                                                       const void* data,
                                                       uint32_t data_length) {
    // Verify packet belongs to active session
    if (header.block_id != active_block_id_ ||
        header.generation_stamp != active_generation_stamp_) {
        return AckStatus::kIOError;
    }

    // Verify CRC32C of received data
    uint32_t computed_crc = compute_crc32c(data, data_length);
    if (computed_crc != header.checksum) {
        return AckStatus::kChecksumError;
    }

    // Check available disk space
    auto avail = store_->available_bytes();
    if (avail.hasError() || avail.value() < data_length) {
        return AckStatus::kDiskFull;
    }

    // Write chunk to local store
    auto append_result =
        store_->append_chunk(header.block_id, header.generation_stamp, data, data_length);
    if (append_result.hasError()) {
        return AckStatus::kIOError;
    }

    // Forward to downstream if targets exist
    if (!downstream_targets_.empty()) {
        auto fwd_result = forward_to_downstream(header, data, data_length);
        if (fwd_result.hasError()) {
            return AckStatus::kDownstreamError;
        }
        if (fwd_result.value() != AckStatus::kSuccess) {
            return fwd_result.value();
        }
    }

    return AckStatus::kSuccess;
}

pl::Result<pl::Void> PipelineReceiver::finalize(uint64_t block_id, uint64_t generation_stamp) {
    if (block_id != active_block_id_ || generation_stamp != active_generation_stamp_) {
        return pl::makeError(pl::Status(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                                        "finalize called for non-active block"));
    }

    // Finalize locally
    auto result = store_->finalize_block(block_id, generation_stamp);
    if (result.hasError()) {
        return pl::makeError(std::move(result.error()));
    }

    // Forward finalize to downstream
    if (!downstream_targets_.empty()) {
        auto fwd_result = forward_finalize(block_id, generation_stamp);
        if (fwd_result.hasError()) {
            return pl::makeError(std::move(fwd_result.error()));
        }
    }

    // Reset session state
    active_block_id_ = 0;
    active_generation_stamp_ = 0;
    downstream_targets_.clear();

    RETURN_VOID;
}

void PipelineReceiver::abort(uint64_t block_id, uint64_t generation_stamp) {
    // Best-effort: remove the tmp file. Ignore errors.
    // The tmp block will be cleaned up on next restart if this fails.
    auto path = std::filesystem::path(store_->storage_root()) / "tmp" /
                fmt::format("blk_{}_{}.blk", block_id, generation_stamp);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    if (block_id == active_block_id_ && generation_stamp == active_generation_stamp_) {
        active_block_id_ = 0;
        active_generation_stamp_ = 0;
        downstream_targets_.clear();
    }
}

// ============================================================================
// Downstream Forwarding (default implementation — no-op for unit testing)
// ============================================================================

pl::Result<AckStatus> PipelineReceiver::forward_to_downstream(
    [[maybe_unused]] const PacketHeader& header,
    [[maybe_unused]] const void* data,
    [[maybe_unused]] uint32_t data_length) {
    // In production, this will be overridden by the brpc service layer
    // to actually send data to the next DN in the pipeline.
    // Default: assume success (for local-only testing).
    return AckStatus::kSuccess;
}

pl::Result<pl::Void> PipelineReceiver::forward_finalize(
    [[maybe_unused]] uint64_t block_id, [[maybe_unused]] uint64_t generation_stamp) {
    // Same as above — overridden in production.
    RETURN_VOID;
}

} // namespace pl::minidfs
