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

#include "cpp/pl/flux/flux_cli.h"
#include <gtest/gtest.h>
#include <sstream>

namespace pl {
namespace {

TEST(FluxCliTest, ExecutesSourceWithPreludeBuiltins) {
    auto env = MakeFluxCliEnvironment();

    auto result = ExecuteFluxSource("sum([1, 2, 3])", "<test>", env);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_EQ("6\n", result.output);
    EXPECT_TRUE(result.error.empty());
}

TEST(FluxCliTest, ExecutesFluxFileSourceWithImportsAndPipelines) {
    auto env = MakeFluxCliEnvironment();
    auto result = ExecuteFluxSource(R"(
        import "csv"

        data = csv.from(
            csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n",
            mode: "raw",
        )
            |> filter(fn: (r) => r._measurement == "cpu")
            |> limit(n: 1)
    )",
                                    "query.flux", env);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_EQ("<table bucket=\"csv\" rows=1>\n", result.output);
    EXPECT_TRUE(result.error.empty());
}

TEST(FluxCliTest, ReportsParserAndRuntimeErrors) {
    auto env = MakeFluxCliEnvironment();

    auto parse_result = ExecuteFluxSource("value = [1 2]", "<bad>", env);
    EXPECT_EQ(2, parse_result.exit_code);
    EXPECT_TRUE(parse_result.output.empty());
    EXPECT_NE(std::string::npos, parse_result.error.find("parser errors"));

    auto runtime_result = ExecuteFluxSource("missing + 1", "<bad>", env);
    EXPECT_EQ(1, runtime_result.exit_code);
    EXPECT_TRUE(runtime_result.output.empty());
    EXPECT_NE(std::string::npos, runtime_result.error.find("missing"));
}

TEST(FluxCliTest, DumpsAstAsTreeAndJson) {
    auto tree_result = DumpFluxAstSource("value = 1 + 2", "<test>");
    EXPECT_EQ(0, tree_result.exit_code);
    EXPECT_TRUE(tree_result.error.empty());
    EXPECT_NE(std::string::npos, tree_result.output.find("File name=\"<test>\""));
    EXPECT_NE(std::string::npos, tree_result.output.find("VariableAssignment id=value"));
    EXPECT_NE(std::string::npos, tree_result.output.find("BinaryExpr op=+"));

    FluxAstOptions json_options;
    json_options.json = true;
    auto json_result = DumpFluxAstSource("value = 1 + 2", "<test>", json_options);
    EXPECT_EQ(0, json_result.exit_code);
    EXPECT_TRUE(json_result.error.empty());
    EXPECT_NE(std::string::npos, json_result.output.find("\"type\":\"File\""));
    EXPECT_NE(std::string::npos, json_result.output.find("VariableAssignment"));
}

TEST(FluxCliTest, DumpsPartialAstAndReportsParserErrors) {
    auto result = DumpFluxAstSource("value = [1 2]", "<bad>");

    EXPECT_EQ(2, result.exit_code);
    EXPECT_NE(std::string::npos, result.output.find("File name=\"<bad>\""));
    EXPECT_NE(std::string::npos, result.error.find("parser errors"));
}

TEST(FluxCliTest, ReplPreservesEnvironmentBetweenInputs) {
    std::istringstream input("x = 40\nx + 2\n:quit\n");
    std::ostringstream output;
    std::ostringstream error;

    int exit_code = RunFluxRepl(input, output, error, false);

    EXPECT_EQ(0, exit_code);
    EXPECT_EQ("40\n42\n", output.str());
    EXPECT_TRUE(error.str().empty());
}

TEST(FluxCliTest, QuietModeSuppressesValueOutput) {
    auto env = MakeFluxCliEnvironment();
    FluxCliOptions options;
    options.quiet = true;

    auto result = ExecuteFluxSource("1 + 2", "<test>", env, options);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_TRUE(result.output.empty());
    EXPECT_TRUE(result.error.empty());
}

} // namespace
} // namespace pl
