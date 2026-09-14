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
// Created: 2026/09/14 10:05

#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/prism/syntax/parser.h"
#include "gtest/gtest.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace pl::prism::syntax {
namespace {

// Reads a golden corpus file from the test runfiles. Lines starting with '#'
// are comments; blank lines are skipped.
std::vector<std::string> read_corpus(const char* basename) {
    std::string error;
    std::unique_ptr<bazel::tools::cpp::runfiles::Runfiles> runfiles(
        bazel::tools::cpp::runfiles::Runfiles::CreateForTest(&error));
    EXPECT_TRUE(runfiles != nullptr) << error;
    if (runfiles == nullptr) {
        return {};
    }
    const std::string path =
        runfiles->Rlocation(std::string("playground/cpp/pl/prism/testdata/") + basename);
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot open corpus file: " << path;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') {
            lines.push_back(line);
        }
    }
    return lines;
}

std::string collect_failures(const std::vector<std::string>& lines,
                             const std::function<ParseResult(Parser&)>& parse) {
    std::string detail;
    size_t failed = 0;
    for (const std::string& sql : lines) {
        Parser parser(sql);
        ParseResult result = parse(parser);
        if (!result.ok()) {
            ++failed;
            if (failed <= 10) {
                detail += "  " + sql + "\n    -> " + result.errors[0].message + "\n";
            }
        }
    }
    if (failed == 0) {
        return {};
    }
    return std::to_string(failed) + " of " + std::to_string(lines.size()) + " entries failed:\n" +
           detail;
}

TEST(GoldenCorpus, StatementsParse) {
    const std::vector<std::string> lines = read_corpus("golden_statements.txt");
    ASSERT_FALSE(lines.empty());
    const std::string failures =
        collect_failures(lines, [](Parser& p) { return p.parse_statement(); });
    EXPECT_TRUE(failures.empty()) << failures;
}

TEST(GoldenCorpus, ExpressionsParse) {
    const std::vector<std::string> lines = read_corpus("golden_expressions.txt");
    ASSERT_FALSE(lines.empty());
    const std::string failures =
        collect_failures(lines, [](Parser& p) { return p.parse_expression(); });
    EXPECT_TRUE(failures.empty()) << failures;
}

// Entries that require features scheduled for P1 and beyond. The test fails
// as soon as one of them starts parsing, which is the signal to move the
// newly passing entries into golden_statements.txt / golden_expressions.txt.
TEST(GoldenCorpus, FutureCorpusStillUnsupported) {
    const std::vector<std::string> lines = read_corpus("golden_future.txt");
    ASSERT_FALSE(lines.empty());
    std::string newly;
    size_t passing = 0;
    for (const std::string& sql : lines) {
        Parser as_statement(sql);
        Parser as_expression(sql);
        if (as_statement.parse_statement().ok() || as_expression.parse_expression().ok()) {
            ++passing;
            if (passing <= 10) {
                newly += "  " + sql + "\n";
            }
        }
    }
    EXPECT_EQ(0, passing) << passing << " future entries now parse; "
                          << "move them to the pass corpus:\n"
                          << newly;
}

} // namespace
} // namespace pl::prism::syntax
