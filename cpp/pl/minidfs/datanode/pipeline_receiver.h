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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// ============================================================================
// PipelineTarget — identifies a downstream datanode in the write pipeline.
// ============================================================================

struct PipelineTarget {
    std::string host;
    uint32_t data_port = 0;
};

// ============================================================================
// PacketHeader — header for each data packet in the pipeline protocol.
//
// Wire format (network order):
//   [4B magic][8B block_id][8B generation_stamp][4B chunk_index][4B data_length][4B crc32c]
// ============================================================================

struct PacketHeader {
    static constexpr uint32_t kMagic = 0x4D504B54; // "MPKT"

    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint32_t chunk_index = 0;
    uint32_t data_length = 0;
    uint32_t checksum = 0; // CRC32C of the data payload
};

// ============================================================================
// AckStatus — status codes in pipeline acknowledgment messages.
// ============================================================================

enum class AckStatus : uint8_t {
    kSuccess = 0,
    kChecksumError = 1,
    kIOError = 2,
    kDiskFull = 3,
    kDownstreamError = 4,
};

// ============================================================================
// PipelineReceiver — handles the receiving side of the write pipeline.
//
// Protocol for a single block write:
//   1. Client (or upstream DN) opens a connection and sends SetupPacket
//      containing block_id, generation_stamp, inode_id, block_index, and
//      the list of downstream targets.
//   2. This DN opens a connection to the next downstream target (if any).
//   3. For each data chunk:
//      a. Receive PacketHeader + data payload from upstream.
//      b. Verify CRC32C of received data.
//      c. Write chunk to local store (append_chunk).
//      d. Forward PacketHeader + data to downstream (if any).
//      e. Wait for ACK from downstream.
//      f. Send ACK to upstream.
//   4. Receive FinalizePacket → finalize_block locally, forward, ACK.
//
// This class encapsulates the logic for steps 2-4. The actual network I/O
// is abstracted via virtual methods to allow unit testing without real sockets.
// In production, the brpc service layer invokes these methods.
// ============================================================================

class PipelineReceiver {
public:
    explicit PipelineReceiver(LocalBlockStore* store);
    ~PipelineReceiver() = default;

    PipelineReceiver(const PipelineReceiver&) = delete;
    PipelineReceiver& operator=(const PipelineReceiver&) = delete;

    /// Setup a new pipeline session for writing a block.
    /// Creates the block in tmp/ and records downstream targets.
    pl::Result<pl::Void> setup(uint64_t block_id,
                               uint64_t inode_id,
                               uint32_t block_index,
                               uint64_t generation_stamp,
                               std::vector<PipelineTarget> downstream_targets);

    /// Receive and process one data packet.
    /// Writes locally, forwards downstream (if targets exist), returns ACK status.
    pl::Result<AckStatus> receive_packet(const PacketHeader& header,
                                         const void* data,
                                         uint32_t data_length);

    /// Finalize the current block write session.
    /// Moves block from tmp/ to current/, notifies downstream.
    pl::Result<pl::Void> finalize(uint64_t block_id, uint64_t generation_stamp);

    /// Abort the current session (e.g., on error). Removes tmp block.
    void abort(uint64_t block_id, uint64_t generation_stamp);

    /// Get the downstream targets for the current session.
    const std::vector<PipelineTarget>& downstream_targets() const { return downstream_targets_; }

protected:
    /// Forward a packet to the next downstream datanode.
    /// Override in tests to avoid real network I/O.
    virtual pl::Result<AckStatus> forward_to_downstream(const PacketHeader& header,
                                                        const void* data,
                                                        uint32_t data_length);

    /// Forward finalize to downstream.
    virtual pl::Result<pl::Void> forward_finalize(uint64_t block_id, uint64_t generation_stamp);

private:
    LocalBlockStore* store_;
    std::vector<PipelineTarget> downstream_targets_;
    uint64_t active_block_id_ = 0;
    uint64_t active_generation_stamp_ = 0;
};

} // namespace pl::minidfs
