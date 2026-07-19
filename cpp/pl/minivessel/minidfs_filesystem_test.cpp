// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0

#include <type_traits>

#include "cpp/pl/minivessel/minidfs_filesystem.h"
#include "cpp/pl/minivessel/shared_wal.h"
#include "gtest/gtest.h"

namespace pl::minivessel {
namespace {

TEST(MiniDfsFileSystemTest, DoesNotPretendObjectStorageIsAnActiveWal) {
    static_assert(std::is_base_of_v<VesselFileSystem, MiniDfsFileSystem>);
    static_assert(!std::is_base_of_v<ActiveLogStorage, MiniDfsFileSystem>);

    MiniDfsFileSystem filesystem(nullptr);
    EXPECT_TRUE(filesystem.capabilities().has(ObjectStorageFeature::kImmutableObjects));
    EXPECT_NE(filesystem.object_filesystem(), nullptr);
    EXPECT_EQ(filesystem.active_log_capabilities().bits(), 0U);
    EXPECT_EQ(filesystem.active_log_storage(), nullptr);
    EXPECT_TRUE(validate_vessel_filesystem(filesystem).ok());
    EXPECT_FALSE(
        validate_vessel_filesystem(filesystem, kFramedSharedWalActiveLogCapabilities).ok());

    FramedSharedWal wal(nullptr,
                        {.group = {.group_id = "missing", .incarnation = GroupIncarnation(1)},
                         .path = "/missing"});
    EXPECT_EQ(wal.acquire_writer("replica", AssignmentEpoch(1), 1000).status().code(),
              absl::StatusCode::kFailedPrecondition);
}

} // namespace
} // namespace pl::minivessel
