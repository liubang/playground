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
// Created: 2026/05/24 11:05

#include <algorithm>
#include <gtest/gtest.h>

#include "cpp/pl/flux/analysis/builtin_metadata.h"

namespace pl::flux::analysis {
namespace {

TEST(BuiltinMetadataTest, ListsKnownPackages) {
    const auto packages = KnownPackages();
    EXPECT_NE(std::find(packages.begin(), packages.end(), "array"), packages.end());
    EXPECT_NE(std::find(packages.begin(), packages.end(), "sqlite"), packages.end());
    EXPECT_NE(std::find(packages.begin(), packages.end(), "mysql"), packages.end());
    EXPECT_NE(std::find(packages.begin(), packages.end(), "timezone"), packages.end());
    EXPECT_TRUE(IsKnownPackage("strings"));
    EXPECT_FALSE(IsKnownPackage("experimental/unknown"));
}

TEST(BuiltinMetadataTest, DescribesPackageBuiltins) {
    const auto* sig = FindBuiltinSignature("array", "map");
    ASSERT_NE(sig, nullptr);
    EXPECT_EQ("array.map", sig->fq_name);
    ASSERT_EQ(2, sig->params.size());
    EXPECT_EQ("arr", sig->params[0].name);
    EXPECT_EQ(BuiltinParamKind::Required, sig->params[0].kind);
    EXPECT_EQ("fn", sig->params[1].name);
}

TEST(BuiltinMetadataTest, DescribesPipeBuiltins) {
    const auto* sig = FindUniverseBuiltinSignature("filter");
    ASSERT_NE(sig, nullptr);
    ASSERT_GE(sig->params.size(), 2);
    EXPECT_EQ("tables", sig->params[0].name);
    EXPECT_EQ(BuiltinParamKind::Pipe, sig->params[0].kind);
    EXPECT_EQ("fn", sig->params[1].name);
    EXPECT_EQ(BuiltinParamKind::Required, sig->params[1].kind);
}

TEST(BuiltinMetadataTest, MarksProviderPackages) {
    const auto* array_from = FindBuiltinSignature("array", "from");
    const auto* sqlite_from = FindBuiltinSignature("sqlite", "from");
    const auto* mysql_from = FindBuiltinSignature("mysql", "from");
    ASSERT_NE(array_from, nullptr);
    ASSERT_NE(sqlite_from, nullptr);
    ASSERT_NE(mysql_from, nullptr);
    EXPECT_TRUE(array_from->provider);
    EXPECT_TRUE(sqlite_from->provider);
    EXPECT_TRUE(mysql_from->provider);
}

TEST(BuiltinMetadataTest, DistinguishesCallableAndValueExports) {
    const auto* timezone_utc = FindBuiltinSignature("timezone", "utc");
    const auto* timezone_fixed = FindBuiltinSignature("timezone", "fixed");
    ASSERT_NE(timezone_utc, nullptr);
    ASSERT_NE(timezone_fixed, nullptr);
    EXPECT_EQ(BuiltinExportKind::Value, timezone_utc->kind);
    EXPECT_EQ(BuiltinExportKind::Function, timezone_fixed->kind);
    EXPECT_FALSE(IsCallableBuiltin(*timezone_utc));
    EXPECT_TRUE(IsCallableBuiltin(*timezone_fixed));
    EXPECT_EQ("timezone.utc: record", SignatureLabel(*timezone_utc));
}

TEST(BuiltinMetadataTest, BuildsCompletionParameters) {
    const auto* sig = FindBuiltinSignature("array", "map");
    ASSERT_NE(sig, nullptr);
    const auto params = CompletionParams(*sig);
    ASSERT_EQ(2, params.size());
    EXPECT_EQ("arr:", params[0]);
    EXPECT_EQ("fn:", params[1]);
}

TEST(BuiltinMetadataTest, CompletionParametersDoNotExposeOptionalMarkerSyntax) {
    const auto* sig = FindUniverseBuiltinSignature("yield");
    ASSERT_NE(sig, nullptr);
    const auto params = CompletionParams(*sig);
    ASSERT_EQ(1, params.size());
    EXPECT_EQ("name:", params[0]);
}

TEST(BuiltinMetadataTest, MatchesImplementedRuntimeContracts) {
    const auto* sum = FindUniverseBuiltinSignature("sum");
    ASSERT_NE(sum, nullptr);
    ASSERT_GE(sum->params.size(), 1);
    EXPECT_EQ("values", sum->params[0].name);
    EXPECT_EQ(BuiltinParamKind::Pipe, sum->params[0].kind);

    const auto* csv_from = FindBuiltinSignature("csv", "from");
    ASSERT_NE(csv_from, nullptr);
    ASSERT_EQ(3, csv_from->params.size());
    EXPECT_EQ("mode", csv_from->params[2].name);
    ASSERT_TRUE(csv_from->params[2].default_value.has_value());
    EXPECT_EQ("annotations", *csv_from->params[2].default_value);

    const auto* join = FindUniverseBuiltinSignature("join");
    ASSERT_NE(join, nullptr);
    ASSERT_GE(join->params.size(), 2);
    EXPECT_EQ("on", join->params[1].name);
    EXPECT_EQ("[string]", join->params[1].type);
}

} // namespace
} // namespace pl::flux::analysis
