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
// Created: 2025/05/10 15:30

#pragma once

#include <cstdint>
#include <string_view>

namespace pl::minidfs {

// MiniDFS error codes, designed to be used with pl::Status.
// Ranges:
//   0       = OK
//   1xxx    = generic / namespace errors
//   2xxx    = lease errors
//   3xxx    = block / datanode errors
//   4xxx    = MySQL errors
//   5xxx    = RPC errors
//   6xxx    = IO errors
//   9xxx    = internal errors

enum class ErrorCode : uint16_t {
    kOK = 0,

    // Generic / Namespace errors
    kInvalidArgument = 1000,
    kNotFound = 1001,
    kAlreadyExists = 1002,
    kNotDirectory = 1003,
    kIsDirectory = 1004,
    kPermissionDenied = 1005,
    kDirectoryNotEmpty = 1006,
    kPathTooLong = 1007,
    kInvalidPath = 1008,

    // Lease errors
    kLeaseExpired = 2000,
    kLeaseConflict = 2001,
    kFileUnderConstruction = 2002,
    kLeaseNotFound = 2003,

    // Block / DataNode errors
    kNoAvailableDataNode = 3000,
    kBlockNotFound = 3001,
    kBlockCorrupt = 3002,
    kReplicaNotFound = 3003,
    kChecksumMismatch = 3004,
    kBlockAlreadyCommitted = 3005,
    kInsufficientReplicas = 3006,
    kInvalidBlockToken = 3007,

    // MySQL errors
    kMySQLError = 4000,
    kMySQLConnectFailed = 4001,
    kMySQLQueryFailed = 4002,
    kMySQLTxnFailed = 4003,

    // RPC errors
    kRPCError = 5000,
    kRPCTimeout = 5001,
    kRPCConnectFailed = 5002,
    kPipelineError = 5003,

    // IO errors
    kIOError = 6000,
    kDiskFull = 6001,
    kFileNotFound = 6002,

    // Internal errors
    kInternalError = 9000,
    kRequestDuplicated = 9001,
};

/// Convert an ErrorCode to a human-readable string.
constexpr std::string_view error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::kOK:
            return "OK";
        case ErrorCode::kInvalidArgument:
            return "InvalidArgument";
        case ErrorCode::kNotFound:
            return "NotFound";
        case ErrorCode::kAlreadyExists:
            return "AlreadyExists";
        case ErrorCode::kNotDirectory:
            return "NotDirectory";
        case ErrorCode::kIsDirectory:
            return "IsDirectory";
        case ErrorCode::kPermissionDenied:
            return "PermissionDenied";
        case ErrorCode::kDirectoryNotEmpty:
            return "DirectoryNotEmpty";
        case ErrorCode::kPathTooLong:
            return "PathTooLong";
        case ErrorCode::kInvalidPath:
            return "InvalidPath";
        case ErrorCode::kLeaseExpired:
            return "LeaseExpired";
        case ErrorCode::kLeaseConflict:
            return "LeaseConflict";
        case ErrorCode::kFileUnderConstruction:
            return "FileUnderConstruction";
        case ErrorCode::kLeaseNotFound:
            return "LeaseNotFound";
        case ErrorCode::kNoAvailableDataNode:
            return "NoAvailableDataNode";
        case ErrorCode::kBlockNotFound:
            return "BlockNotFound";
        case ErrorCode::kBlockCorrupt:
            return "BlockCorrupt";
        case ErrorCode::kReplicaNotFound:
            return "ReplicaNotFound";
        case ErrorCode::kChecksumMismatch:
            return "ChecksumMismatch";
        case ErrorCode::kBlockAlreadyCommitted:
            return "BlockAlreadyCommitted";
        case ErrorCode::kInsufficientReplicas:
            return "InsufficientReplicas";
        case ErrorCode::kInvalidBlockToken:
            return "InvalidBlockToken";
        case ErrorCode::kMySQLError:
            return "MySQLError";
        case ErrorCode::kMySQLConnectFailed:
            return "MySQLConnectFailed";
        case ErrorCode::kMySQLQueryFailed:
            return "MySQLQueryFailed";
        case ErrorCode::kMySQLTxnFailed:
            return "MySQLTxnFailed";
        case ErrorCode::kRPCError:
            return "RPCError";
        case ErrorCode::kRPCTimeout:
            return "RPCTimeout";
        case ErrorCode::kRPCConnectFailed:
            return "RPCConnectFailed";
        case ErrorCode::kPipelineError:
            return "PipelineError";
        case ErrorCode::kIOError:
            return "IOError";
        case ErrorCode::kDiskFull:
            return "DiskFull";
        case ErrorCode::kFileNotFound:
            return "FileNotFound";
        case ErrorCode::kInternalError:
            return "InternalError";
        case ErrorCode::kRequestDuplicated:
            return "RequestDuplicated";
    }
    return "Unknown";
}

} // namespace pl::minidfs
