// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0

#include <type_traits>

#include "cpp/pl/minivessel/active_log_storage.h"
#include "cpp/pl/minivessel/minidfs_filesystem.h"
#include "gtest/gtest.h"

namespace pl::minivessel {
namespace {

TEST(MiniDfsFileSystemTest, AdvertisesImmutableObjectCapabilityOnly) {
    static_assert(std::is_base_of_v<ObjectMetadataBackend, MiniDfsFileSystem>);
    static_assert(!std::is_base_of_v<ActiveLogStorage, MiniDfsFileSystem>);

    MiniDfsFileSystem filesystem(nullptr);
    EXPECT_TRUE(filesystem.capabilities().has(ObjectStorageFeature::kImmutableObjects));
    EXPECT_NE(filesystem.object_filesystem(), nullptr);
}

} // namespace
} // namespace pl::minivessel
