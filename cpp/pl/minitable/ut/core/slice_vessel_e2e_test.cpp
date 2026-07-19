// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>

#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minitable/core/slice_store.h"
#include "cpp/pl/minitable/core/vessel_slice_replica.h"
#include "cpp/pl/minivessel/local_filesystem.h"
#include "cpp/pl/minivessel/shared_wal.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {
namespace {

using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Schema;
using sstv2::types::SchemaBuilder;
using sstv2::types::Value;

std::shared_ptr<const codec::CellKeyCodec> MakeCodec() {
    auto schema = SchemaBuilder().add_column("key", DataType::kString).build();
    if (!schema.has_value()) {
        return nullptr;
    }
    auto codec = codec::CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                             std::make_shared<const Schema>(std::move(*schema)));
    return codec.ok() ? std::make_shared<const codec::CellKeyCodec>(std::move(*codec)) : nullptr;
}

std::unique_ptr<VesselSliceReplica> MakeReplica(
    minivessel::SharedWal* wal,
    const std::shared_ptr<const codec::CellKeyCodec>& codec,
    std::string replica_id,
    uint64_t assignment_epoch) {
    auto store = SliceStore::Create({{1, {}}});
    if (!store.ok()) {
        return nullptr;
    }
    auto machine = SliceApplyMachine::Create(std::move(*store));
    if (!machine.ok()) {
        return nullptr;
    }
    auto state_machine =
        std::make_unique<VesselSliceStateMachine>(std::move(*machine), codec);
    auto replica = VesselSliceReplica::Create(
        wal,
        std::move(state_machine),
        minivessel::ReplicaRuntimeOptions{
            .replica_id = std::move(replica_id),
            .assignment_epoch = minivessel::AssignmentEpoch(assignment_epoch)});
    return replica.ok() ? std::move(*replica) : nullptr;
}

absl::StatusOr<std::string> MakeEntry(const codec::CellKeyCodec& codec,
                                      uint64_t sequence,
                                      std::string row,
                                      std::string value) {
    const Timestamp timestamp{.domain_epoch = 1, .counter = 100 + sequence};
    VersionedStorageKey key{
        .storage_key = {.partition = GlobalOrderPrefix{},
                        .row_key = {Value::make<DataType::kString>(row)},
                        .target = CellRef{.column_family_id = 1,
                                          .qualifier = StaticQualifier{1}}},
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
        .identity = {.client_id = "vessel-client",
                     .request_id = "request-" + std::to_string(sequence),
                     .payload_hash = 1000 + sequence},
        .commit_ts = timestamp,
        .commit_physical_ms = 10'000 + sequence,
        .locality_group_mutations = {{{.encoded_key = std::move(*encoded_key),
                                       .encoded_value = std::move(value)}}},
        .locality_group_ids = {1},
        .serialized_response = "ok-" + std::to_string(sequence)};
    return EncodeSliceMutationV2(mutation, *encoded_row, codec);
}

TEST(SliceVesselE2ETest, ThreeReplicasConvergeAndFailOverWithWriterFencing) {
    const auto root = std::filesystem::path(::testing::TempDir()) /
                      ("minitable-vessel-" + std::to_string(::testing::UnitTest::GetInstance()
                                                               ->random_seed()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    minivessel::LocalFileSystem filesystem;
    const minivessel::FramedSharedWalOptions wal_options{
        .group = {.group_id = "slice-1", .incarnation = minivessel::GroupIncarnation(1)},
        .path = (root / "active.wal").string()};
    minivessel::FramedSharedWal wal_a(&filesystem, wal_options);
    minivessel::FramedSharedWal wal_b(&filesystem, wal_options);
    minivessel::FramedSharedWal wal_c(&filesystem, wal_options);
    const auto codec = MakeCodec();
    ASSERT_NE(codec, nullptr);
    auto replica_a = MakeReplica(&wal_a, codec, "replica-a", 1);
    auto replica_b = MakeReplica(&wal_b, codec, "replica-b", 2);
    auto replica_c = MakeReplica(&wal_c, codec, "replica-c", 3);
    ASSERT_NE(replica_a, nullptr);
    ASSERT_NE(replica_b, nullptr);
    ASSERT_NE(replica_c, nullptr);

    ASSERT_TRUE(replica_a->promote().ok());
    auto first = MakeEntry(*codec, 1, "row-a", "value-a");
    ASSERT_TRUE(first.ok()) << first.status();
    auto committed = replica_a->submit(
        "request-1", std::as_bytes(std::span(first->data(), first->size())));
    ASSERT_TRUE(committed.ok()) << committed.status();
    EXPECT_EQ(committed->lrsn, minivessel::Lrsn(2));
    EXPECT_EQ(replica_b->submit("not-primary", std::as_bytes(std::span(first->data(), first->size())))
                  .status()
                  .code(),
              absl::StatusCode::kFailedPrecondition);

    ASSERT_TRUE(replica_b->poll().ok());
    ASSERT_TRUE(replica_c->poll().ok());
    EXPECT_EQ(replica_b->state_machine().machine().store().visible_applied_index(), 2U);
    EXPECT_EQ(replica_c->state_machine().machine().store().visible_applied_index(), 2U);

    ASSERT_TRUE(replica_a->demote().ok());
    EXPECT_EQ(replica_a
                  ->submit("stale-a", std::as_bytes(std::span(first->data(), first->size())))
                  .status()
                  .code(),
              absl::StatusCode::kFailedPrecondition);
    ASSERT_TRUE(replica_b->promote().ok());
    EXPECT_EQ(replica_b->status().role, minivessel::RuntimeRole::kPrimary);
    EXPECT_GT(replica_b->status().writer_epoch.value(), committed->lrsn.value() - 1);

    auto second = MakeEntry(*codec, 2, "row-b", "value-b");
    ASSERT_TRUE(second.ok()) << second.status();
    auto second_commit = replica_b->submit(
        "request-2", std::as_bytes(std::span(second->data(), second->size())));
    ASSERT_TRUE(second_commit.ok()) << second_commit.status();
    EXPECT_EQ(second_commit->lrsn, minivessel::Lrsn(4));

    ASSERT_TRUE(replica_a->poll().ok());
    ASSERT_TRUE(replica_c->poll().ok());
    for (const auto* replica : {replica_a.get(), replica_b.get(), replica_c.get()}) {
        EXPECT_EQ(replica->state_machine().machine().store().visible_applied_index(), 4U);
        EXPECT_EQ(replica->state_machine().machine().store().timestamp_high_watermark(), 102U);
        EXPECT_EQ(replica->state_machine().machine().export_dedupe_records().size(), 2U);
    }

    ASSERT_TRUE(replica_b->demote().ok());
    ASSERT_TRUE(replica_c->promote().ok());
    EXPECT_EQ(replica_c->status().role, minivessel::RuntimeRole::kPrimary);
    EXPECT_EQ(replica_c->status().applied_lrsn, minivessel::Lrsn(5));
    EXPECT_EQ(replica_c->state_machine().machine().store().visible_applied_index(), 4U);

    replica_a->stop();
    replica_b->stop();
    replica_c->stop();
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace pl::minitable
