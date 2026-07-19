// Copyright (c) 2026 The Authors. All rights reserved.

#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/minivessel/local_filesystem.h"
#include "cpp/pl/minivessel/shared_wal.h"

namespace pl::minivessel {
namespace {

class FramedSharedWalTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("minivessel-shared-wal-" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    FramedSharedWal MakeWal(LocalFileSystem* filesystem) const {
        return FramedSharedWal(filesystem,
                               FramedSharedWalOptions{
                                   .group = {.group_id = "g", .incarnation = GroupIncarnation(1)},
                                   .path = (root_ / "active.wal").string(),
                               });
    }

    std::filesystem::path root_;
};

TEST_F(FramedSharedWalTest, DurableRecordsSurviveWriterTakeover) {
    LocalFileSystem filesystem;
    {
        auto wal = MakeWal(&filesystem);
        auto lease = wal.acquire_writer("primary-a", AssignmentEpoch(1), 30'000);
        ASSERT_TRUE(lease.ok()) << lease.status();
        const std::vector<std::byte> payload{std::byte{1}, std::byte{2}};
        auto first = wal.append(LogRecordType::kMutation, "request-1", payload);
        ASSERT_TRUE(first.ok()) << first.status();
        EXPECT_EQ(first->record.lrsn, Lrsn(1));
        ASSERT_TRUE(wal.release_writer().ok());
    }

    auto wal = MakeWal(&filesystem);
    auto lease = wal.acquire_writer("primary-b", AssignmentEpoch(2), 30'000);
    ASSERT_TRUE(lease.ok()) << lease.status();
    EXPECT_EQ(lease->writer_epoch, WriterEpoch(2));
    auto barrier = wal.append(LogRecordType::kPrimaryBarrier, {}, {});
    ASSERT_TRUE(barrier.ok()) << barrier.status();
    EXPECT_EQ(barrier->record.lrsn, Lrsn(2));

    auto records = wal.read(Lrsn(1), 10);
    ASSERT_TRUE(records.ok()) << records.status();
    ASSERT_EQ(records->size(), 2);
    EXPECT_EQ((*records)[0].request_id, "request-1");
    EXPECT_EQ((*records)[1].type, LogRecordType::kPrimaryBarrier);
}

TEST_F(FramedSharedWalTest, ReadsOnlyRequestedDurableSuffix) {
    LocalFileSystem filesystem;
    auto wal = MakeWal(&filesystem);
    ASSERT_TRUE(wal.acquire_writer("primary", AssignmentEpoch(1), 30'000).ok());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(wal.append(LogRecordType::kMutation, std::to_string(i), {}).ok());
    }
    auto records = wal.read(Lrsn(2), 1);
    ASSERT_TRUE(records.ok()) << records.status();
    ASSERT_EQ(records->size(), 1);
    EXPECT_EQ(records->front().lrsn, Lrsn(2));
    EXPECT_EQ(*wal.durable_lrsn(), Lrsn(3));
}

} // namespace
} // namespace pl::minivessel
