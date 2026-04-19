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
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

namespace pl {
namespace {

std::string ReplaceAll(std::string text, const std::string& needle, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return text;
}

std::string RunfilePath(const std::string& relative_path) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    if (test_srcdir == nullptr || test_workspace == nullptr) {
        return relative_path;
    }
    return std::string(test_srcdir) + "/" + test_workspace + "/" + relative_path;
}

std::string ReadAllText(const std::string& path) {
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << "failed to open " << path;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string RewriteExamplePaths(std::string source) {
    source = ReplaceAll(source,
                        "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv",
                        RunfilePath("cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv"));
    source = ReplaceAll(source,
                        "cpp/pl/flux/examples/ops_dashboard/mem_usage.annotated.csv",
                        RunfilePath("cpp/pl/flux/examples/ops_dashboard/mem_usage.annotated.csv"));
    return source;
}

FluxCliResult ExecuteExampleScript(const std::string& relative_path, Environment& env) {
    const std::string path = RunfilePath(relative_path);
    return ExecuteFluxSource(RewriteExamplePaths(ReadAllText(path)), path, env);
}

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
    EXPECT_NE(std::string::npos, result.output.find("Result: data\n"));
    EXPECT_NE(std::string::npos, result.output.find("Table: bucket=csv, rows=1\n"));
    EXPECT_NE(std::string::npos, result.output.find("_time"));
    EXPECT_NE(std::string::npos, result.output.find("_measurement"));
    EXPECT_NE(std::string::npos, result.output.find("_value"));
    EXPECT_NE(std::string::npos, result.output.find("\"2024-01-01T00:00:00Z\""));
    EXPECT_NE(std::string::npos, result.output.find("\"cpu\""));
    EXPECT_NE(std::string::npos, result.output.find("\"95.5\""));
    EXPECT_NE(std::string::npos, result.output.find("+"));
    EXPECT_TRUE(result.error.empty());
}

TEST(FluxCliTest, ExecutesCheckedInOpsDashboardExample) {
    auto env = MakeFluxCliEnvironment();
    auto result = ExecuteExampleScript("cpp/pl/flux/examples/ops_dashboard/query.flux", env);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_TRUE(result.error.empty());
    EXPECT_NE(std::string::npos, result.output.find("Result: host_health\n"));
    EXPECT_NE(std::string::npos, result.output.find("2024-05-01T10:01:00Z"));
    EXPECT_NE(std::string::npos, result.output.find("2024-05-01T10:02:00Z"));
    EXPECT_NE(std::string::npos, result.output.find("72"));
    EXPECT_NE(std::string::npos, result.output.find("63"));
    EXPECT_NE(std::string::npos, result.output.find("82"));
    EXPECT_NE(std::string::npos, result.output.find("68"));
}

TEST(FluxCliTest, ExecutesCheckedInOpsDashboardQueryVariants) {
    struct ExampleCase {
        std::string path;
        std::vector<std::string> expected_output_fragments;
    };

    const std::vector<ExampleCase> cases = {
        {
            "cpp/pl/flux/examples/ops_dashboard/cpu_top_windows.flux",
            {"Result: cpu_top_windows\n", "91", "87", "82"},
        },
        {
            "cpp/pl/flux/examples/ops_dashboard/fleet_usage_union.flux",
            {"Result: fleet_usage\n", "\"cpu\"", "\"mem\"", "\"edge-2\"", "91"},
        },
        {
            "cpp/pl/flux/examples/ops_dashboard/edge1_cpu_rollup.flux",
            {"Result: edge1_cpu_rollup\n", "samples", "3", "226"},
        },
        {
            "cpp/pl/flux/examples/ops_dashboard/latest_west_cpu.flux",
            {"Result: latest_west_cpu\n", "\"edge-2\"", "\"us-west\"", "87"},
        },
    };

    for (const auto& example : cases) {
        SCOPED_TRACE(example.path);
        auto env = MakeFluxCliEnvironment();
        auto result = ExecuteExampleScript(example.path, env);

        EXPECT_EQ(0, result.exit_code);
        EXPECT_TRUE(result.error.empty());
        for (const auto& fragment : example.expected_output_fragments) {
            EXPECT_NE(std::string::npos, result.output.find(fragment));
        }
    }
}

TEST(FluxCliTest, RendersMultipleNamedResultsAsSeparateBlocks) {
    auto env = MakeFluxCliEnvironment();
    auto result = ExecuteFluxSource(R"(
        value = 41
        value + 1
    )",
                                    "<test>", env);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_EQ(
        "Result: value\n"
        "41\n"
        "\n"
        "Result: _result\n"
        "42\n",
        result.output);
    EXPECT_TRUE(result.error.empty());
}

TEST(FluxCliTest, EmitsAnnotatedCsvForTableResults) {
    auto env = MakeFluxCliEnvironment();
    FluxCliOptions options;
    options.annotated_csv = true;

    auto result = ExecuteFluxSource(R"(
        import "csv"

        data = csv.from(
            csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n",
            mode: "raw",
        )
    )",
                                    "query.flux", env, options);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_EQ(
        "#datatype,string,long,string,string,string\n"
        "#group,false,false,false,false,false\n"
        "#default,data,,,,\n"
        ",result,table,_time,_measurement,_value\n"
        ",data,0,2024-01-01T00:00:00Z,cpu,95.5\n",
        result.output);
    EXPECT_TRUE(result.error.empty());
}

TEST(FluxCliTest, EmitsAnnotatedCsvForScalarResults) {
    auto env = MakeFluxCliEnvironment();
    FluxCliOptions options;
    options.annotated_csv = true;

    auto result = ExecuteFluxSource("value = 41\nvalue + 1", "<test>", env, options);

    EXPECT_EQ(0, result.exit_code);
    EXPECT_EQ(
        "#datatype,string,long,long\n"
        "#group,false,false,false\n"
        "#default,value,,\n"
        ",result,table,_value\n"
        ",value,0,41\n"
        "\n"
        "#datatype,string,long,long\n"
        "#group,false,false,false\n"
        "#default,_result,,\n"
        ",result,table,_value\n"
        ",_result,1,42\n",
        result.output);
    EXPECT_TRUE(result.error.empty());
}

TEST(FluxCliTest, UsesYieldNameInHumanReadableAndAnnotatedCsvOutput) {
    auto env = MakeFluxCliEnvironment();

    auto human = ExecuteFluxSource(R"(
        builtin from : (bucket: string) => stream[A]
        builtin yield : (<-tables: stream[A], ?name: string) => stream[A]

        from(
            bucket: "telegraf",
            rows: [{_time: "2024-01-01T00:00:00Z", _value: 95.0}],
        )
            |> yield(name: "cpu")
    )",
                                   "<test>", env);

    EXPECT_EQ(0, human.exit_code);
    EXPECT_NE(std::string::npos, human.output.find("Result: cpu\n"));
    EXPECT_TRUE(human.error.empty());

    env = MakeFluxCliEnvironment();
    FluxCliOptions csv_options;
    csv_options.annotated_csv = true;
    auto csv = ExecuteFluxSource(R"(
        builtin from : (bucket: string) => stream[A]
        builtin yield : (<-tables: stream[A], ?name: string) => stream[A]

        from(
            bucket: "telegraf",
            rows: [{_time: "2024-01-01T00:00:00Z", _value: 95.0}],
        )
            |> yield(name: "cpu")
    )",
                                 "<test>", env, csv_options);

    EXPECT_EQ(0, csv.exit_code);
    EXPECT_NE(std::string::npos, csv.output.find("#default,cpu,,,"));
    EXPECT_NE(std::string::npos, csv.output.find("2024-01-01T00:00:00Z"));
    EXPECT_NE(std::string::npos, csv.output.find(",cpu,"));
    EXPECT_TRUE(csv.error.empty());
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

TEST(FluxCliTest, ReplSupportsMultiLineInputAndContinuationPrompt) {
    std::istringstream input(
        "config = {\n"
        "host: \"local\",\n"
        "port: 8080,\n"
        "}\n"
        "config.host\n"
        ":quit\n");
    std::ostringstream output;
    std::ostringstream error;

    int exit_code = RunFluxRepl(input, output, error, true);

    EXPECT_EQ(0, exit_code);
    EXPECT_NE(std::string::npos, output.str().find("Flux REPL. Type :help for commands.\n"));
    EXPECT_NE(std::string::npos, output.str().find("flux> "));
    EXPECT_NE(std::string::npos, output.str().find("....> "));
    EXPECT_NE(std::string::npos, output.str().find("{host: \"local\", port: 8080}"));
    EXPECT_NE(std::string::npos, output.str().find("\"local\""));
    EXPECT_TRUE(error.str().empty());
}

TEST(FluxCliTest, ReplHelpCommandPrintsBuiltInCommands) {
    std::istringstream input("help\n:help\nquit\n");
    std::ostringstream output;
    std::ostringstream error;

    int exit_code = RunFluxRepl(input, output, error, true);

    EXPECT_EQ(0, exit_code);
    EXPECT_NE(std::string::npos, output.str().find("Flux REPL commands:\n"));
    EXPECT_NE(std::string::npos, output.str().find("help, :help, .help  Show this help text.\n"));
    EXPECT_NE(
        std::string::npos,
        output.str().find("quit, :quit, .quit, exit, :exit, .exit Leave the REPL.\n"));
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

TEST(FluxCliTest, TrailingSemicolonReportsParserErrorInsteadOfCrashing) {
    auto env = MakeFluxCliEnvironment();

    auto result = ExecuteFluxSource("1;", "<test>", env);

    EXPECT_EQ(2, result.exit_code);
    EXPECT_TRUE(result.output.empty());
    EXPECT_NE(std::string::npos, result.error.find("parser errors"));
    EXPECT_NE(std::string::npos, result.error.find("unexpected token for statement"));
}

} // namespace
} // namespace pl
