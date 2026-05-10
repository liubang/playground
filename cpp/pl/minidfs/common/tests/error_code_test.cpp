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

#include "cpp/pl/minidfs/common/error_code.h"

#include <gtest/gtest.h>
#include <string_view>

namespace pl::minidfs {
namespace {

// ============================================================================
// ErrorCode value tests
// ============================================================================

TEST(ErrorCodeTest, OKIsZero) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kOK), 0u);
}

TEST(ErrorCodeTest, NamespaceErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kInvalidArgument), 1000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kNotFound), 1001u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kAlreadyExists), 1002u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kNotDirectory), 1003u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kIsDirectory), 1004u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kPermissionDenied), 1005u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kDirectoryNotEmpty), 1006u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kPathTooLong), 1007u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kInvalidPath), 1008u);
}

TEST(ErrorCodeTest, LeaseErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kLeaseExpired), 2000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kLeaseConflict), 2001u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kFileUnderConstruction), 2002u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kLeaseNotFound), 2003u);
}

TEST(ErrorCodeTest, BlockDataNodeErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kNoAvailableDataNode), 3000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kBlockNotFound), 3001u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kBlockCorrupt), 3002u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kReplicaNotFound), 3003u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kChecksumMismatch), 3004u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kBlockAlreadyCommitted), 3005u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kInsufficientReplicas), 3006u);
}

TEST(ErrorCodeTest, MySQLErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kMySQLError), 4000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kMySQLConnectFailed), 4001u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kMySQLQueryFailed), 4002u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kMySQLTxnFailed), 4003u);
}

TEST(ErrorCodeTest, RPCErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kRPCError), 5000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kRPCTimeout), 5001u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kRPCConnectFailed), 5002u);
}

TEST(ErrorCodeTest, IOErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kIOError), 6000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kDiskFull), 6001u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kFileNotFound), 6002u);
}

TEST(ErrorCodeTest, InternalErrorRange) {
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kInternalError), 9000u);
    EXPECT_EQ(static_cast<uint16_t>(ErrorCode::kRequestDuplicated), 9001u);
}

// ============================================================================
// error_code_name tests
// ============================================================================

TEST(ErrorCodeTest, NameOK) {
    EXPECT_EQ(error_code_name(ErrorCode::kOK), "OK");
}

TEST(ErrorCodeTest, NameNamespaceErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kInvalidArgument), "InvalidArgument");
    EXPECT_EQ(error_code_name(ErrorCode::kNotFound), "NotFound");
    EXPECT_EQ(error_code_name(ErrorCode::kAlreadyExists), "AlreadyExists");
    EXPECT_EQ(error_code_name(ErrorCode::kNotDirectory), "NotDirectory");
    EXPECT_EQ(error_code_name(ErrorCode::kIsDirectory), "IsDirectory");
    EXPECT_EQ(error_code_name(ErrorCode::kPermissionDenied), "PermissionDenied");
    EXPECT_EQ(error_code_name(ErrorCode::kDirectoryNotEmpty), "DirectoryNotEmpty");
    EXPECT_EQ(error_code_name(ErrorCode::kPathTooLong), "PathTooLong");
    EXPECT_EQ(error_code_name(ErrorCode::kInvalidPath), "InvalidPath");
}

TEST(ErrorCodeTest, NameLeaseErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kLeaseExpired), "LeaseExpired");
    EXPECT_EQ(error_code_name(ErrorCode::kLeaseConflict), "LeaseConflict");
    EXPECT_EQ(error_code_name(ErrorCode::kFileUnderConstruction), "FileUnderConstruction");
    EXPECT_EQ(error_code_name(ErrorCode::kLeaseNotFound), "LeaseNotFound");
}

TEST(ErrorCodeTest, NameBlockErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kNoAvailableDataNode), "NoAvailableDataNode");
    EXPECT_EQ(error_code_name(ErrorCode::kBlockNotFound), "BlockNotFound");
    EXPECT_EQ(error_code_name(ErrorCode::kBlockCorrupt), "BlockCorrupt");
    EXPECT_EQ(error_code_name(ErrorCode::kReplicaNotFound), "ReplicaNotFound");
    EXPECT_EQ(error_code_name(ErrorCode::kChecksumMismatch), "ChecksumMismatch");
    EXPECT_EQ(error_code_name(ErrorCode::kBlockAlreadyCommitted), "BlockAlreadyCommitted");
    EXPECT_EQ(error_code_name(ErrorCode::kInsufficientReplicas), "InsufficientReplicas");
}

TEST(ErrorCodeTest, NameMySQLErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kMySQLError), "MySQLError");
    EXPECT_EQ(error_code_name(ErrorCode::kMySQLConnectFailed), "MySQLConnectFailed");
    EXPECT_EQ(error_code_name(ErrorCode::kMySQLQueryFailed), "MySQLQueryFailed");
    EXPECT_EQ(error_code_name(ErrorCode::kMySQLTxnFailed), "MySQLTxnFailed");
}

TEST(ErrorCodeTest, NameRPCErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kRPCError), "RPCError");
    EXPECT_EQ(error_code_name(ErrorCode::kRPCTimeout), "RPCTimeout");
    EXPECT_EQ(error_code_name(ErrorCode::kRPCConnectFailed), "RPCConnectFailed");
}

TEST(ErrorCodeTest, NameIOErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kIOError), "IOError");
    EXPECT_EQ(error_code_name(ErrorCode::kDiskFull), "DiskFull");
    EXPECT_EQ(error_code_name(ErrorCode::kFileNotFound), "FileNotFound");
}

TEST(ErrorCodeTest, NameInternalErrors) {
    EXPECT_EQ(error_code_name(ErrorCode::kInternalError), "InternalError");
    EXPECT_EQ(error_code_name(ErrorCode::kRequestDuplicated), "RequestDuplicated");
}

// ============================================================================
// error_code_name returns non-empty for all known codes
// ============================================================================

TEST(ErrorCodeTest, AllNamesNonEmpty) {
    auto codes = {
        ErrorCode::kOK,
        ErrorCode::kInvalidArgument, ErrorCode::kNotFound,
        ErrorCode::kAlreadyExists, ErrorCode::kNotDirectory,
        ErrorCode::kIsDirectory, ErrorCode::kPermissionDenied,
        ErrorCode::kDirectoryNotEmpty, ErrorCode::kPathTooLong,
        ErrorCode::kInvalidPath,
        ErrorCode::kLeaseExpired, ErrorCode::kLeaseConflict,
        ErrorCode::kFileUnderConstruction, ErrorCode::kLeaseNotFound,
        ErrorCode::kNoAvailableDataNode, ErrorCode::kBlockNotFound,
        ErrorCode::kBlockCorrupt, ErrorCode::kReplicaNotFound,
        ErrorCode::kChecksumMismatch, ErrorCode::kBlockAlreadyCommitted,
        ErrorCode::kInsufficientReplicas,
        ErrorCode::kMySQLError, ErrorCode::kMySQLConnectFailed,
        ErrorCode::kMySQLQueryFailed, ErrorCode::kMySQLTxnFailed,
        ErrorCode::kRPCError, ErrorCode::kRPCTimeout, ErrorCode::kRPCConnectFailed,
        ErrorCode::kIOError, ErrorCode::kDiskFull, ErrorCode::kFileNotFound,
        ErrorCode::kInternalError, ErrorCode::kRequestDuplicated,
    };
    for (auto code : codes) {
        EXPECT_FALSE(error_code_name(code).empty()) << "Code " << static_cast<uint16_t>(code);
        EXPECT_NE(error_code_name(code), "Unknown") << "Code " << static_cast<uint16_t>(code);
    }
}

} // namespace
} // namespace pl::minidfs
