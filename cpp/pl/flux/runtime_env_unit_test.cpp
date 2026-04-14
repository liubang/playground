// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "cpp/pl/flux/runtime_env.h"
#include <gtest/gtest.h>

namespace pl {
namespace {

TEST(RuntimeEnvTest, DefinesAndLooksUpLocalBindings) {
    Environment env;
    env.define("host", Value::string("local"));
    env.define_option("task.every", Value::duration("5m"));

    ASSERT_TRUE(env.has_local("host"));
    ASSERT_TRUE(env.has_local_option("task.every"));

    ASSERT_TRUE(env.lookup("host").ok());
    EXPECT_EQ("\"local\"", env.lookup("host")->string());
    ASSERT_TRUE(env.lookup_option("task.every").ok());
    EXPECT_EQ("5m", env.lookup_option("task.every")->string());
}

TEST(RuntimeEnvTest, LooksUpBindingsFromParentScope) {
    auto global = std::make_shared<Environment>();
    global->define("bucket", Value::string("telegraf"));
    global->define_option("task.every", Value::duration("1m"));

    Environment child(global);
    child.define("host", Value::string("local"));

    ASSERT_TRUE(child.lookup("bucket").ok());
    EXPECT_EQ("\"telegraf\"", child.lookup("bucket")->string());
    ASSERT_TRUE(child.lookup_option("task.every").ok());
    EXPECT_EQ("1m", child.lookup_option("task.every")->string());
    ASSERT_TRUE(child.lookup("host").ok());
    EXPECT_EQ("\"local\"", child.lookup("host")->string());
}

TEST(RuntimeEnvTest, ChildBindingsShadowParentBindings) {
    auto global = std::make_shared<Environment>();
    global->define("threshold", Value::floating(0.5));

    Environment child(global);
    child.define("threshold", Value::floating(0.9));

    ASSERT_TRUE(global->lookup("threshold").ok());
    EXPECT_EQ("0.5", global->lookup("threshold")->string());
    ASSERT_TRUE(child.lookup("threshold").ok());
    EXPECT_EQ("0.9", child.lookup("threshold")->string());
}

TEST(RuntimeEnvTest, AssignUpdatesNearestBindingOwner) {
    auto global = std::make_shared<Environment>();
    global->define("count", Value::integer(1));

    Environment child(global);
    ASSERT_TRUE(child.assign("count", Value::integer(2)).ok());
    ASSERT_TRUE(global->lookup("count").ok());
    EXPECT_EQ("2", global->lookup("count")->string());

    child.define("count", Value::integer(3));
    ASSERT_TRUE(child.assign("count", Value::integer(4)).ok());
    ASSERT_TRUE(child.lookup("count").ok());
    EXPECT_EQ("4", child.lookup("count")->string());
    ASSERT_TRUE(global->lookup("count").ok());
    EXPECT_EQ("2", global->lookup("count")->string());
}

TEST(RuntimeEnvTest, ReportsMissingBindings) {
    Environment env;

    EXPECT_FALSE(env.lookup("missing").ok());
    EXPECT_FALSE(env.lookup_option("task.missing").ok());
    EXPECT_FALSE(env.assign("missing", Value::integer(1)).ok());
    EXPECT_EQ(absl::StatusCode::kNotFound, env.lookup("missing").status().code());
    EXPECT_EQ(absl::StatusCode::kNotFound, env.lookup_option("task.missing").status().code());
    EXPECT_EQ(absl::StatusCode::kNotFound, env.assign("missing", Value::integer(1)).code());
}

} // namespace
} // namespace pl
