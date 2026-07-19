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
// Created: 2026/07/18 00:36

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minitable/core/slice_raft_state_machine.h"
#include "cpp/pl/minitable/core/slice_snapshot.h"
#include "cpp/pl/minitable/core/slice_store.h"
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

template <typename Predicate> bool WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct TempSstPaths {
    std::string key;
    std::string value;
};

TempSstPaths MakeTempSstPaths(std::string_view suffix) {
    const auto directory = std::filesystem::path(::testing::TempDir()) / "minitable-flush";
    std::filesystem::create_directories(directory);
    const auto prefix = directory / std::string(suffix);
    std::filesystem::remove(prefix.string() + ".key");
    std::filesystem::remove(prefix.string() + ".value");
    return {.key = prefix.string() + ".key", .value = prefix.string() + ".value"};
}

std::string MakeManifestDirectory(std::string_view suffix) {
    const auto directory =
        std::filesystem::path(::testing::TempDir()) / ("minitable-manifest-" + std::string(suffix));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory.string();
}

std::string ReadValue(const SliceReadView& view, uint32_t locality_group_id, std::string_view key) {
    auto cursor = view.new_cursor(locality_group_id);
    EXPECT_TRUE(cursor.ok());
    if (!cursor.ok()) {
        return {};
    }
    EXPECT_TRUE((*cursor)->seek(key).ok());
    if (!(*cursor)->valid() || (*cursor)->key() != key) {
        return {};
    }
    return std::string((*cursor)->value());
}

std::shared_ptr<const codec::CellKeyCodec> MakeCodec() {
    auto schema = SchemaBuilder().add_column("key", DataType::kString).build();
    EXPECT_TRUE(schema.has_value());
    auto codec = codec::CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                             std::make_shared<const Schema>(std::move(*schema)));
    EXPECT_TRUE(codec.ok());
    return std::make_shared<const codec::CellKeyCodec>(std::move(*codec));
}

CommittedSliceMutation CanonicalCommitted(const codec::CellKeyCodec& codec,
                                          uint64_t index,
                                          std::string request_id,
                                          std::string row,
                                          std::string value) {
    VersionedStorageKey key{
        .storage_key = {.partition = GlobalOrderPrefix{},
                        .row_key = {Value::make<DataType::kString>(row)},
                        .target = CellRef{.column_family_id = 1, .qualifier = StaticQualifier{1}}},
        .commit_ts = {.domain_epoch = 1, .counter = index + 100},
        .mutation_seq = 0,
        .op_type = OpType::kPut};
    auto encoded_key = codec.EncodeVersionedStorageKey(key);
    EXPECT_TRUE(encoded_key.ok());
    return {.apply_index = index,
            .identity = {.client_id = "e2e",
                         .request_id = std::move(request_id),
                         .payload_hash = index + 1000},
            .commit_ts = key.commit_ts,
            .commit_physical_ms = 10'000 + index,
            .locality_group_mutations = {{{.encoded_key = std::move(*encoded_key),
                                           .encoded_value = std::move(value)}}},
            .locality_group_ids = {1},
            .serialized_response = "ok-" + std::to_string(index)};
}

CommittedSliceMutation Committed(uint64_t index,
                                 std::string request_id,
                                 uint64_t payload_hash,
                                 std::string key,
                                 std::string value,
                                 uint64_t physical_ms) {
    return {.apply_index = index,
            .identity = {.client_id = "client",
                         .request_id = std::move(request_id),
                         .payload_hash = payload_hash},
            .commit_ts = {.domain_epoch = 1, .counter = index + 100},
            .commit_physical_ms = physical_ms,
            .locality_group_mutations = {{{.encoded_key = std::move(key),
                                           .encoded_value = std::move(value)}}},
            .locality_group_ids = {1},
            .serialized_response = "response-" + std::to_string(index)};
}

TEST(SliceApplyMachineTest, SameCommittedLogProducesIdenticalState) {
    auto left_store = SliceStore::Create({{1, {}}});
    auto right_store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(left_store.ok() && right_store.ok());
    auto left = SliceApplyMachine::Create(std::move(*left_store));
    auto right = SliceApplyMachine::Create(std::move(*right_store));
    ASSERT_TRUE(left.ok() && right.ok());

    for (uint64_t index = 1; index <= 3; ++index) {
        const auto mutation = Committed(index,
                                        "request-" + std::to_string(index),
                                        index,
                                        "key-" + std::to_string(index),
                                        "value-" + std::to_string(index),
                                        1000 + index);
        ASSERT_TRUE((*left)->apply(mutation).ok());
        ASSERT_TRUE((*right)->apply(mutation).ok());
    }
    EXPECT_EQ((*left)->store().visible_applied_index(), 3U);
    EXPECT_EQ((*left)->store().timestamp_high_watermark(), 103U);
    EXPECT_EQ((*left)->store().last_commit_physical_ms(), 1003U);
    for (uint64_t index = 1; index <= 3; ++index) {
        const auto key = "key-" + std::to_string(index);
        EXPECT_EQ(ReadValue((*left)->store().read_view(), 1, key),
                  ReadValue((*right)->store().read_view(), 1, key));
    }
    EXPECT_EQ((*left)->export_dedupe_records(), (*right)->export_dedupe_records());
}

TEST(SliceApplyMachineTest, DuplicateReturnsOriginalResponseAndAdvancesRaftIndexOnly) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    auto machine = SliceApplyMachine::Create(std::move(*store));
    ASSERT_TRUE(machine.ok());
    auto original = Committed(1, "same", 11, "key", "original", 1001);
    auto first = (*machine)->apply(original);
    ASSERT_TRUE(first.ok());
    EXPECT_FALSE(first->duplicate);

    auto duplicate = Committed(2, "same", 11, "key", "must-not-apply", 1002);
    duplicate.serialized_response = "must-not-return";
    auto replay = (*machine)->apply(duplicate);
    ASSERT_TRUE(replay.ok());
    EXPECT_TRUE(replay->duplicate);
    EXPECT_EQ(replay->serialized_response, original.serialized_response);
    EXPECT_EQ((*machine)->store().visible_applied_index(), 2U);
    EXPECT_EQ(ReadValue((*machine)->store().read_view(), 1, "key"), "original");

    auto conflict = Committed(3, "same", 12, "key", "conflict", 1003);
    EXPECT_EQ((*machine)->apply(conflict).status().code(), absl::StatusCode::kAlreadyExists);
    EXPECT_EQ((*machine)->store().visible_applied_index(), 3U);
}

TEST(SliceApplyMachineTest, FlushReopenRestoresCommittedMetadataAndDedupeSnapshot) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("apply-reopen");
    const SliceStorePersistence persistence{
        .filesystem = filesystem, .manifest_directory = MakeManifestDirectory("apply-reopen")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    auto machine = SliceApplyMachine::Create(std::move(*store));
    ASSERT_TRUE(machine.ok());
    const auto mutation = Committed(7, "durable", 77, "key", "value", 7007);
    ASSERT_TRUE((*machine)->apply(mutation).ok());
    const auto records = (*machine)->export_dedupe_records();
    ASSERT_TRUE((*machine)->store().freeze_locality_group(1).ok());
    auto token = (*machine)->store().begin_flush(1);
    ASSERT_TRUE(token.ok());
    EXPECT_EQ(token->timestamp_high_watermark(), mutation.commit_ts.counter);
    EXPECT_EQ(token->last_commit_physical_ms(), mutation.commit_physical_ms);
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    ASSERT_TRUE((*machine)->store().install_flush(*token, *flush).ok());
    const auto manifest = (*machine)->store().persisted_manifest();
    machine->reset();

    auto reopened_store = SliceStore::Reopen(
        {.locality_groups = {{1, {}}}, .persistence = persistence, .manifest = manifest});
    ASSERT_TRUE(reopened_store.ok()) << reopened_store.status();
    EXPECT_EQ((*reopened_store)->timestamp_high_watermark(), mutation.commit_ts.counter);
    EXPECT_EQ((*reopened_store)->last_commit_physical_ms(), mutation.commit_physical_ms);
    auto reopened =
        SliceApplyMachine::Create(std::move(*reopened_store), {.dedupe_records = records});
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    auto duplicate = Committed(8, "durable", 77, "key", "wrong", 7008);
    auto result = (*reopened)->apply(duplicate);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->duplicate);
    EXPECT_EQ(result->serialized_response, mutation.serialized_response);
    EXPECT_EQ(ReadValue((*reopened)->store().read_view(), 1, "key"), "value");
}

TEST(SliceApplyMachineTest, RecoveryRejectsDedupeBeyondDurableAppliedIndex) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    DedupeRecord future{
        .identity = {.client_id = "client", .request_id = "future", .payload_hash = 1},
        .serialized_response = "response",
        .applied_index = 1,
        .commit_physical_ms = 1};
    EXPECT_EQ(
        SliceApplyMachine::Create(std::move(*store), {.dedupe_records = {future}}).status().code(),
        absl::StatusCode::kFailedPrecondition);
}

TEST(SliceApplyMachineTest, FreezeFenceOwnsTimestampMetadataBeforeLaterActiveApply) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    auto machine = SliceApplyMachine::Create(std::move(*store));
    ASSERT_TRUE(machine.ok());
    ASSERT_TRUE((*machine)->apply(Committed(1, "first", 1, "a", "one", 1001)).ok());
    ASSERT_TRUE((*machine)->store().freeze_locality_group(1).ok());
    ASSERT_TRUE((*machine)->apply(Committed(2, "second", 2, "b", "two", 1002)).ok());

    auto token = (*machine)->store().begin_flush(1);
    ASSERT_TRUE(token.ok());
    EXPECT_EQ(token->fence_index(), 1U);
    EXPECT_EQ(token->timestamp_high_watermark(), 101U);
    EXPECT_EQ(token->last_commit_physical_ms(), 1001U);
}

TEST(SliceRaftE2ETest, ThreeReplicasLeaderChangeRestartSnapshotRepairAndReplay) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    auto codec = MakeCodec();
    const auto comparator = MakeComparatorDomain(1, codec->row_key_schema_fingerprint(), 1, 0, 0);
    SliceSnapshotMetadata metadata{.table_id = 10,
                                   .slice_id = 20,
                                   .schema_version = 30,
                                   .route_epoch = 40,
                                   .replica_set_epoch = 50,
                                   .dedupe_retention_floor = 1};
    struct Replica {
        std::string manifest_directory;
        SliceStorePersistence persistence;
        std::unique_ptr<SliceRaftStateMachine> fsm;
    };
    std::vector<Replica> replicas;
    for (size_t replica_id = 0; replica_id < 3; ++replica_id) {
        auto directory = MakeManifestDirectory("raft-e2e-" + std::to_string(replica_id));
        SliceStorePersistence persistence{.filesystem = filesystem,
                                          .manifest_directory = directory,
                                          .comparator_domain = comparator};
        auto store = SliceStore::Create({{1, {}}}, persistence);
        ASSERT_TRUE(store.ok()) << store.status();
        auto machine = SliceApplyMachine::Create(std::move(*store));
        ASSERT_TRUE(machine.ok()) << machine.status();
        replicas.push_back(
            {.manifest_directory = directory,
             .persistence = persistence,
             .fsm = std::make_unique<SliceRaftStateMachine>(std::move(*machine), codec, metadata)});
    }

    std::vector<std::pair<uint64_t, std::string>> wal;
    for (uint64_t index = 1; index <= 3; ++index) {
        auto mutation = CanonicalCommitted(*codec,
                                           index,
                                           "request-" + std::to_string(index),
                                           "row-" + std::to_string(index),
                                           "value-" + std::to_string(index));
        auto row_key =
            codec->EncodeLogicalRowKey(mutation.locality_group_mutations[0].empty()
                                           ? std::vector<Value>{}
                                           : std::vector<Value>{Value::make<DataType::kString>(
                                                 "row-" + std::to_string(index))});
        ASSERT_TRUE(row_key.ok());
        auto encoded = EncodeSliceMutationV2(mutation, *row_key, *codec);
        ASSERT_TRUE(encoded.ok()) << encoded.status();
        wal.emplace_back(index, *encoded);
        for (auto& replica : replicas) {
            auto applied =
                replica.fsm->on_apply(index,
                                      index <= 2 ? 1 : 2,
                                      std::as_bytes(std::span(encoded->data(), encoded->size())));
            ASSERT_TRUE(applied.ok()) << applied.status();
        }
    }
    // The third entry was committed after leadership changed from replica 0 to replica 1.
    for (auto& replica : replicas) {
        EXPECT_EQ(replica.fsm->machine().store().visible_applied_index(), 3U);
        ASSERT_TRUE(replica.fsm->machine().store().freeze_locality_group(1).ok());
        auto token = replica.fsm->machine().store().begin_flush(1);
        ASSERT_TRUE(token.ok());
        const auto suffix = "raft-e2e-" + std::to_string(&replica - replicas.data());
        const auto paths = MakeTempSstPaths(suffix);
        auto flush = SliceStore::build_flush_sst(
            *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
        ASSERT_TRUE(flush.ok()) << flush.status();
        ASSERT_TRUE(replica.fsm->machine().store().install_flush(*token, *flush).ok());
        EXPECT_TRUE(replica.fsm->machine().store().can_snapshot_at(3));
    }
    auto snapshot = replicas[1].fsm->on_snapshot_save();
    ASSERT_TRUE(snapshot.ok()) << snapshot.status();

    // Replica 2 loses all volatile state, installs the leader Snapshot, then replays a new tail.
    auto empty_persistence = replicas[2].persistence;
    empty_persistence.manifest_directory = MakeManifestDirectory("raft-e2e-repair-target");
    auto empty_store = SliceStore::Create({{1, {}}}, empty_persistence);
    ASSERT_TRUE(empty_store.ok());
    auto empty_machine = SliceApplyMachine::Create(std::move(*empty_store));
    ASSERT_TRUE(empty_machine.ok());
    replicas[2].fsm =
        std::make_unique<SliceRaftStateMachine>(std::move(*empty_machine), codec, metadata);
    ASSERT_TRUE(replicas[2]
                    .fsm
                    ->on_snapshot_load(std::as_bytes(std::span(snapshot->data(), snapshot->size())),
                                       {{1, {}}},
                                       replicas[1].persistence)
                    .ok());
    EXPECT_EQ(replicas[2].fsm->machine().store().visible_applied_index(), 3U);
    const auto recovered_records = replicas[2].fsm->machine().export_dedupe_records();
    ASSERT_EQ(recovered_records.size(), 3U);

    auto fourth = CanonicalCommitted(*codec, 4, "request-4", "row-4", "value-4");
    auto fourth_row = codec->EncodeLogicalRowKey({Value::make<DataType::kString>("row-4")});
    ASSERT_TRUE(fourth_row.ok());
    auto fourth_encoded = EncodeSliceMutationV2(fourth, *fourth_row, *codec);
    ASSERT_TRUE(fourth_encoded.ok());
    const std::pair<uint64_t, std::string> tail{4, *fourth_encoded};
    ASSERT_TRUE(replicas[2].fsm->replay(std::span(&tail, 1), 3).ok());
    EXPECT_EQ(replicas[2].fsm->machine().store().visible_applied_index(), 4U);

    // Snapshot corruption is rejected before replacing installed state.
    (*snapshot)[snapshot->size() / 2] ^= 1;
    EXPECT_EQ(replicas[0]
                  .fsm
                  ->on_snapshot_load(std::as_bytes(std::span(snapshot->data(), snapshot->size())),
                                     {{1, {}}},
                                     replicas[1].persistence)
                  .code(),
              absl::StatusCode::kDataLoss);
    EXPECT_EQ(replicas[0].fsm->machine().store().visible_applied_index(), 3U);
}

TEST(SliceRaftStateMachineTest, RejectsLogGapWithoutAdvancingState) {
    auto codec = MakeCodec();
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    auto machine = SliceApplyMachine::Create(std::move(*store));
    ASSERT_TRUE(machine.ok());
    SliceRaftStateMachine fsm(std::move(*machine),
                              codec,
                              {.table_id = 1,
                               .slice_id = 1,
                               .schema_version = 1,
                               .route_epoch = 1,
                               .replica_set_epoch = 1,
                               .dedupe_retention_floor = 1});
    auto mutation = CanonicalCommitted(*codec, 2, "gap", "row", "value");
    auto row = codec->EncodeLogicalRowKey({Value::make<DataType::kString>("row")});
    ASSERT_TRUE(row.ok());
    auto encoded = EncodeSliceMutationV2(mutation, *row, *codec);
    ASSERT_TRUE(encoded.ok());
    auto result = fsm.on_apply(2, 1, std::as_bytes(std::span(encoded->data(), encoded->size())));
    EXPECT_EQ(result.status().code(), absl::StatusCode::kDataLoss);
    EXPECT_EQ(fsm.machine().store().visible_applied_index(), 0U);
}

TEST(SliceStoreTest, AppliesMultipleLocalityGroupsAtOneVisibilityPoint) {
    auto store = SliceStore::Create({{1, {}}, {2, {}}});
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> first = {
        {.encoded_key = "row/cf1", .encoded_value = "one"}};
    const std::vector<MemTableMutation> second = {
        {.encoded_key = "row/cf2", .encoded_value = "two"}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 2, .mutations = second},
        {.locality_group_id = 1, .mutations = first},
    };

    ASSERT_TRUE((*store)->apply(patches, 7).ok());
    const auto view = (*store)->read_view();
    EXPECT_EQ(view.visible_applied_index(), 7U);
    EXPECT_EQ(ReadValue(view, 1, "row/cf1"), "one");
    EXPECT_EQ(ReadValue(view, 2, "row/cf2"), "two");
}

TEST(SliceStoreTest, PrepareFailureAbortsEveryLocalityGroup) {
    auto store = SliceStore::Create({{1, {.memory_limit_bytes = 128, .arena_block_bytes = 8}},
                                     {2, {.memory_limit_bytes = 32, .arena_block_bytes = 8}}});
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> first = {{.encoded_key = "a", .encoded_value = "accepted"}};
    const std::vector<MemTableMutation> second = {
        {.encoded_key = "b", .encoded_value = std::string(64, 'x')}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = first},
        {.locality_group_id = 2, .mutations = second},
    };

    EXPECT_EQ((*store)->apply(patches, 1).code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ((*store)->visible_applied_index(), 0U);
    EXPECT_TRUE(ReadValue((*store)->read_view(), 1, "a").empty());
    EXPECT_TRUE(ReadValue((*store)->read_view(), 2, "b").empty());

    const std::vector<LocalityGroupPatch> retry = {{.locality_group_id = 1, .mutations = first}};
    EXPECT_TRUE((*store)->apply(retry, 1).ok());
}

TEST(SliceStoreTest, PinnedReadViewRetainsOldCrossLocalityGroupState) {
    auto store = SliceStore::Create({{1, {}}, {2, {}}});
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> lg1v1 = {{.encoded_key = "k", .encoded_value = "1a"}};
    const std::vector<MemTableMutation> lg2v1 = {{.encoded_key = "k", .encoded_value = "1b"}};
    const std::vector<LocalityGroupPatch> first = {
        {.locality_group_id = 1, .mutations = lg1v1},
        {.locality_group_id = 2, .mutations = lg2v1},
    };
    ASSERT_TRUE((*store)->apply(first, 1).ok());
    const auto old_view = (*store)->read_view();

    const std::vector<MemTableMutation> lg1v2 = {{.encoded_key = "k", .encoded_value = "2a"}};
    const std::vector<MemTableMutation> lg2v2 = {{.encoded_key = "k", .encoded_value = "2b"}};
    const std::vector<LocalityGroupPatch> second = {
        {.locality_group_id = 1, .mutations = lg1v2},
        {.locality_group_id = 2, .mutations = lg2v2},
    };
    ASSERT_TRUE((*store)->apply(second, 2).ok());
    const auto current_view = (*store)->read_view();

    EXPECT_EQ(ReadValue(old_view, 1, "k"), "1a");
    EXPECT_EQ(ReadValue(old_view, 2, "k"), "1b");
    EXPECT_EQ(ReadValue(current_view, 1, "k"), "2a");
    EXPECT_EQ(ReadValue(current_view, 2, "k"), "2b");
}

TEST(SliceStoreTest, FreezePublishesNewGenerationAndPinsOldView) {
    auto store = SliceStore::Create({{1, {}}, {2, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "old"},
        {.encoded_key = "only-immutable", .encoded_value = "i"},
    };
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patches, 1).ok());

    const auto old_view = (*store)->read_view();
    EXPECT_EQ(old_view.generation(), 1U);
    EXPECT_FALSE(old_view.has_immutable(1));
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());

    const auto frozen_view = (*store)->read_view();
    EXPECT_EQ(frozen_view.generation(), 2U);
    EXPECT_EQ(frozen_view.visible_applied_index(), 1U);
    EXPECT_TRUE(frozen_view.has_immutable(1));
    EXPECT_FALSE(frozen_view.has_immutable(2));
    EXPECT_FALSE(old_view.has_immutable(1));
    EXPECT_EQ(ReadValue(old_view, 1, "a"), "old");
    EXPECT_EQ(ReadValue(frozen_view, 1, "a"), "old");
}

TEST(SliceStoreTest, ActiveShadowsImmutableAndMergedCursorDeduplicatesKeys) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> before = {
        {.encoded_key = "a", .encoded_value = "immutable-a"},
        {.encoded_key = "b", .encoded_value = "immutable-b"},
    };
    const std::vector<LocalityGroupPatch> first = {{.locality_group_id = 1, .mutations = before}};
    ASSERT_TRUE((*store)->apply(first, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());

    const std::vector<MemTableMutation> after = {
        {.encoded_key = "a", .encoded_value = "active-a"},
        {.encoded_key = "c", .encoded_value = "active-c"},
    };
    const std::vector<LocalityGroupPatch> second = {{.locality_group_id = 1, .mutations = after}};
    ASSERT_TRUE((*store)->apply(second, 2).ok());

    const auto view = (*store)->read_view();
    auto cursor = view.new_cursor(1);
    ASSERT_TRUE(cursor.ok());
    ASSERT_TRUE((*cursor)->seek_to_first().ok());
    std::vector<std::pair<std::string, std::string>> rows;
    while ((*cursor)->valid()) {
        rows.emplace_back((*cursor)->key(), (*cursor)->value());
        ASSERT_TRUE((*cursor)->next().ok());
    }
    EXPECT_EQ(rows,
              (std::vector<std::pair<std::string, std::string>>{
                  {"a", "active-a"}, {"b", "immutable-b"}, {"c", "active-c"}}));
}

TEST(SliceStoreTest, RejectsSecondFreezeUntilImmutableIsRetired) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "a", .encoded_value = "1"}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patches, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    EXPECT_EQ((*store)->freeze_locality_group(1).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ((*store)->freeze_locality_group(99).code(), absl::StatusCode::kNotFound);
    EXPECT_EQ((*store)->generation(), 2U);
}

TEST(SliceStoreTest, FlushInstallsIdentityVerifiedSstAndRetiresImmutable) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> before = {
        {.encoded_key = "a", .encoded_value = "old-a"},
        {.encoded_key = "b", .encoded_value = "old-b"},
    };
    const std::vector<LocalityGroupPatch> first = {{.locality_group_id = 1, .mutations = before}};
    ASSERT_TRUE((*store)->apply(first, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    const auto pre_install = (*store)->read_view();

    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    EXPECT_EQ(token->fence_index(), 1U);
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("basic");
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok()) << flush.status();
    EXPECT_EQ(flush->identity().row_count, 2U);
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());

    const auto post_install = (*store)->read_view();
    EXPECT_EQ(post_install.visible_applied_index(), 1U);
    EXPECT_EQ(post_install.generation(), 3U);
    EXPECT_FALSE(post_install.has_immutable(1));
    EXPECT_EQ(post_install.sst_count(1), 1U);
    EXPECT_TRUE(pre_install.has_immutable(1));
    EXPECT_EQ(pre_install.sst_count(1), 0U);
    EXPECT_EQ(ReadValue(pre_install, 1, "a"), "old-a");
    EXPECT_EQ(ReadValue(post_install, 1, "a"), "old-a");
    EXPECT_EQ(ReadValue(post_install, 1, "b"), "old-b");

    const std::vector<MemTableMutation> next = {{.encoded_key = "c", .encoded_value = "new"}};
    const std::vector<LocalityGroupPatch> next_patch = {
        {.locality_group_id = 1, .mutations = next}};
    ASSERT_TRUE((*store)->apply(next_patch, 2).ok());
    EXPECT_TRUE((*store)->freeze_locality_group(1).ok());
}

TEST(SliceStoreTest, ActiveShadowsInstalledSstAndCursorSeeksAcrossSources) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> before = {
        {.encoded_key = "a", .encoded_value = "sst-a"},
        {.encoded_key = "b", .encoded_value = "sst-b"},
    };
    const std::vector<LocalityGroupPatch> first = {{.locality_group_id = 1, .mutations = before}};
    ASSERT_TRUE((*store)->apply(first, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    const auto paths = MakeTempSstPaths("shadow");
    auto flush =
        SliceStore::build_flush_sst(*token,
                                    {.filesystem = std::make_shared<sstv2::io::LocalFileSystem>(),
                                     .key_path = paths.key,
                                     .value_path = paths.value});
    ASSERT_TRUE(flush.ok()) << flush.status();
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());

    const std::vector<MemTableMutation> after = {
        {.encoded_key = "a", .encoded_value = "active-a"},
        {.encoded_key = "c", .encoded_value = "active-c"},
    };
    const std::vector<LocalityGroupPatch> second = {{.locality_group_id = 1, .mutations = after}};
    ASSERT_TRUE((*store)->apply(second, 2).ok());

    auto cursor = (*store)->read_view().new_cursor(1);
    ASSERT_TRUE(cursor.ok());
    ASSERT_TRUE((*cursor)->seek("a").ok());
    std::vector<std::pair<std::string, std::string>> rows;
    while ((*cursor)->valid()) {
        rows.emplace_back((*cursor)->key(), (*cursor)->value());
        ASSERT_TRUE((*cursor)->next().ok());
    }
    EXPECT_EQ(rows,
              (std::vector<std::pair<std::string, std::string>>{
                  {"a", "active-a"}, {"b", "sst-b"}, {"c", "active-c"}}));
}

TEST(SliceStoreTest, InstallIsIdempotentAndRejectsStaleTokenAfterNextFreeze) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "a", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> patch = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patch, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    const auto paths = MakeTempSstPaths("idempotent");
    auto flush =
        SliceStore::build_flush_sst(*token,
                                    {.filesystem = std::make_shared<sstv2::io::LocalFileSystem>(),
                                     .key_path = paths.key,
                                     .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());
    EXPECT_TRUE((*store)->install_flush(*token, *flush).ok());
    EXPECT_EQ((*store)->read_view().sst_count(1), 1U);

    const std::vector<MemTableMutation> next = {{.encoded_key = "b", .encoded_value = "next"}};
    const std::vector<LocalityGroupPatch> next_patch = {
        {.locality_group_id = 1, .mutations = next}};
    ASSERT_TRUE((*store)->apply(next_patch, 2).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    EXPECT_EQ((*store)->install_flush(*token, *flush).code(), absl::StatusCode::kOk);
    auto next_token = (*store)->begin_flush(1);
    ASSERT_TRUE(next_token.ok());
    EXPECT_NE(next_token->immutable_id(), token->immutable_id());
}

TEST(SliceStoreTest, FlushBuildFailurePreservesImmutable) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "a", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> patch = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patch, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    auto flush =
        SliceStore::build_flush_sst(*token,
                                    {.filesystem = std::make_shared<sstv2::io::LocalFileSystem>(),
                                     .key_path = "/missing/minitable/key",
                                     .value_path = "/missing/minitable/value"});
    EXPECT_FALSE(flush.ok());
    EXPECT_TRUE((*store)->read_view().has_immutable(1));
    EXPECT_EQ((*store)->read_view().sst_count(1), 0U);
    EXPECT_EQ(ReadValue((*store)->read_view(), 1, "a"), "v");
}

TEST(SliceStoreTest, RejectsMismatchedFlushProvenanceWithoutRetiringImmutable) {
    auto first_store = SliceStore::Create({{1, {}}});
    auto second_store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(first_store.ok());
    ASSERT_TRUE(second_store.ok());
    const std::vector<MemTableMutation> first_value = {
        {.encoded_key = "k", .encoded_value = "first"}};
    const std::vector<MemTableMutation> second_value = {
        {.encoded_key = "k", .encoded_value = "second"}};
    const std::vector<LocalityGroupPatch> first_patch = {
        {.locality_group_id = 1, .mutations = first_value}};
    const std::vector<LocalityGroupPatch> second_patch = {
        {.locality_group_id = 1, .mutations = second_value}};
    ASSERT_TRUE((*first_store)->apply(first_patch, 1).ok());
    ASSERT_TRUE((*second_store)->apply(second_patch, 1).ok());
    ASSERT_TRUE((*first_store)->freeze_locality_group(1).ok());
    ASSERT_TRUE((*second_store)->freeze_locality_group(1).ok());
    auto first_token = (*first_store)->begin_flush(1);
    auto second_token = (*second_store)->begin_flush(1);
    ASSERT_TRUE(first_token.ok());
    ASSERT_TRUE(second_token.ok());
    const auto paths = MakeTempSstPaths("provenance");
    auto first_flush =
        SliceStore::build_flush_sst(*first_token,
                                    {.filesystem = std::make_shared<sstv2::io::LocalFileSystem>(),
                                     .key_path = paths.key,
                                     .value_path = paths.value});
    ASSERT_TRUE(first_flush.ok());

    EXPECT_EQ((*second_store)->install_flush(*second_token, *first_flush).code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE((*second_store)->read_view().has_immutable(1));
    EXPECT_EQ((*second_store)->read_view().sst_count(1), 0U);
    EXPECT_EQ(ReadValue((*second_store)->read_view(), 1, "k"), "second");
}

TEST(SliceStoreTest, MultipleFlushGenerationsUseNewestSourcePriority) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    for (uint64_t index = 1; index <= 2; ++index) {
        const std::string value = "v" + std::to_string(index);
        const std::vector<MemTableMutation> mutations = {
            {.encoded_key = "k", .encoded_value = value}};
        const std::vector<LocalityGroupPatch> patch = {
            {.locality_group_id = 1, .mutations = mutations}};
        ASSERT_TRUE((*store)->apply(patch, index).ok());
        ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
        auto token = (*store)->begin_flush(1);
        ASSERT_TRUE(token.ok());
        const auto paths = MakeTempSstPaths("generation-" + std::to_string(index));
        auto flush = SliceStore::build_flush_sst(
            *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
        ASSERT_TRUE(flush.ok());
        ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());
    }
    EXPECT_EQ((*store)->read_view().sst_count(1), 2U);
    EXPECT_EQ(ReadValue((*store)->read_view(), 1, "k"), "v2");
}

TEST(SliceStoreTest, RejectsEmptyActiveFreeze) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->freeze_locality_group(1).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_FALSE((*store)->begin_flush(1).ok());
}

TEST(SliceStoreTest, ApplyDuringFlushBuildSurvivesAtomicInstall) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> old_data = {
        {.encoded_key = "old", .encoded_value = "immutable"}};
    const std::vector<LocalityGroupPatch> old_patch = {
        {.locality_group_id = 1, .mutations = old_data}};
    ASSERT_TRUE((*store)->apply(old_patch, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    const auto paths = MakeTempSstPaths("apply-during-flush");
    auto flush =
        SliceStore::build_flush_sst(*token,
                                    {.filesystem = std::make_shared<sstv2::io::LocalFileSystem>(),
                                     .key_path = paths.key,
                                     .value_path = paths.value});
    ASSERT_TRUE(flush.ok());

    const std::vector<MemTableMutation> new_data = {
        {.encoded_key = "new", .encoded_value = "active"}};
    const std::vector<LocalityGroupPatch> new_patch = {
        {.locality_group_id = 1, .mutations = new_data}};
    ASSERT_TRUE((*store)->apply(new_patch, 2).ok());
    const auto before_install = (*store)->read_view();
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());
    const auto after_install = (*store)->read_view();

    EXPECT_EQ(before_install.visible_applied_index(), 2U);
    EXPECT_EQ(after_install.visible_applied_index(), 2U);
    EXPECT_EQ(ReadValue(before_install, 1, "old"), "immutable");
    EXPECT_EQ(ReadValue(before_install, 1, "new"), "active");
    EXPECT_EQ(ReadValue(after_install, 1, "old"), "immutable");
    EXPECT_EQ(ReadValue(after_install, 1, "new"), "active");
    EXPECT_TRUE(before_install.has_immutable(1));
    EXPECT_FALSE(after_install.has_immutable(1));
    EXPECT_EQ(after_install.sst_count(1), 1U);
}

TEST(SliceStoreTest, ConcurrentReadViewsNeverObserveMixedApplyIndexes) {
    auto store =
        SliceStore::Create({{1, {.memory_limit_bytes = 64 * 1024, .arena_block_bytes = 256}},
                            {2, {.memory_limit_bytes = 64 * 1024, .arena_block_bytes = 256}}});
    ASSERT_TRUE(store.ok());

    std::atomic<uint64_t> requested_index = 0;
    std::atomic<uint64_t> observed_index = 0;
    std::atomic<bool> mixed = false;
    std::thread reader([&] {
        for (uint64_t expected = 1; expected <= 100; ++expected) {
            if (!WaitUntil(
                    [&] { return requested_index.load(std::memory_order_acquire) >= expected; })) {
                mixed.store(true, std::memory_order_release);
                return;
            }
            const auto view = (*store)->read_view();
            const auto first = ReadValue(view, 1, "k");
            const auto second = ReadValue(view, 2, "k");
            if (view.visible_applied_index() != expected || first != second ||
                first != std::to_string(expected)) {
                mixed.store(true, std::memory_order_release);
                return;
            }
            observed_index.store(expected, std::memory_order_release);
        }
    });

    for (uint64_t index = 1; index <= 100; ++index) {
        const std::string value = std::to_string(index);
        const std::vector<MemTableMutation> first = {{.encoded_key = "k", .encoded_value = value}};
        const std::vector<MemTableMutation> second = {{.encoded_key = "k", .encoded_value = value}};
        const std::vector<LocalityGroupPatch> patches = {
            {.locality_group_id = 1, .mutations = first},
            {.locality_group_id = 2, .mutations = second},
        };
        const auto status = (*store)->apply(patches, index);
        if (!status.ok()) {
            mixed.store(true, std::memory_order_release);
            requested_index.store(100, std::memory_order_release);
            break;
        }
        requested_index.store(index, std::memory_order_release);
        if (!WaitUntil([&] {
                return observed_index.load(std::memory_order_acquire) >= index ||
                       mixed.load(std::memory_order_acquire);
            })) {
            mixed.store(true, std::memory_order_release);
            requested_index.store(100, std::memory_order_release);
            break;
        }
    }
    reader.join();
    EXPECT_EQ(observed_index.load(std::memory_order_acquire), 100U);
    EXPECT_FALSE(mixed.load(std::memory_order_acquire));
}

TEST(SliceStoreTest, ConcurrentReadersPinCompatibleGenerationAndWatermark) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> first = {{.encoded_key = "k", .encoded_value = "v1"}};
    const std::vector<LocalityGroupPatch> first_patch = {
        {.locality_group_id = 1, .mutations = first}};
    ASSERT_TRUE((*store)->apply(first_patch, 1).ok());

    std::atomic<bool> stop = false;
    std::atomic<bool> invalid = false;
    std::atomic<uint32_t> observed_stages = 0;
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto view = (*store)->read_view();
            const auto value = ReadValue(view, 1, "k");
            const bool before_freeze = view.generation() == 1 &&
                                       view.visible_applied_index() == 1 &&
                                       !view.has_immutable(1) && value == "v1";
            const bool after_freeze = view.generation() == 2 && view.visible_applied_index() == 1 &&
                                      view.has_immutable(1) && value == "v1";
            const bool after_apply = view.generation() == 2 && view.visible_applied_index() == 2 &&
                                     view.has_immutable(1) && value == "v2";
            if (!before_freeze && !after_freeze && !after_apply) {
                invalid.store(true, std::memory_order_release);
                return;
            }
            observed_stages.fetch_or(before_freeze ? 1U : (after_freeze ? 2U : 4U),
                                     std::memory_order_release);
        }
    });

    if (!WaitUntil([&] {
            return (observed_stages.load(std::memory_order_acquire) & 1U) != 0 ||
                   invalid.load(std::memory_order_acquire);
        })) {
        invalid.store(true, std::memory_order_release);
    }
    const auto freeze_status = (*store)->freeze_locality_group(1);
    if (!freeze_status.ok()) {
        invalid.store(true, std::memory_order_release);
    }
    if (!WaitUntil([&] {
            return (observed_stages.load(std::memory_order_acquire) & 2U) != 0 ||
                   invalid.load(std::memory_order_acquire);
        })) {
        invalid.store(true, std::memory_order_release);
    }

    const std::vector<MemTableMutation> second = {{.encoded_key = "k", .encoded_value = "v2"}};
    const std::vector<LocalityGroupPatch> second_patch = {
        {.locality_group_id = 1, .mutations = second}};
    const auto apply_status = (*store)->apply(second_patch, 2);
    if (!apply_status.ok()) {
        invalid.store(true, std::memory_order_release);
    }
    if (!WaitUntil([&] {
            return (observed_stages.load(std::memory_order_acquire) & 4U) != 0 ||
                   invalid.load(std::memory_order_acquire);
        })) {
        invalid.store(true, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    EXPECT_EQ(observed_stages.load(std::memory_order_acquire), 7U);
    EXPECT_FALSE(invalid.load(std::memory_order_acquire));
}

TEST(SliceStoreTest, PersistentManifestReopensInstalledSstAfterCrashBoundary) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("persistent-reopen");
    const auto manifest_directory = MakeManifestDirectory("persistent-reopen");
    const SliceStorePersistence persistence{.filesystem = filesystem,
                                            .manifest_directory = manifest_directory};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "persisted"}};
    const std::vector<LocalityGroupPatch> patch = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patch, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());
    const auto persisted = (*store)->persisted_manifest();
    ASSERT_FALSE(persisted.path.empty());
    EXPECT_EQ(persisted.generation, 2U);

    store->reset();
    auto reopened = SliceStore::Reopen(
        {.locality_groups = {{1, {}}}, .persistence = persistence, .manifest = persisted});
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    EXPECT_EQ((*reopened)->generation(), 2U);
    EXPECT_EQ((*reopened)->visible_applied_index(), 1U);
    EXPECT_EQ((*reopened)->read_view().sst_count(1), 1U);
    EXPECT_EQ(ReadValue((*reopened)->read_view(), 1, "a"), "persisted");
}

TEST(SliceStoreTest, CrashBeforeInstallKeepsPreviousManifestAuthoritative) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("crash-before-install");
    const SliceStorePersistence persistence{.filesystem = filesystem,
                                            .manifest_directory =
                                                MakeManifestDirectory("crash-before-install")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    const auto initial_manifest = (*store)->persisted_manifest();
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "lost", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> patch = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patch, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    store->reset();

    auto reopened = SliceStore::Reopen(
        {.locality_groups = {{1, {}}}, .persistence = persistence, .manifest = initial_manifest});
    ASSERT_TRUE(reopened.ok());
    EXPECT_EQ((*reopened)->read_view().sst_count(1), 0U);
    EXPECT_TRUE(ReadValue((*reopened)->read_view(), 1, "lost").empty());
}

TEST(SliceStoreTest, PersistentStoreCanInstallAgainAfterReopen) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto first_paths = MakeTempSstPaths("reopen-cycle-first");
    const SliceStorePersistence persistence{
        .filesystem = filesystem, .manifest_directory = MakeManifestDirectory("reopen-cycle")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> first = {{.encoded_key = "a", .encoded_value = "one"}};
    const std::vector<LocalityGroupPatch> first_patch = {
        {.locality_group_id = 1, .mutations = first}};
    ASSERT_TRUE((*store)->apply(first_patch, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto first_token = (*store)->begin_flush(1);
    ASSERT_TRUE(first_token.ok());
    auto first_flush = SliceStore::build_flush_sst(
        *first_token,
        {.filesystem = filesystem, .key_path = first_paths.key, .value_path = first_paths.value});
    ASSERT_TRUE(first_flush.ok());
    ASSERT_TRUE((*store)->install_flush(*first_token, *first_flush).ok());
    auto persisted = (*store)->persisted_manifest();
    store->reset();

    auto reopened = SliceStore::Reopen(
        {.locality_groups = {{1, {}}}, .persistence = persistence, .manifest = persisted});
    ASSERT_TRUE(reopened.ok());
    const std::vector<MemTableMutation> second = {{.encoded_key = "b", .encoded_value = "two"}};
    const std::vector<LocalityGroupPatch> second_patch = {
        {.locality_group_id = 1, .mutations = second}};
    ASSERT_TRUE((*reopened)->apply(second_patch, 2).ok());
    ASSERT_TRUE((*reopened)->freeze_locality_group(1).ok());
    auto second_token = (*reopened)->begin_flush(1);
    ASSERT_TRUE(second_token.ok());
    const auto second_paths = MakeTempSstPaths("reopen-cycle-second");
    auto second_flush = SliceStore::build_flush_sst(
        *second_token,
        {.filesystem = filesystem, .key_path = second_paths.key, .value_path = second_paths.value});
    ASSERT_TRUE(second_flush.ok());
    ASSERT_TRUE((*reopened)->install_flush(*second_token, *second_flush).ok());
    EXPECT_EQ((*reopened)->read_view().sst_count(1), 2U);
    EXPECT_EQ(ReadValue((*reopened)->read_view(), 1, "a"), "one");
    EXPECT_EQ(ReadValue((*reopened)->read_view(), 1, "b"), "two");
}

TEST(SliceStoreTest, PersistentModeRejectsMultipleLocalityGroupsUntilSnapshotRecovery) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("multi-lg-persistent");
    EXPECT_EQ(
        SliceStore::Create({{1, {}}, {2, {}}},
                           {.filesystem = filesystem,
                            .manifest_directory = MakeManifestDirectory("multi-lg-persistent")})
            .status()
            .code(),
        absl::StatusCode::kUnimplemented);
}

TEST(SliceStoreTest, ReopenRejectsManifestAndSstIdentityTampering) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("persistent-tamper");
    const SliceStorePersistence persistence{
        .filesystem = filesystem, .manifest_directory = MakeManifestDirectory("persistent-tamper")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    const auto persisted = (*store)->persisted_manifest();
    store->reset();

    auto wrong = persisted;
    ++wrong.identity.checksum;
    EXPECT_EQ(SliceStore::Reopen(
                  {.locality_groups = {{1, {}}}, .persistence = persistence, .manifest = wrong})
                  .status()
                  .code(),
              absl::StatusCode::kFailedPrecondition);
}

TEST(SliceStoreTest, ManifestEditCasIsDeterministicAndLedgerIsBounded) {
    ManifestRef manifest;
    manifest.locality_groups.emplace(1, LocalityGroupManifest{});
    const auto paths = MakeTempSstPaths("manifest-cas");
    SstIdentity identity{
        .key_path = paths.key,
        .value_path = paths.value,
        .key_file = {.file_id = 1, .length = 1, .checksum = 1, .checksum_valid = true},
        .value_file = {.file_id = 2, .length = 1, .checksum = 2, .checksum_valid = true},
        .row_count = 1,
    };
    for (uint64_t edit_id = 1; edit_id <= kMaxInstalledEditLedgerEntries + 3; ++edit_id) {
        auto next = ApplyManifestEdit(manifest,
                                      {.edit_id = edit_id,
                                       .locality_group_id = 1,
                                       .parent_generation = manifest.generation,
                                       .new_generation = manifest.generation + 1,
                                       .immutable_id = edit_id,
                                       .immutable_fence_index = edit_id,
                                       .outputs = {identity}});
        ASSERT_TRUE(next.ok()) << next.status();
        manifest = **next;
    }
    EXPECT_EQ(manifest.installed_edits.size(), kMaxInstalledEditLedgerEntries);
    EXPECT_EQ(manifest.installed_edits.front().edit_id, 4U);
    const auto& installed = manifest.installed_edits.back();
    auto replay = ApplyManifestEdit(manifest,
                                    {.edit_id = installed.edit_id,
                                     .locality_group_id = 1,
                                     .parent_generation = manifest.generation - 1,
                                     .new_generation = installed.result_generation,
                                     .immutable_id = installed.edit_id,
                                     .immutable_fence_index = installed.edit_id,
                                     .outputs = {identity}});
    ASSERT_TRUE(replay.ok()) << replay.status();
    auto conflicting_replay = ApplyManifestEdit(manifest,
                                                {.edit_id = installed.edit_id,
                                                 .locality_group_id = 1,
                                                 .parent_generation = manifest.generation - 1,
                                                 .new_generation = installed.result_generation,
                                                 .immutable_id = installed.edit_id,
                                                 .immutable_fence_index = installed.edit_id + 1,
                                                 .outputs = {identity}});
    EXPECT_EQ(conflicting_replay.status().code(), absl::StatusCode::kAlreadyExists);
    auto stale = ApplyManifestEdit(manifest,
                                   {.edit_id = 1000,
                                    .locality_group_id = 1,
                                    .parent_generation = 1,
                                    .new_generation = 2,
                                    .immutable_id = 1000,
                                    .immutable_fence_index = 1,
                                    .outputs = {identity}});
    EXPECT_EQ(stale.status().code(), absl::StatusCode::kAborted);
}

TEST(SliceStoreTest, FlushFaultPointsPreserveAuthoritativeManifestAndCleanBuildFiles) {
    for (const auto point : {FlushFaultPoint::kBeforeKeyCreate,
                             FlushFaultPoint::kAfterKeyCreate,
                             FlushFaultPoint::kAfterValueCreate,
                             FlushFaultPoint::kBeforeSstFinalize,
                             FlushFaultPoint::kAfterSstFinalize}) {
        auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
        const auto paths = MakeTempSstPaths("fault-" + std::to_string(static_cast<int>(point)));
        auto store = SliceStore::Create({{1, {}}});
        ASSERT_TRUE(store.ok());
        const std::vector<MemTableMutation> mutations = {
            {.encoded_key = "k", .encoded_value = "v"}};
        const std::vector<LocalityGroupPatch> patches = {
            {.locality_group_id = 1, .mutations = mutations}};
        ASSERT_TRUE((*store)->apply(patches, 1).ok());
        ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
        auto token = (*store)->begin_flush(1);
        ASSERT_TRUE(token.ok());
        auto result = SliceStore::build_flush_sst(
            *token,
            {.filesystem = filesystem,
             .key_path = paths.key,
             .value_path = paths.value,
             .fault_injector = [point](FlushFaultPoint current) {
                 return current == point ? absl::UnavailableError("injected") : absl::OkStatus();
             }});
        EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);
        EXPECT_TRUE((*store)->read_view().has_immutable(1));
        EXPECT_FALSE(std::filesystem::exists(paths.key));
        EXPECT_FALSE(std::filesystem::exists(paths.value));
    }
}

TEST(SliceStoreTest, ManifestFaultPointsRecordCollectableOrphans) {
    for (const auto point : {FlushFaultPoint::kBeforeManifestPersist,
                             FlushFaultPoint::kAfterManifestPersist,
                             FlushFaultPoint::kBeforeReadStatePublish}) {
        auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
        const auto suffix = "manifest-fault-" + std::to_string(static_cast<int>(point));
        const auto paths = MakeTempSstPaths(suffix);
        const SliceStorePersistence persistence{.filesystem = filesystem,
                                                .manifest_directory = MakeManifestDirectory(suffix),
                                                .fault_injector = [point](FlushFaultPoint current) {
                                                    return current == point
                                                               ? absl::UnavailableError("injected")
                                                               : absl::OkStatus();
                                                }};
        auto store = SliceStore::Create({{1, {}}}, persistence);
        ASSERT_TRUE(store.ok());
        const auto initial = (*store)->persisted_manifest();
        const std::vector<MemTableMutation> mutations = {
            {.encoded_key = "k", .encoded_value = "v"}};
        const std::vector<LocalityGroupPatch> patches = {
            {.locality_group_id = 1, .mutations = mutations}};
        ASSERT_TRUE((*store)->apply(patches, 1).ok());
        ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
        auto token = (*store)->begin_flush(1);
        ASSERT_TRUE(token.ok());
        auto flush = SliceStore::build_flush_sst(
            *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
        ASSERT_TRUE(flush.ok());
        EXPECT_EQ((*store)->install_flush(*token, *flush).code(), absl::StatusCode::kUnavailable);
        EXPECT_EQ((*store)->persisted_manifest(), initial);
        EXPECT_EQ((*store)->orphan_count(), 1U);
        EXPECT_TRUE((*store)->collect_orphans().ok());
        EXPECT_EQ((*store)->orphan_count(), 0U);
        EXPECT_FALSE(std::filesystem::exists(paths.key));
        EXPECT_FALSE(std::filesystem::exists(paths.value));
    }
}

TEST(SliceStoreTest, ManifestGcHonorsGenerationSafeWatermark) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("manifest-gc");
    const SliceStorePersistence persistence{
        .filesystem = filesystem, .manifest_directory = MakeManifestDirectory("manifest-gc")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    const auto first_manifest = (*store)->persisted_manifest();
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "k", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patches, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());
    ASSERT_TRUE(std::filesystem::exists(first_manifest.path));

    ASSERT_TRUE((*store)->collect_manifests_before(1).ok());
    EXPECT_TRUE(std::filesystem::exists(first_manifest.path));
    ASSERT_TRUE((*store)->collect_manifests_before(2).ok());
    EXPECT_FALSE(std::filesystem::exists(first_manifest.path));
    EXPECT_TRUE(std::filesystem::exists((*store)->persisted_manifest().path));
}

TEST(SliceStoreTest, ReopenAcceptsExplicitCrashOrphansAndIdentityFencedGc) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("recovery-orphan");
    const SliceStorePersistence persistence{
        .filesystem = filesystem, .manifest_directory = MakeManifestDirectory("recovery-orphan")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    const auto initial = (*store)->persisted_manifest();
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "orphan", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patches, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    const auto orphan = flush->identity();
    store->reset();

    auto reopened = SliceStore::Reopen({.locality_groups = {{1, {}}},
                                        .persistence = persistence,
                                        .manifest = initial,
                                        .orphan_candidates = {orphan}});
    ASSERT_TRUE(reopened.ok());
    EXPECT_EQ((*reopened)->read_view().sst_count(1), 0U);
    EXPECT_EQ((*reopened)->orphan_count(), 1U);
    ASSERT_TRUE((*reopened)->collect_orphans().ok());
    EXPECT_FALSE(std::filesystem::exists(paths.key));
    EXPECT_FALSE(std::filesystem::exists(paths.value));
}

TEST(SliceStoreTest, ReopenRejectsLiveSstMisclassifiedAsOrphan) {
    auto filesystem = std::make_shared<sstv2::io::LocalFileSystem>();
    const auto paths = MakeTempSstPaths("live-not-orphan");
    const SliceStorePersistence persistence{
        .filesystem = filesystem, .manifest_directory = MakeManifestDirectory("live-not-orphan")};
    auto store = SliceStore::Create({{1, {}}}, persistence);
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "k", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(patches, 1).ok());
    ASSERT_TRUE((*store)->freeze_locality_group(1).ok());
    auto token = (*store)->begin_flush(1);
    ASSERT_TRUE(token.ok());
    auto flush = SliceStore::build_flush_sst(
        *token, {.filesystem = filesystem, .key_path = paths.key, .value_path = paths.value});
    ASSERT_TRUE(flush.ok());
    const auto identity = flush->identity();
    ASSERT_TRUE((*store)->install_flush(*token, *flush).ok());
    const auto manifest = (*store)->persisted_manifest();
    store->reset();

    EXPECT_EQ(SliceStore::Reopen({.locality_groups = {{1, {}}},
                                  .persistence = persistence,
                                  .manifest = manifest,
                                  .orphan_candidates = {identity}})
                  .status()
                  .code(),
              absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(std::filesystem::exists(paths.key));
    EXPECT_TRUE(std::filesystem::exists(paths.value));
}

TEST(SliceStoreTest, ManifestCodecRejectsCorruption) {
    ManifestRef manifest;
    manifest.locality_groups.emplace(1, LocalityGroupManifest{});
    auto encoded = EncodeManifest(manifest);
    ASSERT_TRUE(encoded.ok());
    encoded->back() ^= 1;
    EXPECT_EQ(DecodeManifest(std::as_bytes(std::span(*encoded))).status().code(),
              absl::StatusCode::kDataLoss);
}

TEST(SliceStoreTest, RejectsInvalidPatchSetsAndNonAdvancingIndex) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "k", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> valid = {
        {.locality_group_id = 1, .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(valid, 1).ok());
    EXPECT_EQ((*store)->apply(valid, 1).code(), absl::StatusCode::kInvalidArgument);

    const std::vector<LocalityGroupPatch> duplicate = {
        {.locality_group_id = 1, .mutations = mutations},
        {.locality_group_id = 1, .mutations = mutations},
    };
    EXPECT_EQ((*store)->apply(duplicate, 2).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_FALSE((*store)->read_view().new_cursor(99).ok());
}

} // namespace
} // namespace pl::minitable
