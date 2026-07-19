// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minitable/proto/v2/internal.pb.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {
namespace {

namespace internal_proto = ::pl::minitable::proto::v2::internal;
using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Schema;
using sstv2::types::SchemaBuilder;
using sstv2::types::Value;

std::shared_ptr<const codec::CellKeyCodec> MakeCodec() {
    auto schema = SchemaBuilder().add_column("key", DataType::kString).build();
    EXPECT_TRUE(schema.has_value());
    auto codec = codec::CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                             std::make_shared<const Schema>(std::move(*schema)));
    EXPECT_TRUE(codec.ok()) << codec.status();
    return std::make_shared<const codec::CellKeyCodec>(std::move(*codec));
}

std::string EncodeKey(const codec::CellKeyCodec& codec,
                      const RecordTarget& target,
                      OpType op_type,
                      uint32_t mutation_seq) {
    VersionedStorageKey key{
        .storage_key = {.partition = GlobalOrderPrefix{},
                        .row_key = {Value::make<DataType::kString>("row")},
                        .target = target},
        .commit_ts = {.domain_epoch = 7, .counter = 11},
        .mutation_seq = mutation_seq,
        .op_type = op_type,
    };
    auto encoded = codec.EncodeVersionedStorageKey(key);
    EXPECT_TRUE(encoded.ok()) << encoded.status();
    return encoded.ok() ? std::move(*encoded) : std::string{};
}

CommittedSliceMutation MakeMutation(const codec::CellKeyCodec& codec) {
    return {
        .apply_index = 99,
        .identity = {.client_id = "client", .request_id = "request", .payload_hash = 123},
        .commit_ts = {.domain_epoch = 7, .counter = 11},
        .commit_physical_ms = 1000,
        .locality_group_mutations =
            {
                {{.encoded_key =
                      EncodeKey(codec,
                                CellRef{.column_family_id = 1, .qualifier = StaticQualifier{2}},
                                OpType::kPut,
                                0),
                  .encoded_value = "value"},
                 {.encoded_key = EncodeKey(codec, RowTombstone{}, OpType::kDelete, 1),
                  .encoded_value = {}}},
                {{.encoded_key = EncodeKey(
                      codec, ColumnFamilyTombstone{.column_family_id = 3}, OpType::kDelete, 2),
                  .encoded_value = {}}},
            },
        .locality_group_ids = {1, 2},
        .serialized_response = "response",
    };
}

std::string Serialize(const internal_proto::SliceMutation& mutation) {
    std::string encoded;
    EXPECT_TRUE(mutation.SerializeToString(&encoded));
    return encoded;
}

internal_proto::SliceMutation ParseValidMutation(const codec::CellKeyCodec& codec) {
    auto row = codec.EncodeLogicalRowKey({Value::make<DataType::kString>("row")});
    EXPECT_TRUE(row.ok()) << row.status();
    auto encoded = EncodeSliceMutationV2(MakeMutation(codec), *row, codec);
    EXPECT_TRUE(encoded.ok()) << encoded.status();
    internal_proto::SliceMutation proto;
    EXPECT_TRUE(proto.ParseFromString(*encoded));
    return proto;
}

absl::Status DecodeStatus(const std::string& encoded, const codec::CellKeyCodec& codec) {
    return DecodeSliceMutationV2(
               std::as_bytes(std::span(encoded.data(), encoded.size())), 99, codec)
        .status();
}

TEST(SliceMutationCodecTest, RoundTripsCanonicalRowTransaction) {
    const auto codec = MakeCodec();
    const auto original = MakeMutation(*codec);
    auto row = codec->EncodeLogicalRowKey({Value::make<DataType::kString>("row")});
    ASSERT_TRUE(row.ok()) << row.status();

    auto encoded = EncodeSliceMutationV2(original, *row, *codec);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    auto decoded = DecodeSliceMutationV2(
        std::as_bytes(std::span(encoded->data(), encoded->size())), 42, *codec);
    ASSERT_TRUE(decoded.ok()) << decoded.status();

    EXPECT_EQ(decoded->apply_index, 42U);
    EXPECT_EQ(decoded->identity, original.identity);
    EXPECT_EQ(decoded->commit_ts, original.commit_ts);
    EXPECT_EQ(decoded->commit_physical_ms, original.commit_physical_ms);
    EXPECT_EQ(decoded->locality_group_ids, original.locality_group_ids);
    EXPECT_EQ(decoded->locality_group_mutations, original.locality_group_mutations);
    EXPECT_EQ(decoded->serialized_response, original.serialized_response);
}

TEST(SliceMutationCodecTest, RejectsMalformedWirePayloadAndZeroApplyIndex) {
    const auto codec = MakeCodec();
    const std::string malformed(1, static_cast<char>(0x80));
    EXPECT_EQ(DecodeStatus(malformed, *codec).code(), absl::StatusCode::kDataLoss);

    const auto valid = Serialize(ParseValidMutation(*codec));
    EXPECT_EQ(DecodeSliceMutationV2(std::as_bytes(std::span(valid.data(), valid.size())), 0, *codec)
                  .status()
                  .code(),
              absl::StatusCode::kInvalidArgument);
}

TEST(SliceMutationCodecTest, RejectsIncompleteIdentityAndCommitMetadata) {
    const auto codec = MakeCodec();
    auto proto = ParseValidMutation(*codec);

    proto.mutable_identity()->clear_client_id();
    EXPECT_EQ(DecodeStatus(Serialize(proto), *codec).code(), absl::StatusCode::kInvalidArgument);

    proto = ParseValidMutation(*codec);
    proto.mutable_row_transaction()->clear_commit_ts();
    EXPECT_EQ(DecodeStatus(Serialize(proto), *codec).code(), absl::StatusCode::kInvalidArgument);

    proto = ParseValidMutation(*codec);
    proto.mutable_row_transaction()->set_commit_physical_ms(0);
    EXPECT_EQ(DecodeStatus(Serialize(proto), *codec).code(), absl::StatusCode::kInvalidArgument);
}

TEST(SliceMutationCodecTest, RejectsInvalidCanonicalPatchStructure) {
    const auto codec = MakeCodec();
    auto proto = ParseValidMutation(*codec);
    proto.mutable_row_transaction()->add_locality_groups()->CopyFrom(
        proto.row_transaction().locality_groups(0));
    EXPECT_EQ(DecodeStatus(Serialize(proto), *codec).code(), absl::StatusCode::kInvalidArgument);

    proto = ParseValidMutation(*codec);
    proto.mutable_row_transaction()->mutable_locality_groups(0)->add_mutations()->CopyFrom(
        proto.row_transaction().locality_groups(0).mutations(0));
    EXPECT_EQ(DecodeStatus(Serialize(proto), *codec).code(), absl::StatusCode::kInvalidArgument);

    proto = ParseValidMutation(*codec);
    proto.mutable_row_transaction()
        ->mutable_locality_groups(0)
        ->mutable_mutations(0)
        ->clear_operation();
    EXPECT_EQ(DecodeStatus(Serialize(proto), *codec).code(), absl::StatusCode::kInvalidArgument);
}

TEST(SliceMutationCodecTest, EncoderRejectsIncompletePatchesAndUnsupportedMerge) {
    const auto codec = MakeCodec();
    auto row = codec->EncodeLogicalRowKey({Value::make<DataType::kString>("row")});
    ASSERT_TRUE(row.ok()) << row.status();

    auto mutation = MakeMutation(*codec);
    mutation.identity.client_id.clear();
    EXPECT_EQ(EncodeSliceMutationV2(mutation, *row, *codec).status().code(),
              absl::StatusCode::kInvalidArgument);

    mutation = MakeMutation(*codec);
    mutation.locality_group_ids = {1, 1};
    EXPECT_EQ(EncodeSliceMutationV2(mutation, *row, *codec).status().code(),
              absl::StatusCode::kInvalidArgument);

    mutation = MakeMutation(*codec);
    mutation.locality_group_mutations[0].clear();
    EXPECT_EQ(EncodeSliceMutationV2(mutation, *row, *codec).status().code(),
              absl::StatusCode::kInvalidArgument);

    mutation = MakeMutation(*codec);
    mutation.locality_group_mutations = {
        {{.encoded_key = EncodeKey(*codec,
                                   CellRef{.column_family_id = 1, .qualifier = StaticQualifier{2}},
                                   OpType::kMerge,
                                   0),
          .encoded_value = "operand"}}};
    mutation.locality_group_ids = {1};
    EXPECT_EQ(EncodeSliceMutationV2(mutation, *row, *codec).status().code(),
              absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace pl::minitable
