#pragma once

#include <brpc/channel.h>
#include <memory>
#include <mutex>
#include <string>

#include "cpp/pl/minivessel/e2e/minivessel_e2e.pb.h"
#include "cpp/pl/minivessel/shared_wal.h"

namespace pl::minivessel::e2e {

class RemoteSharedWal final : public SharedWal {
public:
    RemoteSharedWal(std::string address, GroupIdentity group);

    absl::Status init();
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
    brpc::Channel channel_;
    std::unique_ptr<protocol::SharedLogService_Stub> stub_;
    GroupIdentity group_;
    std::mutex session_mutex_;
    protocol::WriterSession session_;
};

} // namespace pl::minivessel::e2e
