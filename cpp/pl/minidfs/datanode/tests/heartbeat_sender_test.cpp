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

#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/datanode/heartbeat_sender.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

class HeartbeatSenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("minidfs_hb_test_" + std::to_string(::getpid()) +
                                                 "_" + std::to_string(counter_++));
        fs::create_directories(test_dir_);

        LocalBlockStore::Config config;
        config.storage_root = test_dir_.string();
        config.reserved_bytes = 0;
        store_ = std::make_unique<LocalBlockStore>(std::move(config));
        store_->init();
    }

    void TearDown() override {
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::unique_ptr<LocalBlockStore> store_;
    static inline int counter_ = 0;
};

TEST_F(HeartbeatSenderTest, SendOnceSuccess) {
    std::atomic<int> hb_count{0};
    uint64_t received_dn_id = 0;

    HeartbeatFunc hb_func = [&](uint64_t dn_id,
                                uint64_t capacity,
                                uint64_t used,
                                uint64_t free_bytes) -> Result<std::vector<HeartbeatCommand>> {
        hb_count++;
        received_dn_id = dn_id;
        EXPECT_GT(capacity, 0u);
        return std::vector<HeartbeatCommand>{};
    };

    CommandHandler cmd_handler = [](const HeartbeatCommand&) {
    };

    HeartbeatSender::Config cfg;
    cfg.datanode_id = 77;
    cfg.interval_ms = 100000;

    HeartbeatSender sender(cfg, store_.get(), hb_func, cmd_handler);
    auto result = sender.send_once();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(hb_count.load(), 1);
    EXPECT_EQ(received_dn_id, 77u);
}

TEST_F(HeartbeatSenderTest, SendOnceWithCommands) {
    std::vector<HeartbeatCommand> received_cmds;

    HeartbeatFunc hb_func =
        [](uint64_t, uint64_t, uint64_t, uint64_t) -> Result<std::vector<HeartbeatCommand>> {
        std::vector<HeartbeatCommand> cmds;
        cmds.push_back(HeartbeatCommand{
            .type = CommandType::kReplicate,
            .block_id = 100,
            .generation_stamp = 5000,
            .target_host = "dn2",
            .target_port = 9999,
        });
        cmds.push_back(HeartbeatCommand{
            .type = CommandType::kDelete,
            .block_id = 101,
            .generation_stamp = 5001,
        });
        return cmds;
    };

    CommandHandler cmd_handler = [&](const HeartbeatCommand& cmd) {
        received_cmds.push_back(cmd);
    };

    HeartbeatSender::Config cfg;
    cfg.datanode_id = 1;
    HeartbeatSender sender(cfg, store_.get(), hb_func, cmd_handler);

    auto result = sender.send_once();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 2u);
    EXPECT_EQ(received_cmds.size(), 2u);
    EXPECT_EQ(received_cmds[0].type, CommandType::kReplicate);
    EXPECT_EQ(received_cmds[0].block_id, 100u);
    EXPECT_EQ(received_cmds[1].type, CommandType::kDelete);
}

TEST_F(HeartbeatSenderTest, SendOnceRPCFailure) {
    HeartbeatFunc hb_func =
        [](uint64_t, uint64_t, uint64_t, uint64_t) -> Result<std::vector<HeartbeatCommand>> {
        return pl::makeError(pl::Status(5000, "heartbeat RPC failed"));
    };
    CommandHandler cmd_handler = [](const HeartbeatCommand&) {
    };

    HeartbeatSender::Config cfg;
    cfg.datanode_id = 1;
    HeartbeatSender sender(cfg, store_.get(), hb_func, cmd_handler);

    auto result = sender.send_once();
    EXPECT_TRUE(result.hasError());
}

TEST_F(HeartbeatSenderTest, NoneCommandsNotDispatched) {
    HeartbeatFunc hb_func =
        [](uint64_t, uint64_t, uint64_t, uint64_t) -> Result<std::vector<HeartbeatCommand>> {
        std::vector<HeartbeatCommand> cmds;
        cmds.push_back(HeartbeatCommand{.type = CommandType::kNone});
        return cmds;
    };

    std::atomic<int> dispatched{0};
    CommandHandler cmd_handler = [&](const HeartbeatCommand&) {
        dispatched++;
    };

    HeartbeatSender::Config cfg;
    cfg.datanode_id = 1;
    HeartbeatSender sender(cfg, store_.get(), hb_func, cmd_handler);

    auto result = sender.send_once();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(dispatched.load(), 0); // kNone should not be dispatched
}

TEST_F(HeartbeatSenderTest, StartAndStop) {
    std::atomic<int> hb_count{0};
    HeartbeatFunc hb_func =
        [&](uint64_t, uint64_t, uint64_t, uint64_t) -> Result<std::vector<HeartbeatCommand>> {
        hb_count++;
        return std::vector<HeartbeatCommand>{};
    };
    CommandHandler cmd_handler = [](const HeartbeatCommand&) {
    };

    HeartbeatSender::Config cfg;
    cfg.datanode_id = 1;
    cfg.interval_ms = 50; // short interval

    HeartbeatSender sender(cfg, store_.get(), hb_func, cmd_handler);
    EXPECT_FALSE(sender.running());

    sender.start();
    EXPECT_TRUE(sender.running());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sender.stop();
    EXPECT_FALSE(sender.running());
    EXPECT_GE(hb_count.load(), 1);
}

TEST_F(HeartbeatSenderTest, DoubleStartNoOp) {
    HeartbeatFunc hb_func =
        [](uint64_t, uint64_t, uint64_t, uint64_t) -> Result<std::vector<HeartbeatCommand>> {
        return std::vector<HeartbeatCommand>{};
    };
    CommandHandler cmd_handler = [](const HeartbeatCommand&) {
    };

    HeartbeatSender::Config cfg;
    cfg.datanode_id = 1;
    cfg.interval_ms = 100000;

    HeartbeatSender sender(cfg, store_.get(), hb_func, cmd_handler);
    sender.start();
    sender.start(); // should be no-op
    EXPECT_TRUE(sender.running());
    sender.stop();
}

} // namespace
} // namespace pl::minidfs
