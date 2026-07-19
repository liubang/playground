#include <brpc/closure_guard.h>
#include <brpc/server.h>
#include <butil/logging.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <gflags/gflags.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "cpp/pl/minivessel/e2e/minivessel_e2e.pb.h"
#include "cpp/pl/minivessel/e2e/object_layout.h"
#include "cpp/pl/minivessel/minidfs_object_store.h"

DEFINE_int32(port, 9200, "shared-log brpc port");
DEFINE_string(namenode_addr, "namenode:9000", "MiniDFS NameNode address");
DEFINE_string(group_id, "counter", "Vessel group ID");
DEFINE_uint64(incarnation, 1, "Vessel group incarnation");
DEFINE_uint32(replication, 3, "MiniDFS object replication");

namespace pl::minivessel::e2e {
namespace proto = ::pl::minivessel::e2e::protocol;
namespace {

constexpr size_t kMaxRequestIdBytes = 4096;
constexpr size_t kMaxPayloadBytes = 16U * 1024U * 1024U;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

void SetStatus(proto::RpcStatus* output, int code, std::string message) {
    output->set_code(code);
    output->set_message(std::move(message));
}
void Ok(proto::RpcStatus* output) {
    SetStatus(output, 0, {});
}
void Error(proto::RpcStatus* output, absl::StatusCode code, std::string message) {
    SetStatus(output, static_cast<int>(code), std::move(message));
}

LogRecordType FromProto(proto::RecordType type) {
    switch (type) {
        case proto::MUTATION:
            return LogRecordType::kMutation;
        case proto::PRIMARY_BARRIER:
            return LogRecordType::kPrimaryBarrier;
        case proto::CHECKPOINT:
            return LogRecordType::kCheckpoint;
        default:
            return static_cast<LogRecordType>(0);
    }
}

class SharedLogService final : public proto::SharedLogService {
public:
    SharedLogService(std::unique_ptr<MiniDfsObjectStore> dfs, GroupIdentity group)
        : dfs_(std::move(dfs)), group_(std::move(group)) {}

    void AcquireWriter(google::protobuf::RpcController*,
                       const proto::AcquireWriterRequest* request,
                       proto::WriterResponse* response,
                       google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        std::lock_guard lock(mutex_);
        if (request->owner().empty() || request->assignment_epoch() == 0 ||
            request->lease_timeout_ms() == 0) {
            Error(response->mutable_status(),
                  absl::StatusCode::kInvalidArgument,
                  "owner, assignment_epoch and lease_timeout_ms are required");
            return;
        }
        if (request->assignment_epoch() < last_assignment_epoch_) {
            Error(response->mutable_status(),
                  absl::StatusCode::kAborted,
                  "assignment epoch is older than the authority fence");
            return;
        }
        if (request->assignment_epoch() == last_assignment_epoch_ &&
            !last_assignment_owner_.empty() && request->owner() != last_assignment_owner_) {
            Error(response->mutable_status(),
                  absl::StatusCode::kAborted,
                  "assignment epoch is already bound to another owner");
            return;
        }
        if (lease_.lease_id() != 0 && lease_.expires_at_ms() > NowMs()) {
            Error(response->mutable_status(),
                  absl::StatusCode::kAlreadyExists,
                  "an unexpired writer already owns the group");
            return;
        }
        const uint64_t now = NowMs();
        if (last_epoch_ == std::numeric_limits<uint64_t>::max() ||
            last_lease_id_ == std::numeric_limits<uint64_t>::max() ||
            request->lease_timeout_ms() > std::numeric_limits<uint64_t>::max() - now) {
            Error(response->mutable_status(),
                  absl::StatusCode::kOutOfRange,
                  "writer epoch, lease ID or expiry time exhausted");
            return;
        }
        ++last_epoch_;
        ++last_lease_id_;
        last_assignment_epoch_ = request->assignment_epoch();
        last_assignment_owner_ = request->owner();
        lease_.set_owner(request->owner());
        lease_.set_assignment_epoch(request->assignment_epoch());
        lease_.set_writer_epoch(last_epoch_);
        lease_.set_lease_id(last_lease_id_);
        lease_.set_expires_at_ms(now + request->lease_timeout_ms());
        lease_timeout_ms_ = request->lease_timeout_ms();
        Ok(response->mutable_status());
        *response->mutable_session() = lease_;
    }

    void RenewWriter(google::protobuf::RpcController*,
                     const proto::SessionRequest* request,
                     proto::WriterResponse* response,
                     google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        std::lock_guard lock(mutex_);
        if (!ValidateSession(request->session(), response->mutable_status())) {
            return;
        }
        lease_.set_expires_at_ms(NowMs() + lease_timeout_ms_);
        Ok(response->mutable_status());
        *response->mutable_session() = lease_;
    }

    void ReleaseWriter(google::protobuf::RpcController*,
                       const proto::SessionRequest* request,
                       proto::RpcStatus* response,
                       google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        std::lock_guard lock(mutex_);
        if (!ValidateSession(request->session(), response)) {
            return;
        }
        lease_.Clear();
        Ok(response);
    }

    void Append(google::protobuf::RpcController*,
                const proto::AppendRequest* request,
                proto::AppendResponse* response,
                google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        std::lock_guard lock(mutex_);
        if (!ValidateSession(request->session(), response->mutable_status())) {
            return;
        }
        const LogRecordType type = FromProto(request->type());
        if (static_cast<uint8_t>(type) == 0) {
            Error(response->mutable_status(),
                  absl::StatusCode::kInvalidArgument,
                  "unknown record type");
            return;
        }
        if ((type == LogRecordType::kMutation && request->request_id().empty()) ||
            request->request_id().size() > kMaxRequestIdBytes ||
            request->payload().size() > kMaxPayloadBytes) {
            Error(response->mutable_status(),
                  absl::StatusCode::kInvalidArgument,
                  "mutation request ID is required and request limits must be respected");
            return;
        }
        if (catalog_.size() >= std::numeric_limits<uint64_t>::max()) {
            Error(response->mutable_status(), absl::StatusCode::kOutOfRange, "LRSN exhausted");
            return;
        }
        const Lrsn lrsn(static_cast<uint64_t>(catalog_.size()) + 1);
        auto path = RecordObjectPath(
            group_, lrsn, WriterEpoch(lease_.writer_epoch()), type, request->request_id());
        if (!path.ok()) {
            Error(response->mutable_status(),
                  path.status().code(),
                  std::string(path.status().message()));
            return;
        }
        proto::Record stored;
        stored.set_lrsn(lrsn.value());
        stored.set_writer_epoch(lease_.writer_epoch());
        stored.set_type(request->type());
        stored.set_request_id(request->request_id());
        stored.set_payload(request->payload());
        std::string bytes;
        if (!stored.SerializeToString(&bytes)) {
            Error(response->mutable_status(),
                  absl::StatusCode::kInternal,
                  "failed to serialize immutable record");
            return;
        }
        auto published = dfs_->put_immutable(*path, bytes);
        if (!published.ok()) {
            Error(response->mutable_status(),
                  published.status().code(),
                  std::string(published.status().message()));
            return;
        }
        if (!ValidateSession(request->session(), response->mutable_status())) {
            // Immutable close may have succeeded after lease expiry. Such an object is an orphan
            // and is deliberately not published into the authority catalog.
            return;
        }
        stored.set_object_path(*path);
        const auto& identity = *published;
        auto* wire_identity = stored.mutable_identity();
        wire_identity->set_inode_id(identity.inode_id);
        wire_identity->set_content_generation(identity.content_generation);
        wire_identity->set_length(identity.length);
        wire_identity->set_checksum(identity.checksum);
        wire_identity->set_checksum_valid(identity.checksum_valid);
        if (identity.length > std::numeric_limits<uint64_t>::max() - durable_offset_) {
            Error(response->mutable_status(),
                  absl::StatusCode::kOutOfRange,
                  "durable byte offset exhausted");
            return;
        }
        stored.set_durable_offset(durable_offset_ + identity.length);
        if (stored.lrsn() != catalog_.size() + 1 ||
            (!catalog_.empty() && catalog_.back().lrsn() + 1 != stored.lrsn())) {
            Error(response->mutable_status(),
                  absl::StatusCode::kDataLoss,
                  "authority catalog insertion is not contiguous");
            return;
        }
        // Publication is this final contiguous catalog insertion, strictly after immutable close.
        durable_offset_ = stored.durable_offset();
        catalog_.push_back(stored);
        Ok(response->mutable_status());
        *response->mutable_record() = stored;
    }

    void Read(google::protobuf::RpcController*,
              const proto::ReadRequest* request,
              proto::ReadResponse* response,
              google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        std::lock_guard lock(mutex_);
        if (request->first_lrsn() == 0 || request->max_records() == 0) {
            Error(response->mutable_status(),
                  absl::StatusCode::kInvalidArgument,
                  "first_lrsn and max_records must be non-zero");
            return;
        }
        if (request->first_lrsn() - 1 > std::numeric_limits<size_t>::max() ||
            request->max_records() > std::numeric_limits<size_t>::max()) {
            Error(response->mutable_status(),
                  absl::StatusCode::kOutOfRange,
                  "read range exceeds authority address space");
            return;
        }
        const size_t begin = static_cast<size_t>(request->first_lrsn() - 1);
        const size_t limit = static_cast<size_t>(request->max_records());
        for (size_t i = begin;
             i < catalog_.size() && static_cast<size_t>(response->records_size()) < limit;
             ++i) {
            const auto& entry = catalog_[i];
            if (entry.lrsn() != static_cast<uint64_t>(i) + 1 || entry.writer_epoch() == 0 ||
                entry.durable_offset() == 0 ||
                (i != 0 && entry.durable_offset() <= catalog_[i - 1].durable_offset())) {
                Error(response->mutable_status(),
                      absl::StatusCode::kDataLoss,
                      "authority catalog is not contiguous");
                response->clear_records();
                return;
            }
            ImmutableObjectIdentity identity{entry.identity().inode_id(),
                                             entry.identity().content_generation(),
                                             entry.identity().length(),
                                             entry.identity().checksum(),
                                             entry.identity().checksum_valid()};
            auto bytes = dfs_->read(entry.object_path(), identity);
            if (!bytes.ok()) {
                Error(response->mutable_status(),
                      absl::StatusCode::kDataLoss,
                      "identity-bound MiniDFS read failed: " + bytes.status().ToString());
                response->clear_records();
                return;
            }
            proto::Record decoded;
            if (!decoded.ParseFromString(*bytes) || decoded.lrsn() != entry.lrsn() ||
                decoded.writer_epoch() != entry.writer_epoch() || decoded.type() != entry.type() ||
                decoded.request_id() != entry.request_id() ||
                decoded.payload() != entry.payload()) {
                Error(response->mutable_status(),
                      absl::StatusCode::kDataLoss,
                      "immutable object content does not match authority catalog");
                response->clear_records();
                return;
            }
            decoded.set_object_path(entry.object_path());
            *decoded.mutable_identity() = entry.identity();
            decoded.set_durable_offset(entry.durable_offset());
            *response->add_records() = std::move(decoded);
        }
        Ok(response->mutable_status());
    }

    void Status(google::protobuf::RpcController*,
                const proto::Group*,
                proto::StatusResponse* response,
                google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        std::lock_guard lock(mutex_);
        Ok(response->mutable_status());
        response->set_durable_lrsn(catalog_.size());
        response->set_writer_epoch(last_epoch_);
        response->set_writer_owner(lease_.owner());
        response->set_object_count(catalog_.size());
    }

private:
    bool ValidateSession(const proto::WriterSession& candidate, proto::RpcStatus* status) {
        if (lease_.lease_id() == 0 || lease_.expires_at_ms() <= NowMs()) {
            Error(status, absl::StatusCode::kDeadlineExceeded, "writer lease expired");
            return false;
        }
        if (candidate.owner() != lease_.owner() || candidate.lease_id() != lease_.lease_id() ||
            candidate.writer_epoch() != lease_.writer_epoch() ||
            candidate.assignment_epoch() != lease_.assignment_epoch()) {
            Error(status, absl::StatusCode::kAborted, "writer owner/token/epoch is fenced");
            return false;
        }
        return true;
    }

    std::unique_ptr<MiniDfsObjectStore> dfs_;
    GroupIdentity group_;
    std::mutex mutex_;
    proto::WriterSession lease_;
    uint64_t lease_timeout_ms_ = 0;
    uint64_t last_epoch_ = 0;
    uint64_t last_lease_id_ = 0;
    uint64_t last_assignment_epoch_ = 0;
    std::string last_assignment_owner_;
    uint64_t durable_offset_ = 0;
    std::vector<proto::Record> catalog_;
};

} // namespace
} // namespace pl::minivessel::e2e

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    auto dfs = pl::minivessel::MiniDfsObjectStore::Create(
        FLAGS_namenode_addr, "minivessel-shared-log", FLAGS_replication);
    if (!dfs.ok()) {
        LOG(ERROR) << dfs.status();
        return 1;
    }
    pl::minivessel::e2e::SharedLogService service(
        std::move(*dfs),
        {.group_id = FLAGS_group_id,
         .incarnation = pl::minivessel::GroupIncarnation(FLAGS_incarnation)});
    brpc::Server server;
    if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0 ||
        server.Start(FLAGS_port, nullptr) != 0) {
        LOG(ERROR) << "failed to start shared-log service";
        return 1;
    }
    server.RunUntilAskedToQuit();
    return 0;
}
