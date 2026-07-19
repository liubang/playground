#include "cpp/pl/minivessel/e2e/remote_shared_wal.h"

#include <brpc/controller.h>
#include <cstring>
#include <limits>
#include <utility>

#include "absl/status/status.h"

namespace pl::minivessel::e2e {
namespace proto = protocol;
namespace {

absl::Status WireStatus(const proto::RpcStatus& status) {
    if (status.code() == 0) {
        return absl::OkStatus();
    }
    if (status.code() < static_cast<int>(absl::StatusCode::kCancelled) ||
        status.code() > static_cast<int>(absl::StatusCode::kUnauthenticated)) {
        return absl::InternalError("shared-log returned an invalid status code");
    }
    return absl::Status(static_cast<absl::StatusCode>(status.code()), status.message());
}

proto::RecordType ToProto(LogRecordType type) {
    switch (type) {
        case LogRecordType::kMutation:
            return proto::MUTATION;
        case LogRecordType::kPrimaryBarrier:
            return proto::PRIMARY_BARRIER;
        case LogRecordType::kCheckpoint:
            return proto::CHECKPOINT;
    }
    return proto::RECORD_TYPE_UNSPECIFIED;
}

absl::StatusOr<LogRecordType> FromProto(proto::RecordType type) {
    switch (type) {
        case proto::MUTATION:
            return LogRecordType::kMutation;
        case proto::PRIMARY_BARRIER:
            return LogRecordType::kPrimaryBarrier;
        case proto::CHECKPOINT:
            return LogRecordType::kCheckpoint;
        default:
            return absl::DataLossError("shared-log returned an invalid record type");
    }
}

absl::Status ValidateSession(const proto::WriterSession& session,
                             std::string_view expected_owner = {},
                             AssignmentEpoch expected_assignment = {}) {
    if (session.owner().empty() || session.assignment_epoch() == 0 || session.writer_epoch() == 0 ||
        session.lease_id() == 0 || session.expires_at_ms() == 0) {
        return absl::DataLossError("shared-log returned an incomplete writer session");
    }
    if ((!expected_owner.empty() && session.owner() != expected_owner) ||
        (expected_assignment.valid() &&
         session.assignment_epoch() != expected_assignment.value())) {
        return absl::DataLossError("shared-log returned a mismatched writer session");
    }
    return absl::OkStatus();
}

absl::StatusOr<LogRecord> FromProto(const proto::Record& record) {
    auto type = FromProto(record.type());
    if (!type.ok()) {
        return type.status();
    }
    if (record.lrsn() == 0 || record.writer_epoch() == 0 || record.object_path().empty() ||
        record.identity().inode_id() == 0 || record.identity().length() == 0 ||
        record.durable_offset() == 0 ||
        (*type == LogRecordType::kMutation && record.request_id().empty())) {
        return absl::DataLossError("shared-log returned a record with invalid identity");
    }
    LogRecord output{.lrsn = Lrsn(record.lrsn()),
                     .writer_epoch = WriterEpoch(record.writer_epoch()),
                     .type = *type,
                     .request_id = record.request_id(),
                     .payload = std::vector<std::byte>(record.payload().size())};
    std::memcpy(output.payload.data(), record.payload().data(), record.payload().size());
    return output;
}

WriterLease ToLease(const proto::WriterSession& session, const GroupIdentity& group) {
    return {.group = group,
            .owner_instance_id = session.owner(),
            .assignment_epoch = AssignmentEpoch(session.assignment_epoch()),
            .writer_epoch = WriterEpoch(session.writer_epoch()),
            .lease_id = LeaseId(session.lease_id()),
            .expires_at = UnixTimeMillis(session.expires_at_ms())};
}

absl::Status RpcFailure(const brpc::Controller& controller) {
    return absl::UnavailableError(controller.ErrorText());
}

} // namespace

RemoteSharedWal::RemoteSharedWal(std::string address, GroupIdentity group)
    : group_(std::move(group)) {
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 0;
    if (channel_.Init(address.c_str(), &options) == 0) {
        stub_ = std::make_unique<proto::SharedLogService_Stub>(&channel_);
    }
}

absl::Status RemoteSharedWal::init() {
    return stub_ == nullptr ? absl::UnavailableError("failed to initialize shared-log channel")
                            : absl::OkStatus();
}

absl::StatusOr<WriterLease> RemoteSharedWal::acquire_writer(std::string_view owner,
                                                            AssignmentEpoch assignment_epoch,
                                                            uint64_t lease_timeout_ms) {
    proto::AcquireWriterRequest request;
    request.set_owner(owner);
    request.set_assignment_epoch(assignment_epoch.value());
    request.set_lease_timeout_ms(lease_timeout_ms);
    proto::WriterResponse response;
    brpc::Controller controller;
    stub_->AcquireWriter(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return RpcFailure(controller);
    }
    if (auto status = WireStatus(response.status()); !status.ok()) {
        return status;
    }
    if (auto status = ValidateSession(response.session(), owner, assignment_epoch); !status.ok()) {
        return status;
    }
    std::lock_guard lock(session_mutex_);
    session_ = response.session();
    return ToLease(session_, group_);
}

absl::StatusOr<WriterLease> RemoteSharedWal::renew_writer() {
    proto::SessionRequest request;
    {
        std::lock_guard lock(session_mutex_);
        *request.mutable_session() = session_;
    }
    proto::WriterResponse response;
    brpc::Controller controller;
    stub_->RenewWriter(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return RpcFailure(controller);
    }
    if (auto status = WireStatus(response.status()); !status.ok()) {
        return status;
    }
    if (auto status = ValidateSession(response.session(),
                                      request.session().owner(),
                                      AssignmentEpoch(request.session().assignment_epoch()));
        !status.ok() || response.session().writer_epoch() != request.session().writer_epoch() ||
        response.session().lease_id() != request.session().lease_id()) {
        return status.ok() ? absl::DataLossError("renew changed writer identity") : status;
    }
    std::lock_guard lock(session_mutex_);
    session_ = response.session();
    return ToLease(session_, group_);
}

absl::StatusOr<DurableAppend> RemoteSharedWal::append(LogRecordType type,
                                                      std::string request_id,
                                                      std::span<const std::byte> payload) {
    proto::AppendRequest request;
    {
        std::lock_guard lock(session_mutex_);
        *request.mutable_session() = session_;
    }
    request.set_type(ToProto(type));
    request.set_request_id(request_id);
    request.set_payload(payload.data(), payload.size());
    proto::AppendResponse response;
    brpc::Controller controller;
    stub_->Append(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return RpcFailure(controller);
    }
    if (auto status = WireStatus(response.status()); !status.ok()) {
        return status;
    }
    auto record = FromProto(response.record());
    if (!record.ok()) {
        return record.status();
    }
    if (record->writer_epoch.value() != request.session().writer_epoch() || record->type != type ||
        record->request_id != request_id || record->payload.size() != payload.size() ||
        !std::equal(record->payload.begin(), record->payload.end(), payload.begin())) {
        return absl::DataLossError("append response does not match the submitted record");
    }
    return DurableAppend{std::move(*record), ByteOffset(response.record().durable_offset())};
}

absl::StatusOr<std::vector<LogRecord>> RemoteSharedWal::read(Lrsn first_lrsn, size_t max_records) {
    if (!first_lrsn.valid() || max_records == 0) {
        return absl::InvalidArgumentError("read requires a valid first LRSN and non-zero limit");
    }
    proto::ReadRequest request;
    request.set_first_lrsn(first_lrsn.value());
    request.set_max_records(max_records);
    proto::ReadResponse response;
    brpc::Controller controller;
    stub_->Read(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return RpcFailure(controller);
    }
    if (auto status = WireStatus(response.status()); !status.ok()) {
        return status;
    }
    std::vector<LogRecord> records;
    records.reserve(static_cast<size_t>(response.records_size()));
    uint64_t expected = first_lrsn.value();
    uint64_t previous_offset = 0;
    for (const auto& wire_record : response.records()) {
        auto record = FromProto(wire_record);
        if (!record.ok()) {
            return record.status();
        }
        if (record->lrsn.value() != expected ||
            (previous_offset != 0 && wire_record.durable_offset() <= previous_offset)) {
            return absl::DataLossError("shared-log read is not contiguous");
        }
        records.push_back(std::move(*record));
        previous_offset = wire_record.durable_offset();
        if (expected == std::numeric_limits<uint64_t>::max()) {
            if (&wire_record != &response.records().rbegin()[0]) {
                return absl::DataLossError("shared-log read overflows LRSN");
            }
        } else {
            ++expected;
        }
    }
    return records;
}

absl::StatusOr<Lrsn> RemoteSharedWal::durable_lrsn() {
    proto::Group request;
    request.set_group_id(group_.group_id);
    request.set_incarnation(group_.incarnation.value());
    proto::StatusResponse response;
    brpc::Controller controller;
    stub_->Status(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return RpcFailure(controller);
    }
    if (auto status = WireStatus(response.status()); !status.ok()) {
        return status;
    }
    return Lrsn(response.durable_lrsn());
}

absl::Status RemoteSharedWal::release_writer() {
    proto::SessionRequest request;
    {
        std::lock_guard lock(session_mutex_);
        if (session_.lease_id() == 0) {
            return absl::OkStatus();
        }
        *request.mutable_session() = session_;
        session_.Clear();
    }
    proto::RpcStatus response;
    brpc::Controller controller;
    stub_->ReleaseWriter(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return RpcFailure(controller);
    }
    return WireStatus(response);
}

} // namespace pl::minivessel::e2e
