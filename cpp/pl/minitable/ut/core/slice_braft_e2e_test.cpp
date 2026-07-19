// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "braft/raft.h"
#include "brpc/server.h"
#include "butil/at_exit.h"
#include "butil/iobuf.h"
#include "cpp/pl/minitable/core/braft_slice_adapter.h"
#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minitable/core/slice_proposal_frontend.h"
#include "cpp/pl/minitable/core/slice_store.h"
#include "cpp/pl/minitable/proto/v2/data.pb.h"
#include "cpp/pl/sstv2/io/local_filesystem.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::minitable {
namespace {

using namespace std::chrono_literals;
using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Schema;
using sstv2::types::SchemaBuilder;
using sstv2::types::Value;

int PickUnusedPort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ntohs(address.sin_port);
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::seconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

class ProposalWaiter final {
public:
    ProposalCompletion callback() {
        return [this](absl::StatusOr<ApplyResult> result) {
            std::lock_guard lock(mutex_);
            status_ = result.status();
            if (result.ok()) {
                result_ = std::move(*result);
            }
            completed_ = true;
            condition_.notify_all();
        };
    }

    bool wait_for(std::chrono::seconds timeout = 15s) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return completed_; });
    }

    [[nodiscard]] const absl::Status& status() const { return status_; }
    [[nodiscard]] const ApplyResult& result() const { return result_; }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ = false;
    absl::Status status_ = absl::UnknownError("proposal has not completed");
    ApplyResult result_;
};

class BlockingClosure final : public ::braft::Closure {
public:
    void Run() override {
        std::lock_guard lock(mutex_);
        completed_ = true;
        condition_.notify_all();
    }

    bool wait_for(std::chrono::seconds timeout = 15s) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return completed_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ = false;
};

std::shared_ptr<const codec::CellKeyCodec> MakeCodec() {
    auto schema = SchemaBuilder().add_column("key", DataType::kString).build();
    if (!schema.has_value()) {
        return nullptr;
    }
    auto result = codec::CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                              std::make_shared<const Schema>(std::move(*schema)));
    return result.ok() ? std::make_shared<const codec::CellKeyCodec>(std::move(*result)) : nullptr;
}

proto::v2::PutRequest MakePutRequest(std::string request_id, std::string row, std::string value) {
    proto::v2::PutRequest request;
    auto* header = request.mutable_header();
    header->mutable_context()->set_client_id("real-braft-rpc");
    header->mutable_context()->set_request_id(std::move(request_id));
    header->set_table_id(1);
    header->set_slice_id(1);
    header->set_schema_version(1);
    header->set_route_epoch(1);
    request.mutable_row_key()->add_values()->set_string_value(std::move(row));
    auto* mutation = request.add_mutations();
    mutation->mutable_cell()->set_column_family_id(1);
    mutation->mutable_cell()->mutable_static_qualifier()->set_column_id(1);
    mutation->set_type(proto::v2::SET);
    mutation->mutable_value()->set_string_value(std::move(value));
    return request;
}

absl::StatusOr<std::string> MakeEntry(const codec::CellKeyCodec& codec,
                                      uint64_t request_number,
                                      std::string row,
                                      std::string value) {
    const Timestamp timestamp{.domain_epoch = 1, .counter = 100 + request_number};
    VersionedStorageKey key{
        .storage_key = {.partition = GlobalOrderPrefix{},
                        .row_key = {Value::make<DataType::kString>(row)},
                        .target = CellRef{.column_family_id = 1, .qualifier = StaticQualifier{1}}},
        .commit_ts = timestamp,
        .mutation_seq = 0,
        .op_type = OpType::kPut};
    auto encoded_key = codec.EncodeVersionedStorageKey(key);
    auto encoded_row = codec.EncodeLogicalRowKey(key.storage_key.row_key);
    if (!encoded_key.ok()) {
        return encoded_key.status();
    }
    if (!encoded_row.ok()) {
        return encoded_row.status();
    }
    CommittedSliceMutation mutation{
        .identity = {.client_id = "real-braft",
                     .request_id = "request-" + std::to_string(request_number),
                     .payload_hash = 1000 + request_number},
        .commit_ts = timestamp,
        .commit_physical_ms = 10'000 + request_number,
        .locality_group_mutations = {{{.encoded_key = std::move(*encoded_key),
                                       .encoded_value = std::move(value)}}},
        .locality_group_ids = {1},
        .serialized_response = "ok-" + std::to_string(request_number)};
    return EncodeSliceMutationV2(mutation, *encoded_row, codec);
}

class SliceBraftE2ETest : public ::testing::Test {
protected:
    struct Replica {
        ::braft::PeerId peer;
        std::filesystem::path root;
        SliceStorePersistence persistence;
        std::unique_ptr<BraftSliceAdapter> adapter;
        std::unique_ptr<::braft::Node> node;
        std::unique_ptr<SliceProposalFrontend> frontend;
    };

    void SetUp() override {
        port_ = PickUnusedPort();
        ASSERT_GT(port_, 0);
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("minitable-real-braft-" + std::to_string(port_));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        codec_ = MakeCodec();
        ASSERT_NE(codec_, nullptr);
        comparator_ = MakeComparatorDomain(1, codec_->row_key_schema_fingerprint(), 1, 0, 0);
        group_id_ = "minitable-slice-e2e-" + std::to_string(port_);

        ASSERT_EQ(::braft::add_service(&server_, port_), 0);
        ASSERT_EQ(server_.Start(port_, nullptr), 0);

        std::string configuration;
        for (int index = 0; index < 3; ++index) {
            if (!configuration.empty()) {
                configuration.push_back(',');
            }
            configuration += "127.0.0.1:" + std::to_string(port_) + ":" + std::to_string(index);
        }
        ASSERT_EQ(initial_conf_.parse_from(configuration), 0);
        replicas_.resize(3);
        for (size_t index = 0; index < replicas_.size(); ++index) {
            ASSERT_EQ(replicas_[index].peer.parse("127.0.0.1:" + std::to_string(port_) + ":" +
                                                  std::to_string(index)),
                      0);
            replicas_[index].root = root_ / ("replica-" + std::to_string(index));
            ASSERT_TRUE(StartReplica(index));
        }
        ASSERT_NE(WaitForLeader(), nullptr);
    }

    void TearDown() override {
        for (auto& replica : replicas_) {
            if (replica.node != nullptr) {
                replica.node->shutdown(nullptr);
            }
        }
        for (auto& replica : replicas_) {
            if (replica.node != nullptr) {
                replica.node->join();
                replica.frontend.reset();
                replica.node.reset();
                replica.adapter.reset();
            }
        }
        server_.Stop(0);
        server_.Join();
        std::filesystem::remove_all(root_);
    }

    bool StartReplica(size_t index) {
        auto& replica = replicas_[index];
        std::filesystem::create_directories(replica.root);
        const auto manifest_directory = replica.root / "manifest";
        std::filesystem::create_directories(manifest_directory);
        replica.persistence = {.filesystem = filesystem_,
                               .manifest_directory = manifest_directory.string(),
                               .comparator_domain = comparator_};
        auto store = SliceStore::Create({{1, {}}}, replica.persistence);
        if (!store.ok()) {
            return false;
        }
        auto machine = SliceApplyMachine::Create(std::move(*store));
        if (!machine.ok()) {
            return false;
        }
        auto state_machine = std::make_unique<SliceRaftStateMachine>(
            std::move(*machine),
            codec_,
            SliceSnapshotMetadata{.table_id = 1,
                                  .slice_id = 1,
                                  .schema_version = 1,
                                  .route_epoch = 1,
                                  .replica_set_epoch = 1,
                                  .dedupe_retention_floor = 1});
        replica.adapter =
            std::make_unique<BraftSliceAdapter>(std::move(state_machine),
                                                std::map<uint32_t, MemTableOptions>{{1, {}}},
                                                replica.persistence);
        replica.node = std::make_unique<::braft::Node>(group_id_, replica.peer);
        replica.frontend = std::make_unique<SliceProposalFrontend>(
            replica.node.get(), replica.adapter.get(), codec_);
        ::braft::NodeOptions options;
        options.initial_conf = initial_conf_;
        options.election_timeout_ms = 800;
        options.snapshot_interval_s = 0;
        options.fsm = replica.adapter.get();
        options.node_owns_fsm = false;
        const std::string prefix = "local://" + replica.root.string();
        options.log_uri = prefix + "/log";
        options.raft_meta_uri = prefix + "/raft_meta";
        options.snapshot_uri = prefix + "/snapshot";
        return replica.node->init(options) == 0;
    }

    void StopReplica(size_t index, bool erase_persistent_state) {
        auto& replica = replicas_[index];
        if (replica.node != nullptr) {
            replica.node->shutdown(nullptr);
            replica.node->join();
            replica.frontend.reset();
            replica.node.reset();
            replica.adapter.reset();
        }
        if (erase_persistent_state) {
            std::filesystem::remove_all(replica.root);
        }
    }

    Replica* WaitForLeader() {
        Replica* leader = nullptr;
        const bool elected = WaitUntil([&] {
            leader = nullptr;
            for (auto& replica : replicas_) {
                if (replica.node != nullptr && replica.node->is_leader()) {
                    if (leader != nullptr) {
                        return false;
                    }
                    leader = &replica;
                }
            }
            return leader != nullptr;
        });
        return elected ? leader : nullptr;
    }

    bool Apply(Replica& leader, const std::string& entry) {
        butil::IOBuf data;
        data.append(entry);
        BlockingClosure done;
        ::braft::Task task;
        task.data = &data;
        task.done = &done;
        leader.node->apply(task);
        return done.wait_for() && done.status().ok();
    }

    absl::StatusOr<ApplyResult> ProposePut(Replica& leader,
                                           const proto::v2::PutRequest& request,
                                           uint64_t physical_time_ms) {
        ProposalWaiter waiter;
        leader.frontend->propose_put(request, physical_time_ms, waiter.callback());
        if (!waiter.wait_for()) {
            return absl::DeadlineExceededError("proposal did not complete");
        }
        if (!waiter.status().ok()) {
            return waiter.status();
        }
        return waiter.result();
    }

    bool Flush(Replica& replica, std::string suffix) {
        auto& store = replica.adapter->state_machine().machine().store();
        if (!store.freeze_locality_group(1).ok()) {
            return false;
        }
        auto token = store.begin_flush(1);
        if (!token.ok()) {
            return false;
        }
        const auto sst = replica.root / std::move(suffix);
        auto flush = SliceStore::build_flush_sst(*token,
                                                 {.filesystem = filesystem_,
                                                  .key_path = sst.string() + ".key",
                                                  .value_path = sst.string() + ".value"});
        return flush.ok() && store.install_flush(*token, *flush).ok();
    }

    butil::AtExitManager at_exit_;
    brpc::Server server_;
    int port_ = 0;
    std::filesystem::path root_;
    std::string group_id_;
    ::braft::Configuration initial_conf_;
    std::shared_ptr<sstv2::io::LocalFileSystem> filesystem_ =
        std::make_shared<sstv2::io::LocalFileSystem>();
    std::shared_ptr<const codec::CellKeyCodec> codec_;
    ComparatorDomain comparator_;
    std::vector<Replica> replicas_;
};

TEST_F(SliceBraftE2ETest, ProposalFrontendReturnsAppliedResponseAndSurvivesLeadershipTransfer) {
    auto* leader = WaitForLeader();
    ASSERT_NE(leader, nullptr);
    ASSERT_TRUE(WaitUntil([&] { return leader->frontend->leader_term() > 0; }));

    const auto request = MakePutRequest("frontend-request", "frontend-row", "value");
    auto first = ProposePut(*leader, request, 20'000);
    ASSERT_TRUE(first.ok()) << first.status();
    EXPECT_FALSE(first->duplicate);
    proto::v2::PutResponse first_response;
    ASSERT_TRUE(first_response.ParseFromString(first->serialized_response));
    EXPECT_GT(first_response.commit_ts().counter(), 0U);

    const uint64_t before_retry = leader->frontend->allocated_timestamp_counter();
    auto retry = ProposePut(*leader, request, 20'001);
    ASSERT_TRUE(retry.ok()) << retry.status();
    EXPECT_TRUE(retry->duplicate);
    EXPECT_EQ(retry->serialized_response, first->serialized_response);
    EXPECT_EQ(leader->frontend->allocated_timestamp_counter(), before_retry);

    Replica* target = nullptr;
    for (auto& replica : replicas_) {
        if (&replica != leader) {
            target = &replica;
            break;
        }
    }
    ASSERT_NE(target, nullptr);
    ASSERT_EQ(leader->node->transfer_leadership_to(target->peer), 0);
    ASSERT_TRUE(WaitUntil(
        [&] { return target->node->is_leader() && target->frontend->leader_term() > 0; }));

    auto after_transfer =
        ProposePut(*target, MakePutRequest("after-transfer", "next-row", "next"), 20'002);
    ASSERT_TRUE(after_transfer.ok()) << after_transfer.status();
    proto::v2::PutResponse transferred_response;
    ASSERT_TRUE(transferred_response.ParseFromString(after_transfer->serialized_response));
    EXPECT_GT(transferred_response.commit_ts().counter(), first_response.commit_ts().counter());
    ASSERT_TRUE(WaitUntil([&] {
        for (const auto& replica : replicas_) {
            if (replica.adapter->state_machine().machine().store().timestamp_high_watermark() !=
                transferred_response.commit_ts().counter()) {
                return false;
            }
        }
        return true;
    }));
}

TEST_F(SliceBraftE2ETest, ReplicatesTransfersLeadershipAndTakesSnapshot) {
    auto* leader = WaitForLeader();
    ASSERT_NE(leader, nullptr);
    auto first = MakeEntry(*codec_, 1, "row-1", "value-1");
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(Apply(*leader, *first));
    ASSERT_TRUE(WaitUntil([&] {
        for (const auto& replica : replicas_) {
            if (replica.adapter->state_machine().machine().store().visible_applied_index() == 0) {
                return false;
            }
        }
        return true;
    }));

    Replica* target = nullptr;
    for (auto& replica : replicas_) {
        if (&replica != leader) {
            target = &replica;
            break;
        }
    }
    ASSERT_NE(target, nullptr);
    ASSERT_EQ(leader->node->transfer_leadership_to(target->peer), 0);
    ASSERT_TRUE(WaitUntil([&] { return target->node->is_leader(); }));

    auto second = MakeEntry(*codec_, 2, "row-2", "value-2");
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(Apply(*target, *second));
    ASSERT_TRUE(WaitUntil([&] {
        const auto expected =
            target->adapter->state_machine().machine().store().visible_applied_index();
        if (expected == 0) {
            return false;
        }
        for (const auto& replica : replicas_) {
            if (replica.adapter->state_machine().machine().store().visible_applied_index() !=
                expected) {
                return false;
            }
        }
        return true;
    }));

    for (size_t index = 0; index < replicas_.size(); ++index) {
        ASSERT_TRUE(Flush(replicas_[index], "before-snapshot-" + std::to_string(index)));
    }
    BlockingClosure snapshot_done;
    target->node->snapshot(&snapshot_done);
    ASSERT_TRUE(snapshot_done.wait_for());
    ASSERT_TRUE(snapshot_done.status().ok()) << snapshot_done.status();

    // braft keeps logs around its newest Snapshot for lagging followers. Advance the state and
    // take a second Snapshot so the first Snapshot boundary is definitely compacted.
    auto third = MakeEntry(*codec_, 3, "row-3", "value-3");
    ASSERT_TRUE(third.ok());
    ASSERT_TRUE(Apply(*target, *third));
    ASSERT_TRUE(WaitUntil([&] {
        const auto expected =
            target->adapter->state_machine().machine().store().visible_applied_index();
        for (const auto& replica : replicas_) {
            if (replica.adapter->state_machine().machine().store().visible_applied_index() !=
                expected) {
                return false;
            }
        }
        return true;
    }));
    for (size_t index = 0; index < replicas_.size(); ++index) {
        ASSERT_TRUE(Flush(replicas_[index], "compacting-snapshot-" + std::to_string(index)));
    }
    BlockingClosure compacting_snapshot_done;
    target->node->snapshot(&compacting_snapshot_done);
    ASSERT_TRUE(compacting_snapshot_done.wait_for());
    ASSERT_TRUE(compacting_snapshot_done.status().ok()) << compacting_snapshot_done.status();

    size_t repair_index = 0;
    while (&replicas_[repair_index] == target) {
        ++repair_index;
    }
    StopReplica(repair_index, true);
    ASSERT_TRUE(StartReplica(repair_index));
    const uint64_t snapshot_index =
        target->adapter->state_machine().machine().store().visible_applied_index();

    // This append probes the empty follower. The compacted prefix cannot be replayed, forcing
    // braft to install the leader Snapshot before applying this tail entry.
    auto fourth = MakeEntry(*codec_, 4, "row-4", "value-4");
    ASSERT_TRUE(fourth.ok());
    ASSERT_TRUE(Apply(*target, *fourth));
    ASSERT_TRUE(WaitUntil(
        [&] {
            return replicas_[repair_index]
                       .adapter->state_machine()
                       .machine()
                       .store()
                       .visible_applied_index() > snapshot_index;
        },
        30s));
    EXPECT_EQ(
        replicas_[repair_index].adapter->state_machine().machine().export_dedupe_records().size(),
        4U);
    ASSERT_TRUE(WaitUntil([&] {
        const auto expected =
            target->adapter->state_machine().machine().store().visible_applied_index();
        for (const auto& replica : replicas_) {
            if (replica.adapter->state_machine().machine().store().visible_applied_index() !=
                expected) {
                return false;
            }
        }
        return true;
    }));
}

} // namespace
} // namespace pl::minitable
