// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "cpp/pl/minitable/core/data_service_core.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::minitable {
namespace {

namespace pb = ::pl::minitable::proto::v2;
using sstv2::types::DataType;
using sstv2::types::Schema;
using sstv2::types::SchemaBuilder;

std::shared_ptr<const codec::CellKeyCodec> MakeCodec() {
    auto schema = SchemaBuilder().add_column("key", DataType::kString).build();
    EXPECT_TRUE(schema.has_value());
    auto codec = codec::CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                             std::make_shared<const Schema>(std::move(*schema)));
    EXPECT_TRUE(codec.ok()) << codec.status();
    return std::make_shared<const codec::CellKeyCodec>(std::move(*codec));
}

void FillHeader(pb::DataRequestHeader* header, std::string request_id = "request") {
    header->mutable_context()->set_client_id("client");
    header->mutable_context()->set_request_id(std::move(request_id));
    header->set_table_id(10);
    header->set_slice_id(20);
    header->set_schema_version(30);
    header->set_route_epoch(40);
}

void FillRow(pb::RowKey* row, std::string value = "row") {
    row->add_values()->set_string_value(std::move(value));
}

pb::CellRef Cell(uint32_t column_family_id = 1, uint32_t column_id = 2) {
    pb::CellRef result;
    result.set_column_family_id(column_family_id);
    result.mutable_static_qualifier()->set_column_id(column_id);
    return result;
}

pb::PutRequest Put(std::string value = "value", std::string request_id = "put") {
    pb::PutRequest request;
    FillHeader(request.mutable_header(), std::move(request_id));
    FillRow(request.mutable_row_key());
    auto* mutation = request.add_mutations();
    *mutation->mutable_cell() = Cell();
    mutation->set_type(pb::SET);
    mutation->mutable_value()->set_string_value(std::move(value));
    return request;
}

pb::DeleteRequest DeleteRow(std::string request_id = "delete") {
    pb::DeleteRequest request;
    FillHeader(request.mutable_header(), std::move(request_id));
    FillRow(request.mutable_row_key());
    request.add_targets()->set_row(true);
    return request;
}

std::unique_ptr<SliceApplyMachine> MakeMachine() {
    auto store = SliceStore::Create({{1, {}}});
    EXPECT_TRUE(store.ok()) << store.status();
    auto machine = SliceApplyMachine::Create(std::move(*store));
    EXPECT_TRUE(machine.ok()) << machine.status();
    return machine.ok() ? std::move(*machine) : nullptr;
}

ApplyResult ApplyPrepared(const PreparedDataMutation& prepared,
                          uint64_t index,
                          const codec::CellKeyCodec& codec,
                          SliceApplyMachine* machine) {
    auto result = machine->apply_serialized(
        std::as_bytes(std::span(prepared.encoded_entry.data(), prepared.encoded_entry.size())),
        index,
        codec);
    EXPECT_TRUE(result.ok()) << result.status();
    return result.ok() ? std::move(*result) : ApplyResult{};
}

pb::GetRequest Get() {
    pb::GetRequest request;
    FillHeader(request.mutable_header(), "get");
    FillRow(request.mutable_row_key());
    request.mutable_read_policy()->set_consistency(pb::STRONG);
    request.set_max_versions(1);
    return request;
}

TEST(DataServiceCoreTest, PutRoundTripsThroughReplicatedApplyAndGet) {
    const auto codec = MakeCodec();
    auto machine = MakeMachine();
    auto prepared = PreparePutV2(Put(), {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec);
    ASSERT_TRUE(prepared.ok()) << prepared.status();

    const auto applied = ApplyPrepared(*prepared, 1, *codec, machine.get());
    EXPECT_FALSE(applied.duplicate);
    EXPECT_EQ(applied.serialized_response, prepared->serialized_response);

    auto row = ExecuteGetV2(Get(), {.domain_epoch = 1, .counter = 1}, 1, *codec, *machine);
    ASSERT_TRUE(row.ok()) << row.status();
    ASSERT_TRUE(row->has_value());
    ASSERT_EQ((*row)->cells_size(), 1);
    EXPECT_EQ((*row)->row_key().values(0).string_value(), "row");
    EXPECT_EQ((*row)->cells(0).value().string_value(), "value");
    EXPECT_EQ((*row)->cells(0).commit_ts().counter(), 1U);
}

TEST(DataServiceCoreTest, RetryWithSamePayloadKeepsIdentityAcrossTimestampAllocation) {
    const auto codec = MakeCodec();
    auto first = PreparePutV2(Put(), {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec);
    auto retry_request = Put();
    retry_request.mutable_header()->mutable_context()->set_deadline_unix_ms(9999);
    retry_request.mutable_header()->mutable_context()->set_trace_id("another-attempt");
    auto retry = PreparePutV2(retry_request, {{.domain_epoch = 1, .counter = 2}, 1001}, 1, *codec);
    ASSERT_TRUE(first.ok() && retry.ok());

    auto decoded_first = DecodeSliceMutationV2(
        std::as_bytes(std::span(first->encoded_entry.data(), first->encoded_entry.size())),
        1,
        *codec);
    auto decoded_retry = DecodeSliceMutationV2(
        std::as_bytes(std::span(retry->encoded_entry.data(), retry->encoded_entry.size())),
        2,
        *codec);
    ASSERT_TRUE(decoded_first.ok() && decoded_retry.ok());
    EXPECT_EQ(decoded_first->identity, decoded_retry->identity);
}

TEST(DataServiceCoreTest, DifferentPayloadWithSameRequestIdTriggersDedupeConflict) {
    const auto codec = MakeCodec();
    auto machine = MakeMachine();
    auto first = PreparePutV2(Put("one"), {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec);
    auto conflict = PreparePutV2(Put("two"), {{.domain_epoch = 1, .counter = 2}, 1001}, 1, *codec);
    ASSERT_TRUE(first.ok() && conflict.ok());
    ApplyPrepared(*first, 1, *codec, machine.get());

    auto result = machine->apply_serialized(
        std::as_bytes(std::span(conflict->encoded_entry.data(), conflict->encoded_entry.size())),
        2,
        *codec);
    EXPECT_EQ(result.status().code(), absl::StatusCode::kAlreadyExists);
}

TEST(DataServiceCoreTest, DeleteVariantsProduceCanonicalTombstones) {
    const auto codec = MakeCodec();
    auto request = DeleteRow();
    auto row = PrepareDeleteV2(request, {{.domain_epoch = 1, .counter = 2}, 1001}, 1, *codec);
    ASSERT_TRUE(row.ok()) << row.status();
    auto decoded = DecodeSliceMutationV2(
        std::as_bytes(std::span(row->encoded_entry.data(), row->encoded_entry.size())), 2, *codec);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    auto key =
        codec->DecodeVersionedStorageKey(decoded->locality_group_mutations[0][0].encoded_key);
    ASSERT_TRUE(key.ok());
    EXPECT_TRUE(std::holds_alternative<RowTombstone>(key->storage_key.target));

    request.clear_targets();
    request.add_targets()->set_column_family_id(3);
    auto family = PrepareDeleteV2(request, {{.domain_epoch = 1, .counter = 3}, 1002}, 1, *codec);
    ASSERT_TRUE(family.ok()) << family.status();
    decoded = DecodeSliceMutationV2(
        std::as_bytes(std::span(family->encoded_entry.data(), family->encoded_entry.size())),
        3,
        *codec);
    key = codec->DecodeVersionedStorageKey(decoded->locality_group_mutations[0][0].encoded_key);
    ASSERT_TRUE(key.ok());
    EXPECT_TRUE(std::holds_alternative<ColumnFamilyTombstone>(key->storage_key.target));

    request.clear_targets();
    *request.add_targets()->mutable_cell() = Cell();
    auto cell = PrepareDeleteV2(request, {{.domain_epoch = 1, .counter = 4}, 1003}, 1, *codec);
    ASSERT_TRUE(cell.ok()) << cell.status();
    decoded = DecodeSliceMutationV2(
        std::as_bytes(std::span(cell->encoded_entry.data(), cell->encoded_entry.size())),
        4,
        *codec);
    key = codec->DecodeVersionedStorageKey(decoded->locality_group_mutations[0][0].encoded_key);
    ASSERT_TRUE(key.ok());
    EXPECT_TRUE(std::holds_alternative<CellRef>(key->storage_key.target));
}

TEST(DataServiceCoreTest, RowDeleteHidesEarlierPut) {
    const auto codec = MakeCodec();
    auto machine = MakeMachine();
    auto put = PreparePutV2(Put(), {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec);
    auto remove =
        PrepareDeleteV2(DeleteRow(), {{.domain_epoch = 1, .counter = 2}, 1001}, 1, *codec);
    ASSERT_TRUE(put.ok() && remove.ok());
    ApplyPrepared(*put, 1, *codec, machine.get());
    ApplyPrepared(*remove, 2, *codec, machine.get());

    auto row = ExecuteGetV2(Get(), {.domain_epoch = 1, .counter = 2}, 1, *codec, *machine);
    ASSERT_TRUE(row.ok());
    EXPECT_FALSE(row->has_value());
}

TEST(DataServiceCoreTest, RejectsUnsupportedOrIncompleteRequests) {
    const auto codec = MakeCodec();
    auto put = Put();
    put.mutable_header()->mutable_context()->clear_request_id();
    EXPECT_EQ(
        PreparePutV2(put, {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec).status().code(),
        absl::StatusCode::kInvalidArgument);

    put = Put();
    put.mutable_mutations(0)->set_type(pb::SET_NULL);
    EXPECT_EQ(
        PreparePutV2(put, {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec).status().code(),
        absl::StatusCode::kUnimplemented);

    auto remove = DeleteRow();
    remove.add_targets()->set_column_family_id(1);
    EXPECT_EQ(PrepareDeleteV2(remove, {{.domain_epoch = 1, .counter = 1}, 1000}, 1, *codec)
                  .status()
                  .code(),
              absl::StatusCode::kInvalidArgument);

    auto get = Get();
    get.mutable_read_policy()->set_consistency(pb::BOUNDED_STALENESS);
    auto machine = MakeMachine();
    EXPECT_EQ(
        ExecuteGetV2(get, {.domain_epoch = 1, .counter = 1}, 1, *codec, *machine).status().code(),
        absl::StatusCode::kUnimplemented);
}

} // namespace
} // namespace pl::minitable
