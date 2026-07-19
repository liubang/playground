// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "cpp/pl/minitable/core/embedded_slice.h"
#include "cpp/pl/sstv2/io/local_filesystem.h"
#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "gtest/gtest.h"

namespace pl::minitable {
namespace {

using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Value;

codec::CellKeyCodec MakeCodec(DataType type = DataType::kUint64,
                              sstv2::types::SortOrder order = sstv2::types::SortOrder::kAscending) {
    auto schema = sstv2::types::SchemaBuilder().add_column("row", type, order).build();
    EXPECT_TRUE(schema.has_value());
    auto codec = codec::CellKeyCodec::Create(
        {}, std::make_shared<const sstv2::types::Schema>(std::move(*schema)));
    EXPECT_TRUE(codec.ok()) << codec.status();
    return std::move(*codec);
}

std::vector<Value> Row(uint64_t id) {
    return {Value::make<DataType::kUint64>(id)};
}

CellRef Cell(uint32_t cf, uint32_t column) {
    return {.column_family_id = cf, .qualifier = StaticQualifier{.column_id = column}};
}

CellMutation Put(CellRef ref, std::string value) {
    return {.target = std::move(ref),
            .op_type = OpType::kPut,
            .value = Value::make<DataType::kString>(std::move(value))};
}

CellMutation DeleteCell(CellRef ref) {
    return {.target = std::move(ref), .op_type = OpType::kDelete};
}

std::optional<std::string> ReadCell(const EmbeddedSlice& slice,
                                    uint64_t row,
                                    CellRef ref,
                                    uint64_t counter) {
    auto result = slice.get(Row(row), {.domain_epoch = 7, .counter = counter});
    EXPECT_TRUE(result.ok()) << result.status();
    if (!result.ok() || !result->has_value()) {
        return std::nullopt;
    }
    for (const auto& cell : result->value().cells) {
        if (cell.ref == ref) {
            return std::string(cell.value.as_string());
        }
    }
    return std::nullopt;
}

EmbeddedSliceOptions PersistentOptions(std::string_view suffix) {
    const auto directory =
        (std::filesystem::path(::testing::TempDir()) / ("minitable-mvcc-" + std::string(suffix)))
            .string();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return {.timestamp_domain_epoch = 7,
            .persistence = {.filesystem = std::make_shared<sstv2::io::LocalFileSystem>(),
                            .manifest_directory = directory}};
}

struct RefVersion {
    uint64_t timestamp = 0;
    bool deleted = false;
    std::string value;
};

class ReferenceModel {
public:
    void Apply(uint64_t row, uint32_t column, uint64_t timestamp, bool deleted, std::string value) {
        versions_[{row, column}].push_back(
            {.timestamp = timestamp, .deleted = deleted, .value = std::move(value)});
    }

    std::optional<std::string> Get(uint64_t row, uint32_t column, uint64_t read_ts) const {
        const auto it = versions_.find({row, column});
        if (it == versions_.end()) {
            return std::nullopt;
        }
        for (auto version = it->second.rbegin(); version != it->second.rend(); ++version) {
            if (version->timestamp <= read_ts) {
                return version->deleted ? std::nullopt : std::optional(version->value);
            }
        }
        return std::nullopt;
    }

private:
    std::map<std::pair<uint64_t, uint32_t>, std::vector<RefVersion>> versions_;
};

TEST(EmbeddedSliceTest, SnapshotReadsPreserveNullEmptyAndDeleteSemantics) {
    auto slice = EmbeddedSlice::Create(MakeCodec(), {.timestamp_domain_epoch = 7});
    ASSERT_TRUE(slice.ok()) << slice.status();
    const auto first = (*slice)->mutate(
        Row(1), std::vector<CellMutation>{Put(Cell(1, 1), "old"), Put(Cell(1, 2), "")});
    ASSERT_TRUE(first.ok()) << first.status();
    const auto second = (*slice)->mutate(
        Row(1), std::vector<CellMutation>{DeleteCell(Cell(1, 1)), Put(Cell(1, 3), "new")});
    ASSERT_TRUE(second.ok()) << second.status();

    EXPECT_EQ(ReadCell(**slice, 1, Cell(1, 1), first->counter), "old");
    EXPECT_EQ(ReadCell(**slice, 1, Cell(1, 2), second->counter), "");
    EXPECT_EQ(ReadCell(**slice, 1, Cell(1, 1), second->counter), std::nullopt);
    EXPECT_EQ(ReadCell(**slice, 1, Cell(1, 3), second->counter), "new");

    const CellMutation null_put{.target = Cell(1, 4), .op_type = OpType::kPut, .value = Value{}};
    const auto third = (*slice)->mutate(Row(1), std::span<const CellMutation>(&null_put, 1));
    ASSERT_TRUE(third.ok()) << third.status();
    auto row = (*slice)->get(Row(1), *third);
    ASSERT_TRUE(row.ok() && row->has_value());
    ASSERT_EQ(row->value().cells.size(), 3U);
    EXPECT_EQ(row->value().cells.back().value.type(), DataType::kNone);
}

TEST(EmbeddedSliceTest, RowAndFamilyTombstonesRespectMutationSequenceAndRecreate) {
    auto slice = EmbeddedSlice::Create(MakeCodec(), {.timestamp_domain_epoch = 7});
    ASSERT_TRUE(slice.ok());
    ASSERT_TRUE(
        (*slice)
            ->mutate(Row(9), std::vector<CellMutation>{Put(Cell(1, 1), "a"), Put(Cell(2, 1), "b")})
            .ok());
    auto family_delete = (*slice)->mutate(
        Row(9),
        std::vector<CellMutation>{
            {.target = ColumnFamilyTombstone{.column_family_id = 1}, .op_type = OpType::kDelete},
            Put(Cell(1, 1), "after-family-delete")});
    ASSERT_TRUE(family_delete.ok());
    EXPECT_EQ(ReadCell(**slice, 9, Cell(1, 1), family_delete->counter), "after-family-delete");
    EXPECT_EQ(ReadCell(**slice, 9, Cell(2, 1), family_delete->counter), "b");

    auto row_delete = (*slice)->mutate(
        Row(9),
        std::vector<CellMutation>{{.target = RowTombstone{}, .op_type = OpType::kDelete},
                                  Put(Cell(2, 1), "after-row-delete")});
    ASSERT_TRUE(row_delete.ok());
    EXPECT_EQ(ReadCell(**slice, 9, Cell(1, 1), row_delete->counter), std::nullopt);
    EXPECT_EQ(ReadCell(**slice, 9, Cell(2, 1), row_delete->counter), "after-row-delete");
}

TEST(EmbeddedSliceTest, ScanIsOrderedBoundedAndRejectsForeignTimestampDomain) {
    auto slice = EmbeddedSlice::Create(MakeCodec(), {.timestamp_domain_epoch = 7});
    ASSERT_TRUE(slice.ok());
    for (uint64_t row : {3, 1, 2}) {
        ASSERT_TRUE(
            (*slice)
                ->mutate(Row(row), std::vector<CellMutation>{Put(Cell(1, 1), std::to_string(row))})
                .ok());
    }
    auto empty = (*slice)->scan(Row(2), Row(2), {.domain_epoch = 7, .counter = 3});
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty->empty());
    auto rows = (*slice)->scan(Row(2), Row(4), {.domain_epoch = 7, .counter = 3});
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ(rows->at(0).row_key, Row(2));
    EXPECT_EQ(rows->at(1).row_key, Row(3));
    EXPECT_EQ((*slice)
                  ->scan(std::nullopt, std::nullopt, {.domain_epoch = 8, .counter = 3})
                  .status()
                  .code(),
              absl::StatusCode::kInvalidArgument);
}

TEST(EmbeddedSliceTest, FlushAndReopenPreserveMvccAndTimestampHighWatermark) {
    auto options = PersistentOptions("reopen");
    auto slice = EmbeddedSlice::Create(MakeCodec(), options);
    ASSERT_TRUE(slice.ok()) << slice.status();
    auto old = (*slice)->mutate(Row(1), std::vector<CellMutation>{Put(Cell(1, 1), "old")});
    auto current = (*slice)->mutate(Row(1), std::vector<CellMutation>{Put(Cell(1, 1), "new")});
    ASSERT_TRUE(old.ok() && current.ok());
    ASSERT_TRUE((*slice)->freeze().ok());
    const auto key_path = options.persistence.manifest_directory + "/data-1.sst";
    const auto value_path = options.persistence.manifest_directory + "/value-1.sst";
    ASSERT_TRUE((*slice)
                    ->flush({.filesystem = options.persistence.filesystem,
                             .key_path = key_path,
                             .value_path = value_path})
                    .ok());
    const PersistedManifest manifest = (*slice)->persisted_manifest();

    auto reopened = EmbeddedSlice::Reopen(MakeCodec(), {.options = options, .manifest = manifest});
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    EXPECT_EQ(ReadCell(**reopened, 1, Cell(1, 1), old->counter), "old");
    EXPECT_EQ(ReadCell(**reopened, 1, Cell(1, 1), current->counter), "new");
    EXPECT_EQ((*reopened)->last_timestamp_counter(), current->counter);
    EXPECT_EQ((*reopened)->storage_for_testing().read_view().manifest().timestamp_high_watermark,
              current->counter);
    auto next = (*reopened)->mutate(Row(2), std::vector<CellMutation>{Put(Cell(1, 1), "next")});
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next->counter, current->counter + 1);
}

TEST(EmbeddedSliceTest, RandomizedReferenceModelAcrossFlushBoundary) {
    auto options = PersistentOptions("random");
    auto slice = EmbeddedSlice::Create(MakeCodec(), options);
    ASSERT_TRUE(slice.ok());
    ReferenceModel model;
    std::mt19937_64 random(0x5eed);
    uint64_t timestamp = 0;
    for (size_t step = 0; step < 300; ++step) {
        const uint64_t row = random() % 8;
        const uint32_t column = static_cast<uint32_t>(random() % 4 + 1);
        const bool deleted = random() % 5 == 0;
        const std::string value = "v-" + std::to_string(step);
        const auto mutation = deleted ? DeleteCell(Cell(1, column)) : Put(Cell(1, column), value);
        auto result = (*slice)->mutate(Row(row), std::span<const CellMutation>(&mutation, 1));
        ASSERT_TRUE(result.ok()) << result.status();
        timestamp = result->counter;
        model.Apply(row, column, timestamp, deleted, value);

        if (step == 149) {
            ASSERT_TRUE((*slice)->freeze().ok());
            ASSERT_TRUE(
                (*slice)
                    ->flush({.filesystem = options.persistence.filesystem,
                             .key_path = options.persistence.manifest_directory + "/random.sst",
                             .value_path = options.persistence.manifest_directory + "/random.val"})
                    .ok());
        }
        for (size_t probe = 0; probe < 3; ++probe) {
            const uint64_t probe_row = random() % 8;
            const uint32_t probe_column = static_cast<uint32_t>(random() % 4 + 1);
            const uint64_t read_ts = random() % (timestamp + 1);
            EXPECT_EQ(ReadCell(**slice, probe_row, Cell(1, probe_column), read_ts),
                      model.Get(probe_row, probe_column, read_ts));
        }
    }
}

TEST(EmbeddedSliceTest, ConcurrentMutationsAllocateUniqueOrderedTimestamps) {
    auto slice = EmbeddedSlice::Create(MakeCodec(), {.timestamp_domain_epoch = 7});
    ASSERT_TRUE(slice.ok());
    constexpr size_t kWriters = 8;
    constexpr size_t kWritesPerWriter = 40;
    std::vector<std::thread> writers;
    std::vector<std::vector<uint64_t>> timestamps(kWriters);
    writers.reserve(kWriters);
    for (size_t writer = 0; writer < kWriters; ++writer) {
        writers.emplace_back([&, writer] {
            timestamps[writer].reserve(kWritesPerWriter);
            for (size_t write = 0; write < kWritesPerWriter; ++write) {
                auto result = (*slice)->mutate(
                    Row(writer), std::vector<CellMutation>{Put(Cell(1, 1), std::to_string(write))});
                ASSERT_TRUE(result.ok()) << result.status();
                timestamps[writer].push_back(result->counter);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    std::vector<uint64_t> all;
    for (const auto& values : timestamps) {
        all.insert(all.end(), values.begin(), values.end());
    }
    std::ranges::sort(all);
    ASSERT_EQ(all.size(), kWriters * kWritesPerWriter);
    for (size_t index = 0; index < all.size(); ++index) {
        EXPECT_EQ(all[index], index + 1);
    }
}

TEST(EmbeddedSliceTest, TombstoneOnlyGetDoesNotScanIntoNextRow) {
    auto slice = EmbeddedSlice::Create(MakeCodec(), {.timestamp_domain_epoch = 7});
    ASSERT_TRUE(slice.ok());
    ASSERT_TRUE((*slice)->mutate(Row(1), std::vector<CellMutation>{Put(Cell(1, 1), "gone")}).ok());
    ASSERT_TRUE((*slice)->mutate(Row(1), std::vector<CellMutation>{DeleteCell(Cell(1, 1))}).ok());
    auto next = (*slice)->mutate(Row(2), std::vector<CellMutation>{Put(Cell(1, 1), "next")});
    ASSERT_TRUE(next.ok());
    auto result = (*slice)->get(Row(1), *next);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_FALSE(result->has_value());
}

TEST(EmbeddedSliceTest, ReopenRejectsDifferentRowKeySchemaAndTimestampDomain) {
    auto options = PersistentOptions("domain-mismatch");
    auto slice = EmbeddedSlice::Create(MakeCodec(), options);
    ASSERT_TRUE(slice.ok());
    ASSERT_TRUE((*slice)->mutate(Row(1), std::vector<CellMutation>{Put(Cell(1, 1), "v")}).ok());
    ASSERT_TRUE((*slice)->freeze().ok());
    ASSERT_TRUE((*slice)
                    ->flush({.filesystem = options.persistence.filesystem,
                             .key_path = options.persistence.manifest_directory + "/domain.sst",
                             .value_path = options.persistence.manifest_directory + "/domain.val"})
                    .ok());
    const auto manifest = (*slice)->persisted_manifest();

    EXPECT_EQ(EmbeddedSlice::Reopen(MakeCodec(DataType::kInt64),
                                    {.options = options, .manifest = manifest})
                  .status()
                  .code(),
              absl::StatusCode::kFailedPrecondition);
    ++options.timestamp_domain_epoch;
    EXPECT_EQ(EmbeddedSlice::Reopen(MakeCodec(), {.options = options, .manifest = manifest})
                  .status()
                  .code(),
              absl::StatusCode::kFailedPrecondition);
}

TEST(EmbeddedSliceTest, ScanRejectsOversizedRowWithoutReturningPartialData) {
    auto slice = EmbeddedSlice::Create(
        MakeCodec(), {.timestamp_domain_epoch = 7, .max_row_aggregate_bytes = 32});
    ASSERT_TRUE(slice.ok());
    ASSERT_TRUE(
        (*slice)
            ->mutate(Row(1), std::vector<CellMutation>{Put(Cell(1, 1), std::string(64, 'x'))})
            .ok());
    EXPECT_EQ((*slice)
                  ->scan(std::nullopt, std::nullopt, {.domain_epoch = 7, .counter = 1})
                  .status()
                  .code(),
              absl::StatusCode::kResourceExhausted);
}

TEST(EmbeddedSliceTest, RejectsInvalidMutationShapesWithoutAdvancingTimestamp) {
    auto slice = EmbeddedSlice::Create(MakeCodec(), {.timestamp_domain_epoch = 7});
    ASSERT_TRUE(slice.ok());
    const CellMutation invalid_put{.target = RowTombstone{},
                                   .op_type = OpType::kPut,
                                   .value = Value::make<DataType::kString>("bad")};
    EXPECT_EQ(
        (*slice)->mutate(Row(1), std::span<const CellMutation>(&invalid_put, 1)).status().code(),
        absl::StatusCode::kInvalidArgument);
    const CellMutation invalid_delete{.target = Cell(1, 1),
                                      .op_type = OpType::kDelete,
                                      .value = Value::make<DataType::kString>("bad")};
    EXPECT_EQ(
        (*slice)->mutate(Row(1), std::span<const CellMutation>(&invalid_delete, 1)).status().code(),
        absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*slice)->last_timestamp_counter(), 0U);
}

} // namespace
} // namespace pl::minitable
