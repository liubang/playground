// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/05/10 15:00

#pragma once

#include <cstdint>

namespace pl::minidfs {

// Size constants
inline constexpr uint64_t kKB = 1024ULL;
inline constexpr uint64_t kMB = 1024ULL * kKB;
inline constexpr uint64_t kGB = 1024ULL * kMB;
inline constexpr uint64_t kTB = 1024ULL * kGB;
inline constexpr uint64_t kPB = 1024ULL * kTB;

// Block format constants
inline constexpr uint32_t kBlockMagic = 0x4D444653; // "MDFS" in little-endian
inline constexpr uint32_t kBlockFormatVersion = 1;
inline constexpr uint32_t kMaxChunkCount = 256;
inline constexpr uint32_t kDefaultChunkSize = 1 * kMB;

// Filesystem defaults
inline constexpr uint64_t kDefaultBlockSize = 128 * kMB;
inline constexpr uint32_t kDefaultReplication = 3;
inline constexpr uint32_t kMinWriteReplica = 2;
inline constexpr uint32_t kDefaultPermission = 0755;

// DataNode timing
inline constexpr uint64_t kDefaultHeartbeatIntervalMs = 3000;
inline constexpr uint64_t kDefaultStaleTimeoutMs = 30000;
inline constexpr uint64_t kDefaultDeadTimeoutMs = 600000;
inline constexpr uint64_t kDefaultBlockReportIntervalMs = 600000;

// Replication
inline constexpr uint64_t kDefaultReplicationScanIntervalMs = 30000;
inline constexpr uint32_t kDefaultMaxReplicationTasksPerRound = 100;

// Lease
inline constexpr uint64_t kDefaultLeaseTimeoutMs = 60000;

// IO
inline constexpr uint64_t kDefaultIOBufferSize = 1 * kMB;

// Namespace
inline constexpr uint64_t kMaxPathLength = 8192;
inline constexpr uint32_t kMaxNameLength = 255;

// ID allocation
inline constexpr uint64_t kRootInodeId = 1;

} // namespace pl::minidfs
