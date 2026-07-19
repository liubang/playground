#include <atomic>
#include <brpc/closure_guard.h>
#include <brpc/server.h>
#include <butil/logging.h>
#include <chrono>
#include <gflags/gflags.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "cpp/pl/minivessel/e2e/counter_state_machine.h"
#include "cpp/pl/minivessel/e2e/minivessel_e2e.pb.h"
#include "cpp/pl/minivessel/e2e/remote_shared_wal.h"

DEFINE_int32(port, 9300, "counter replica brpc port");
DEFINE_string(shared_log_addr, "sharedlog:9200", "shared-log address");
DEFINE_string(replica_id, "replica-a", "replica ID");
DEFINE_uint64(assignment_epoch, 1, "assignment epoch");
DEFINE_uint64(poll_interval_ms, 200, "background poll interval");

namespace pl::minivessel::e2e {
namespace proto = protocol;
namespace {

std::string RoleName(RuntimeRole role) {
    switch (role) {
        case RuntimeRole::kStandby:
            return "standby";
        case RuntimeRole::kPromoting:
            return "promoting";
        case RuntimeRole::kPrimary:
            return "primary";
        case RuntimeRole::kFailed:
            return "failed";
        case RuntimeRole::kStopped:
            return "stopped";
    }
    return "unknown";
}

void SetStatus(proto::RpcStatus* output, const absl::Status& status) {
    output->set_code(status.ok() ? 0 : static_cast<int>(status.code()));
    output->set_message(status.ok() ? "" : status.ToString());
}

class Lifecycle final : public RoleLifecycle {
public:
    void on_role_change(RuntimeRole, const ReplicaStatus& current) override {
        std::lock_guard lock(mutex_);
        ++transitions_;
        resources_ = current.role == RuntimeRole::kPrimary;
    }
    std::pair<bool, uint64_t> snapshot() const {
        std::lock_guard lock(mutex_);
        return {resources_, transitions_};
    }

private:
    mutable std::mutex mutex_;
    bool resources_ = false;
    uint64_t transitions_ = 0;
};

class CounterService final : public proto::CounterService {
public:
    CounterService(std::string replica_id, std::string log_address, AssignmentEpoch epoch)
        : replica_id_(std::move(replica_id)),
          wal_(std::move(log_address), {.group_id = "counter", .incarnation = GroupIncarnation(1)}),
          runtime_(&wal_,
                   &counter_,
                   {.replica_id = replica_id_,
                    .assignment_epoch = epoch,
                    .writer_lease_timeout_ms = 5000,
                    .role_lifecycle = &lifecycle_}) {}

    absl::Status start() {
        if (auto status = wal_.init(); !status.ok())
            return status;
        poller_ = std::thread([this] {
            while (!stopping_.load()) {
                if (!polling_paused_.load()) {
                    (void)runtime_.poll();
                }
                CloseStaleSession();
                std::this_thread::sleep_for(std::chrono::milliseconds(FLAGS_poll_interval_ms));
            }
        });
        return absl::OkStatus();
    }
    ~CounterService() override {
        stopping_.store(true);
        if (poller_.joinable())
            poller_.join();
        std::optional<ReplicaRuntime::PrimarySession> session;
        {
            std::lock_guard lock(session_mutex_);
            session = std::move(session_);
        }
        session.reset();
        runtime_.stop();
    }

    void Status(google::protobuf::RpcController*,
                const proto::Empty*,
                proto::CounterStatus* response,
                google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        FillStatus(response, absl::OkStatus());
    }
    void Promote(google::protobuf::RpcController*,
                 const proto::Empty*,
                 proto::CounterStatus* response,
                 google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        absl::Status status;
        {
            std::lock_guard lock(session_mutex_);
            if (session_.has_value()) {
                status = absl::AlreadyExistsError("primary session already active");
            } else {
                auto promoted = runtime_.promote_session();
                if (!promoted.ok())
                    status = promoted.status();
                else
                    session_.emplace(std::move(*promoted));
            }
        }
        FillStatus(response, status);
    }
    void Demote(google::protobuf::RpcController*,
                const proto::Empty*,
                proto::CounterStatus* response,
                google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        absl::Status status;
        {
            std::lock_guard lock(session_mutex_);
            if (!session_.has_value())
                status = absl::FailedPreconditionError("not primary");
            else {
                status = session_->demote();
                session_.reset();
            }
        }
        FillStatus(response, status);
    }
    void Add(google::protobuf::RpcController*,
             const proto::AddRequest* request,
             proto::CommitResponse* response,
             google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        absl::StatusOr<CommitResult> committed = absl::FailedPreconditionError("not primary");
        std::optional<ReplicaRuntime::PrimarySession> stale;
        {
            std::lock_guard lock(session_mutex_);
            if (session_.has_value()) {
                committed =
                    session_->submit(request->request_id(), EncodeCounterValue(request->delta()));
                // submit() only returns a system/admission error after the runtime has closed
                // primary admission. Deterministic business rejection is a successful CommitResult.
                if (!committed.ok()) {
                    stale = std::move(session_);
                }
            }
        }
        stale.reset();
        if (!committed.ok()) {
            SetStatus(response->mutable_status(), committed.status());
            return;
        }
        if (committed->apply_result.rejected()) {
            SetStatus(response->mutable_status(), committed->apply_result.rejection());
            return;
        }
        SetStatus(response->mutable_status(), absl::OkStatus());
        response->set_lrsn(committed->lrsn.value());
    }
    void Checkpoint(google::protobuf::RpcController*,
                    const proto::Empty*,
                    proto::CommitResponse* response,
                    google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        absl::StatusOr<CommitResult> committed = absl::FailedPreconditionError("not primary");
        {
            std::lock_guard lock(session_mutex_);
            if (session_.has_value()) {
                committed = session_->checkpoint();
            }
        }
        if (!committed.ok()) {
            if (runtime_.status().role != RuntimeRole::kPrimary) {
                CloseStaleSession();
            }
            SetStatus(response->mutable_status(), committed.status());
            return;
        }
        SetStatus(response->mutable_status(), absl::OkStatus());
        response->set_lrsn(committed->lrsn.value());
    }
    void Poll(google::protobuf::RpcController*,
              const proto::Empty*,
              proto::CounterStatus* response,
              google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        FillStatus(response, runtime_.poll());
    }
    void PausePolling(google::protobuf::RpcController*,
                      const proto::Empty*,
                      proto::CounterStatus* response,
                      google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        polling_paused_.store(true);
        FillStatus(response, absl::OkStatus());
    }
    void ResumePolling(google::protobuf::RpcController*,
                       const proto::Empty*,
                       proto::CounterStatus* response,
                       google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        polling_paused_.store(false);
        FillStatus(response, absl::OkStatus());
    }
    void Stop(google::protobuf::RpcController*,
              const proto::Empty*,
              proto::CounterStatus* response,
              google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        stopping_.store(true);
        std::optional<ReplicaRuntime::PrimarySession> session;
        {
            std::lock_guard lock(session_mutex_);
            session = std::move(session_);
        }
        session.reset();
        runtime_.stop();
        FillStatus(response, absl::OkStatus());
    }

private:
    void CloseStaleSession() {
        if (runtime_.status().role == RuntimeRole::kPrimary) {
            return;
        }
        std::optional<ReplicaRuntime::PrimarySession> stale;
        {
            std::lock_guard lock(session_mutex_);
            stale = std::move(session_);
        }
        stale.reset();
    }

    void FillStatus(proto::CounterStatus* response, const absl::Status& operation) {
        auto guard = runtime_.read();
        const auto& status = guard.status();
        const auto& counter = static_cast<const CounterStateMachine&>(guard.state_machine());
        const auto [resources, transitions] = lifecycle_.snapshot();
        SetStatus(response->mutable_status(), operation);
        response->set_replica_id(replica_id_);
        response->set_role(RoleName(status.role));
        response->set_value(counter.value());
        response->set_applied_lrsn(status.applied_lrsn.value());
        response->set_writer_epoch(status.writer_epoch.value());
        response->set_primary_resources_active(resources);
        response->set_lifecycle_transitions(transitions);
        response->set_last_error(status.last_error.ok() ? "" : status.last_error.ToString());
    }

    std::string replica_id_;
    RemoteSharedWal wal_;
    CounterStateMachine counter_;
    Lifecycle lifecycle_;
    ReplicaRuntime runtime_;
    std::mutex session_mutex_;
    std::optional<ReplicaRuntime::PrimarySession> session_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> polling_paused_{false};
    std::thread poller_;
};

} // namespace
} // namespace pl::minivessel::e2e

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    pl::minivessel::e2e::CounterService service(
        FLAGS_replica_id,
        FLAGS_shared_log_addr,
        pl::minivessel::AssignmentEpoch(FLAGS_assignment_epoch));
    if (auto status = service.start(); !status.ok()) {
        LOG(ERROR) << status;
        return 1;
    }
    brpc::Server server;
    if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0 ||
        server.Start(FLAGS_port, nullptr) != 0) {
        LOG(ERROR) << "failed to start counter replica";
        return 1;
    }
    server.RunUntilAskedToQuit();
    return 0;
}
