// Copyright (c) 2024 liubang. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cpp/pl/braft/proto/counter.pb.h"
#include <braft/protobuf_file.h>
#include <braft/raft.h>
#include <braft/storage.h>
#include <braft/util.h>
#include <brpc/controller.h>
#include <brpc/server.h>
#include <gflags/gflags.h>

DEFINE_bool(check_term, true, "Check if the leader changed to another term");
DEFINE_bool(disable_cli, false, "Don't allow raft_cli access this node");
DEFINE_bool(log_applied_task, false, "Print notice log when a task is applied");
DEFINE_int32(election_timeout_ms,
             5000,
             "Start election in such milliseconds if disconnect with the leader");
DEFINE_int32(port, 8100, "Listen port of this peer");
DEFINE_int32(snapshot_interval, 30, "Interval between each snapshot");
DEFINE_string(conf, "", "Initial configuration of the replication group");
DEFINE_string(data_path, "./data", "Path of data stored on");
DEFINE_string(group, "Counter", "Id of the replication group");

namespace pl::braft_example {

// NOTE: proto package "pl.braft.proto" generates C++ namespace pl::braft::proto,
// which shadows the global ::braft namespace inside namespace pl.
// Therefore all references to the braft library MUST use the ::braft:: prefix.

class Counter;

// Closure that encloses RPC context for the fetch_add operation.
class FetchAddClosure : public ::braft::Closure {
public:
    FetchAddClosure(Counter* counter,
                    const ::pl::braft::proto::FetchAddRequest* request,
                    ::pl::braft::proto::CounterResponse* response,
                    google::protobuf::Closure* done)
        : counter_(counter), request_(request), response_(response), done_(done) {}

    const ::pl::braft::proto::FetchAddRequest* request() const { return request_; }
    ::pl::braft::proto::CounterResponse* response() const { return response_; }
    void Run() override;

private:
    Counter* counter_;
    const ::pl::braft::proto::FetchAddRequest* request_;
    ::pl::braft::proto::CounterResponse* response_;
    google::protobuf::Closure* done_;
};

// A distributed counter implemented as a braft::StateMachine.
// The counter supports two operations: fetch_add (atomic increment)
// and get (read current value). All writes go through Raft consensus
// to ensure consistency across replicas.
class Counter : public ::braft::StateMachine {
public:
    Counter() : node_(nullptr), value_(0), leader_term_(-1) {}

    ~Counter() override { delete node_; }

    // Start this Raft node and join the replication group.
    int start() {
        butil::EndPoint addr(butil::my_ip(), FLAGS_port);
        ::braft::NodeOptions node_options;
        if (node_options.initial_conf.parse_from(FLAGS_conf) != 0) {
            LOG(ERROR) << "Fail to parse configuration `" << FLAGS_conf << '\'';
            return -1;
        }
        node_options.election_timeout_ms = FLAGS_election_timeout_ms;
        node_options.fsm = this;
        node_options.node_owns_fsm = false;
        node_options.snapshot_interval_s = FLAGS_snapshot_interval;
        std::string prefix = "local://" + FLAGS_data_path;
        node_options.log_uri = prefix + "/log";
        node_options.raft_meta_uri = prefix + "/raft_meta";
        node_options.snapshot_uri = prefix + "/snapshot";
        node_options.disable_cli = FLAGS_disable_cli;
        auto* node = new ::braft::Node(FLAGS_group, ::braft::PeerId(addr));
        if (node->init(node_options) != 0) {
            LOG(ERROR) << "Fail to init raft node";
            delete node;
            return -1;
        }
        node_ = node;
        return 0;
    }

    // Handle fetch_add RPC: serialize the request and apply it through Raft.
    void fetch_add(const ::pl::braft::proto::FetchAddRequest* request,
                   ::pl::braft::proto::CounterResponse* response,
                   google::protobuf::Closure* done) {
        ::brpc::ClosureGuard done_guard(done);
        const int64_t term = leader_term_.load(butil::memory_order_relaxed);
        if (term < 0) {
            return redirect(response);
        }
        butil::IOBuf log;
        butil::IOBufAsZeroCopyOutputStream wrapper(&log);
        if (!request->SerializeToZeroCopyStream(&wrapper)) {
            LOG(ERROR) << "Fail to serialize request";
            response->set_success(false);
            return;
        }
        ::braft::Task task;
        task.data = &log;
        task.done = new FetchAddClosure(this, request, response, done_guard.release());
        if (FLAGS_check_term) {
            task.expected_term = term;
        }
        return node_->apply(task);
    }

    // Handle get RPC: return the current counter value (leader only).
    void get(::pl::braft::proto::CounterResponse* response) {
        if (!is_leader()) {
            return redirect(response);
        }
        response->set_success(true);
        response->set_value(value_.load(butil::memory_order_relaxed));
    }

    bool is_leader() const { return leader_term_.load(butil::memory_order_acquire) > 0; }

    void shutdown() {
        if (node_) {
            node_->shutdown(nullptr);
        }
    }

    void join() {
        if (node_) {
            node_->join();
        }
    }

private:
    friend class FetchAddClosure;

    void redirect(::pl::braft::proto::CounterResponse* response) {
        response->set_success(false);
        if (node_) {
            ::braft::PeerId leader = node_->leader_id();
            if (!leader.is_empty()) {
                response->set_redirect(leader.to_string());
            }
        }
    }

    // Apply committed log entries to the state machine.
    void on_apply(::braft::Iterator& iter) override {
        for (; iter.valid(); iter.next()) {
            int64_t delta_value = 0;
            ::pl::braft::proto::CounterResponse* response = nullptr;
            ::braft::AsyncClosureGuard closure_guard(iter.done());
            if (iter.done()) {
                // Applied by this node — get value directly from the closure.
                auto* c = dynamic_cast<FetchAddClosure*>(iter.done());
                response = c->response();
                delta_value = c->request()->value();
            } else {
                // Replicated from leader — parse the log entry.
                butil::IOBufAsZeroCopyInputStream wrapper(iter.data());
                ::pl::braft::proto::FetchAddRequest request;
                CHECK(request.ParseFromZeroCopyStream(&wrapper));
                delta_value = request.value();
            }
            const int64_t prev = value_.fetch_add(delta_value, butil::memory_order_relaxed);
            if (response) {
                response->set_success(true);
                response->set_value(prev);
            }
            LOG_IF(INFO, FLAGS_log_applied_task)
                << "Added value=" << prev << " by delta=" << delta_value
                << " at log_index=" << iter.index();
        }
    }

    struct SnapshotArg {
        int64_t value;
        ::braft::SnapshotWriter* writer;
        ::braft::Closure* done;
    };

    static void* save_snapshot(void* arg) {
        auto* sa = static_cast<SnapshotArg*>(arg);
        std::unique_ptr<SnapshotArg> arg_guard(sa);
        ::brpc::ClosureGuard done_guard(sa->done);
        std::string snapshot_path = sa->writer->get_path() + "/data";
        LOG(INFO) << "Saving snapshot to " << snapshot_path;
        ::pl::braft::proto::Snapshot s;
        s.set_value(sa->value);
        ::braft::ProtoBufFile pb_file(snapshot_path);
        if (pb_file.save(&s, true) != 0) {
            sa->done->status().set_error(EIO, "Fail to save pb_file");
            return nullptr;
        }
        if (sa->writer->add_file("data") != 0) {
            sa->done->status().set_error(EIO, "Fail to add file to writer");
            return nullptr;
        }
        return nullptr;
    }

    void on_snapshot_save(::braft::SnapshotWriter* writer, ::braft::Closure* done) override {
        auto* arg = new SnapshotArg;
        arg->value = value_.load(butil::memory_order_relaxed);
        arg->writer = writer;
        arg->done = done;
        bthread_t tid;
        bthread_start_urgent(&tid, nullptr, save_snapshot, arg);
    }

    int on_snapshot_load(::braft::SnapshotReader* reader) override {
        CHECK(!is_leader()) << "Leader is not supposed to load snapshot";
        if (reader->get_file_meta("data", nullptr) != 0) {
            LOG(ERROR) << "Fail to find `data' on " << reader->get_path();
            return -1;
        }
        std::string snapshot_path = reader->get_path() + "/data";
        ::braft::ProtoBufFile pb_file(snapshot_path);
        ::pl::braft::proto::Snapshot s;
        if (pb_file.load(&s) != 0) {
            LOG(ERROR) << "Fail to load snapshot from " << snapshot_path;
            return -1;
        }
        value_.store(s.value(), butil::memory_order_relaxed);
        return 0;
    }

    void on_leader_start(int64_t term) override {
        leader_term_.store(term, butil::memory_order_release);
        LOG(INFO) << "Node becomes leader";
    }

    void on_leader_stop(const butil::Status& status) override {
        leader_term_.store(-1, butil::memory_order_release);
        LOG(INFO) << "Node stepped down : " << status;
    }

    void on_shutdown() override { LOG(INFO) << "This node is down"; }

    void on_error(const ::braft::Error& e) override { LOG(ERROR) << "Met raft error " << e; }

    void on_configuration_committed(const ::braft::Configuration& conf) override {
        LOG(INFO) << "Configuration of this group is " << conf;
    }

    void on_stop_following(const ::braft::LeaderChangeContext& ctx) override {
        LOG(INFO) << "Node stops following " << ctx;
    }

    void on_start_following(const ::braft::LeaderChangeContext& ctx) override {
        LOG(INFO) << "Node start following " << ctx;
    }

private:
    ::braft::Node* volatile node_;
    butil::atomic<int64_t> value_;
    butil::atomic<int64_t> leader_term_;
};

void FetchAddClosure::Run() {
    std::unique_ptr<FetchAddClosure> self_guard(this);
    ::brpc::ClosureGuard done_guard(done_);
    if (status().ok()) {
        return;
    }
    counter_->redirect(response_);
}

// brpc service implementation that delegates to the Counter state machine.
class CounterServiceImpl : public ::pl::braft::proto::CounterService {
public:
    explicit CounterServiceImpl(Counter* counter) : counter_(counter) {}

    void fetch_add(::google::protobuf::RpcController* /*controller*/,
                   const ::pl::braft::proto::FetchAddRequest* request,
                   ::pl::braft::proto::CounterResponse* response,
                   ::google::protobuf::Closure* done) override {
        return counter_->fetch_add(request, response, done);
    }

    void get(::google::protobuf::RpcController* /*controller*/,
             const ::pl::braft::proto::GetRequest* /*request*/,
             ::pl::braft::proto::CounterResponse* response,
             ::google::protobuf::Closure* done) override {
        ::brpc::ClosureGuard done_guard(done);
        return counter_->get(response);
    }

private:
    Counter* counter_;
};

} // namespace pl::braft_example

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    butil::AtExitManager exit_manager;

    ::brpc::Server server;
    pl::braft_example::Counter counter;
    pl::braft_example::CounterServiceImpl service(&counter);

    if (server.AddService(&service, ::brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }
    if (::braft::add_service(&server, FLAGS_port) != 0) {
        LOG(ERROR) << "Fail to add raft service";
        return -1;
    }
    if (server.Start(FLAGS_port, nullptr) != 0) {
        LOG(ERROR) << "Fail to start Server";
        return -1;
    }
    if (counter.start() != 0) {
        LOG(ERROR) << "Fail to start Counter";
        return -1;
    }

    LOG(INFO) << "Counter service is running on " << server.listen_address();
    while (!::brpc::IsAskedToQuit()) {
        sleep(1);
    }

    LOG(INFO) << "Counter service is going to quit";
    counter.shutdown();
    server.Stop(0);
    counter.join();
    server.Join();
    return 0;
}
