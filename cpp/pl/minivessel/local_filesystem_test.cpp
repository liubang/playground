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

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "cpp/pl/minivessel/local_filesystem.h"
#include "gtest/gtest.h"

namespace pl::minivessel {
namespace {

class LocalFileSystemTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("minivessel-local-fs-" +
                 std::to_string(testing::UnitTest::GetInstance()->random_seed()) + "-" +
                 std::to_string(counter_++));
        ASSERT_TRUE(std::filesystem::create_directories(root_));
        wal_path_ = (root_ / "wal-1.open").string();
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    static std::span<const std::byte> Bytes(std::string_view value) {
        return std::as_bytes(std::span(value.data(), value.size()));
    }

    inline static uint64_t counter_ = 0;
    std::filesystem::path root_;
    std::string wal_path_;
};

TEST_F(LocalFileSystemTest, PublishesOnlySyncedPrefixAndRecoversUnsyncedTail) {
    LocalFileSystem filesystem;
    const auto object_capabilities = filesystem.capabilities();
    EXPECT_TRUE(object_capabilities.has(ObjectStorageFeature::kImmutableObjects));
    const auto log_capabilities = filesystem.active_log_capabilities();
    EXPECT_TRUE(log_capabilities.has(ActiveLogFeature::kDurableAppend));
    EXPECT_TRUE(log_capabilities.has(ActiveLogFeature::kWriterFencing));
    EXPECT_TRUE(log_capabilities.has(ActiveLogFeature::kDurableTail));
    EXPECT_FALSE(log_capabilities.has(ActiveLogFeature::kLeaseRecovery));

    auto writer = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(7),
        .lease_timeout_ms = 60'000,
    });
    ASSERT_TRUE(writer.ok()) << writer.status();
    ASSERT_TRUE(filesystem
                    .append(writer->handle,
                            AppendOptions{.expected_offset = ByteOffset(0),
                                          .packet_sequence = PacketSequence(0)},
                            Bytes("durable"))
                    .ok());
    auto synced = filesystem.sync(writer->handle);
    ASSERT_TRUE(synced.ok()) << synced.status();
    EXPECT_EQ(synced->durable_offset, ByteOffset(7));

    ASSERT_TRUE(filesystem
                    .append(writer->handle,
                            AppendOptions{.expected_offset = ByteOffset(7),
                                          .packet_sequence = PacketSequence(1)},
                            Bytes("-discard"))
                    .ok());
    auto durable_size = filesystem.durable_size(wal_path_);
    ASSERT_TRUE(durable_size.ok());
    EXPECT_EQ(*durable_size, ByteOffset(7));
    std::array<std::byte, 7> durable{};
    ASSERT_TRUE(filesystem.read_durable(wal_path_, ByteOffset(0), durable).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(durable.data()), durable.size()),
              "durable");
    std::array<std::byte, 8> beyond_boundary{};
    EXPECT_FALSE(filesystem.read_durable(wal_path_, ByteOffset(0), beyond_boundary).ok());

    ASSERT_TRUE(filesystem.release_writer(writer->handle).ok());
    auto recovered = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-b",
        .assignment_epoch = AssignmentEpoch(8),
        .lease_timeout_ms = 60'000,
    });
    ASSERT_TRUE(recovered.ok()) << recovered.status();
    EXPECT_GT(recovered->writer_epoch, writer->writer_epoch);
    EXPECT_EQ(recovered->next_offset, ByteOffset(7));
    EXPECT_EQ(std::filesystem::file_size(wal_path_), 7);
    EXPECT_TRUE(filesystem.release_writer(recovered->handle).ok());
}

TEST_F(LocalFileSystemTest, ExpiredWriterCannotBeRenewed) {
    LocalFileSystem filesystem;
    auto writer = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(1),
        .lease_timeout_ms = 10,
    });
    ASSERT_TRUE(writer.ok()) << writer.status();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto renewed = filesystem.renew_writer(writer->handle);
    EXPECT_FALSE(renewed.ok());
    EXPECT_EQ(renewed.status().code(), absl::StatusCode::kAborted);
    EXPECT_TRUE(filesystem.release_writer(writer->handle).ok());
}

TEST_F(LocalFileSystemTest, RejectsConcurrentWriterAndInvalidAppendPosition) {
    LocalFileSystem first;
    LocalFileSystem second;
    auto writer = first.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(1),
        .lease_timeout_ms = 60'000,
    });
    ASSERT_TRUE(writer.ok());

    auto conflict = second.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-b",
        .assignment_epoch = AssignmentEpoch(2),
        .lease_timeout_ms = 60'000,
    });
    EXPECT_FALSE(conflict.ok());
    EXPECT_FALSE(first
                     .append(writer->handle,
                             AppendOptions{.expected_offset = ByteOffset(1),
                                           .packet_sequence = PacketSequence(0)},
                             Bytes("x"))
                     .ok());
    EXPECT_TRUE(first.release_writer(writer->handle).ok());
}

TEST_F(LocalFileSystemTest, RejectsMissingOrCorruptedMetadataWithoutTruncatingWal) {
    {
        std::ofstream wal(wal_path_, std::ios::binary);
        wal << "must-survive";
    }
    const auto original_size = std::filesystem::file_size(wal_path_);
    LocalFileSystem filesystem;
    auto missing = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(1),
        .lease_timeout_ms = 60'000,
    });
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), absl::StatusCode::kDataLoss);
    EXPECT_EQ(std::filesystem::file_size(wal_path_), original_size);

    std::filesystem::remove(wal_path_);
    auto writer = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(1),
        .lease_timeout_ms = 60'000,
    });
    ASSERT_TRUE(writer.ok()) << writer.status();
    ASSERT_TRUE(filesystem.release_writer(writer->handle).ok());
    {
        std::fstream metadata(wal_path_ + ".minivessel.meta",
                              std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(metadata.good());
        metadata.seekp(20);
        metadata.put(static_cast<char>(0x7f));
    }
    auto corrupted = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-b",
        .assignment_epoch = AssignmentEpoch(2),
        .lease_timeout_ms = 60'000,
    });
    EXPECT_FALSE(corrupted.ok());
    EXPECT_EQ(corrupted.status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(LocalFileSystemTest, RejectsAssignmentEpochRollback) {
    LocalFileSystem filesystem;
    auto writer = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(9),
        .lease_timeout_ms = 60'000,
    });
    ASSERT_TRUE(writer.ok()) << writer.status();
    ASSERT_TRUE(filesystem.release_writer(writer->handle).ok());

    auto stale = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-b",
        .assignment_epoch = AssignmentEpoch(8),
        .lease_timeout_ms = 60'000,
    });
    EXPECT_FALSE(stale.ok());
    EXPECT_EQ(stale.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(LocalFileSystemTest, SealedWalCannotBeReopenedForWriting) {
    LocalFileSystem filesystem;
    auto writer = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-a",
        .assignment_epoch = AssignmentEpoch(1),
        .lease_timeout_ms = 60'000,
    });
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(filesystem
                    .append(writer->handle,
                            AppendOptions{.expected_offset = ByteOffset(0),
                                          .packet_sequence = PacketSequence(0)},
                            Bytes("sealed"))
                    .ok());
    ASSERT_TRUE(filesystem.seal(writer->handle).ok());

    auto reopened = filesystem.acquire_writer(AcquireWriterRequest{
        .path = wal_path_,
        .owner_instance_id = "replica-b",
        .assignment_epoch = AssignmentEpoch(2),
        .lease_timeout_ms = 60'000,
    });
    EXPECT_FALSE(reopened.ok());
    auto durable_size = filesystem.durable_size(wal_path_);
    ASSERT_TRUE(durable_size.ok());
    EXPECT_EQ(*durable_size, ByteOffset(6));
}

TEST_F(LocalFileSystemTest, ReusesSstv2ObjectFilesystemForCheckpoints) {
    LocalFileSystem filesystem;
    auto objects = filesystem.object_filesystem();
    ASSERT_NE(objects, nullptr);
    const std::string checkpoint_path = (root_ / "checkpoint.manifest").string();
    auto handle = objects->create(checkpoint_path);
    ASSERT_TRUE(handle.ok()) << handle.status();
    ASSERT_TRUE(objects->append(*handle, Bytes("checkpoint")).ok());
    auto identity = objects->close(*handle);
    ASSERT_TRUE(identity.ok()) << identity.status();
    EXPECT_EQ(identity->length, 10);
    EXPECT_TRUE(identity->checksum_valid);
}

} // namespace
} // namespace pl::minivessel
