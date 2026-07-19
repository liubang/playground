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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minivessel/active_log_storage.h"
#include "cpp/pl/minivessel/types.h"

namespace pl::minivessel {

enum class LogRecordType : uint8_t {
    kMutation = 1,
    kPrimaryBarrier = 2,
    kCheckpoint = 3,
};

struct LogRecord final {
    Lrsn lrsn;
    WriterEpoch writer_epoch;
    LogRecordType type = LogRecordType::kMutation;
    std::string request_id;
    std::vector<std::byte> payload;
};

struct DurableAppend final {
    LogRecord record;
    ByteOffset durable_offset;
};

class SharedWal {
public:
    virtual ~SharedWal() = default;

    virtual absl::StatusOr<WriterLease> acquire_writer(std::string_view owner_instance_id,
                                                       AssignmentEpoch assignment_epoch,
                                                       uint64_t lease_timeout_ms) = 0;
    virtual absl::StatusOr<WriterLease> renew_writer() = 0;
    virtual absl::StatusOr<DurableAppend> append(LogRecordType type,
                                                 std::string request_id,
                                                 std::span<const std::byte> payload) = 0;
    virtual absl::StatusOr<std::vector<LogRecord>> read(Lrsn first_lrsn, size_t max_records) = 0;
    virtual absl::StatusOr<Lrsn> durable_lrsn() = 0;
    virtual absl::Status release_writer() = 0;
};

struct FramedSharedWalOptions final {
    GroupIdentity group;
    std::string path;
    size_t max_record_bytes = 16U * 1024U * 1024U;
};

class FramedSharedWal final : public SharedWal {
public:
    FramedSharedWal(ActiveLogStorage* storage, FramedSharedWalOptions options);
    ~FramedSharedWal() override;

    FramedSharedWal(const FramedSharedWal&) = delete;
    FramedSharedWal& operator=(const FramedSharedWal&) = delete;

    absl::StatusOr<WriterLease> acquire_writer(std::string_view owner_instance_id,
                                               AssignmentEpoch assignment_epoch,
                                               uint64_t lease_timeout_ms) override;
    absl::StatusOr<WriterLease> renew_writer() override;
    absl::StatusOr<DurableAppend> append(LogRecordType type,
                                         std::string request_id,
                                         std::span<const std::byte> payload) override;
    absl::StatusOr<std::vector<LogRecord>> read(Lrsn first_lrsn, size_t max_records) override;
    absl::StatusOr<Lrsn> durable_lrsn() override;
    absl::Status release_writer() override;

private:
    struct ScanResult final {
        std::vector<LogRecord> records;
        Lrsn last_lrsn;
        ByteOffset durable_offset;
    };

    absl::StatusOr<ScanResult> scan_durable();

    ActiveLogStorage* storage_;
    FramedSharedWalOptions options_;
    WriterSession writer_;
    std::string owner_instance_id_;
    Lrsn next_lrsn_;
};

} // namespace pl::minivessel
