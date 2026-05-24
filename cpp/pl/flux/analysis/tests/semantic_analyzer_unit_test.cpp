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

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/flux/analysis/semantic_analyzer.h"
#include "cpp/pl/flux/syntax/parser.h"

namespace pl::flux::analysis {
namespace {

AnalysisResult Analyze(std::string source) {
    Parser parser(std::move(source));
    auto file = parser.parse_file("semantic_test.flux");
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    return SemanticAnalyzer().Analyze(*file);
}

bool HasDiagnostic(const AnalysisResult& result, const std::string& needle) {
    for (const auto& diag : result.diagnostics) {
        if (diag.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<File> ParseFile(std::string source, std::string name) {
    Parser parser(std::move(source));
    auto file = parser.parse_file(std::move(name));
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    return file;
}

TEST(SemanticAnalyzerTest, AcceptsKnownPackageCall) {
    auto result = Analyze(R"(
import "array"

array.map(arr: [1, 2], fn: (x) => x + 1)
)");
    EXPECT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
}

TEST(SemanticAnalyzerTest, ReportsUnknownKnownPackageMember) {
    auto result = Analyze(R"(
import "array"

array.nope(arr: [1])
)");
    EXPECT_TRUE(HasDiagnostic(result, "unknown function `nope`"));
}

TEST(SemanticAnalyzerTest, ChecksMissingAndUnknownNamedArguments) {
    auto missing = Analyze(R"(
import "array"

array.map(arr: [1])
)");
    EXPECT_TRUE(HasDiagnostic(missing, "missing required argument `fn`"));

    auto unknown = Analyze(R"(
import "array"

array.map(arr: [1], fn: (x) => x, nope: 2)
)");
    EXPECT_TRUE(HasDiagnostic(unknown, "unknown argument `nope`"));
}

TEST(SemanticAnalyzerTest, PipeSatisfiesPipeParameter) {
    auto result = Analyze(R"(
import "array"

array.from(rows: [{_time: 2024-01-01T00:00:00Z, _value: 1}])
    |> filter(fn: (r) => r._value > 0)
)");
    EXPECT_FALSE(HasDiagnostic(result, "missing required argument `tables`"));
    EXPECT_TRUE(result.diagnostics.empty()) << result.diagnostics.front().message;
}

TEST(SemanticAnalyzerTest, ReportsUndefinedIdentifiersButResolvesLambdaParams) {
    auto result = Analyze(R"(
f = (r) => r._value + missing
)");
    EXPECT_TRUE(HasDiagnostic(result, "undefined identifier: missing"));
    EXPECT_FALSE(HasDiagnostic(result, "undefined identifier: r"));
}

TEST(SemanticAnalyzerTest, ReportsDuplicateFunctionParametersAndArguments) {
    auto duplicate_param = Analyze(R"(
f = (r, r) => r
)");
    EXPECT_TRUE(HasDiagnostic(duplicate_param, "duplicate function parameter: r"));

    auto duplicate_arg = Analyze(R"(
import "array"

array.map(arr: [1], arr: [2], fn: (x) => x)
)");
    EXPECT_TRUE(HasDiagnostic(duplicate_arg, "duplicate argument `arr`"));
}

TEST(SemanticAnalyzerTest, BuildsDefinitionReferenceGraph) {
    auto result = Analyze(R"(
x = 1
y = x + 2
)");
    const auto* def = result.FindDefinition("x");
    ASSERT_NE(def, nullptr);
    const auto refs = result.ReferencesOf(*def);
    ASSERT_EQ(1, refs.size());
    EXPECT_EQ("x", refs[0]->name);
    EXPECT_EQ(def->id, refs[0]->definition_id);
    EXPECT_TRUE(refs[0]->resolved);
    EXPECT_EQ(
        def,
        result.DefinitionForSymbolAt(refs[0]->location.start.line, refs[0]->location.start.column));
}

TEST(SemanticAnalyzerTest, KeepsShadowedFunctionParametersSeparate) {
    auto result = Analyze(R"(
left = (r) => r + 1
right = (r) => r + 2
)");
    std::vector<const Symbol*> params;
    for (const auto& def : result.definitions) {
        if (def.name == "r" && def.kind == SymbolKind::Parameter) {
            params.push_back(&def);
        }
    }
    ASSERT_EQ(2, params.size());
    const auto left_refs = result.ReferencesOf(*params[0]);
    const auto right_refs = result.ReferencesOf(*params[1]);
    EXPECT_EQ(1, left_refs.size());
    EXPECT_EQ(1, right_refs.size());
    EXPECT_NE(left_refs[0]->definition_id, right_refs[0]->definition_id);
}

TEST(SemanticAnalyzerTest, ResolvesBlockShadowingToNearestScope) {
    auto result = Analyze(R"(
x = 1
testcase t {
    x = 2
    return x
}
)");
    const Symbol* block_x = nullptr;
    for (const auto& def : result.definitions) {
        if (def.name == "x" && def.kind == SymbolKind::Variable && def.scope_id != 0) {
            block_x = &def;
        }
    }
    ASSERT_NE(block_x, nullptr);
    const auto refs = result.ReferencesOf(*block_x);
    ASSERT_EQ(1, refs.size());
    EXPECT_EQ(block_x->id, refs[0]->definition_id);
}

TEST(SemanticAnalyzerTest, BindsImportedPackageAndPackageMemberCalls) {
    auto result = Analyze(R"(
import "array"

array.map(arr: [1], fn: (x) => x)
)");
    const auto* package = result.FindDefinition("array");
    ASSERT_NE(package, nullptr);
    EXPECT_EQ(SymbolKind::Import, package->kind);
    EXPECT_EQ("array", *package->import_path);

    bool saw_package_ref = false;
    bool saw_member_ref = false;
    for (const auto& ref : result.references) {
        if (ref.kind == ReferenceKind::PackageObject && ref.definition_id == package->id) {
            saw_package_ref = true;
        }
        if (ref.kind == ReferenceKind::PackageMember && ref.name == "array.map" &&
            ref.package == "array" && ref.member == "map") {
            saw_member_ref = true;
        }
    }
    EXPECT_TRUE(saw_package_ref);
    EXPECT_TRUE(saw_member_ref);
}

TEST(SemanticAnalyzerTest, InfersScalarCompositeAndFunctionTypes) {
    auto result = Analyze(R"(
answer = 40 + 2
flag = answer > 0
shape = {host: "a", value: answer}
f = (r) => r.value + 1
)");
    ASSERT_NE(nullptr, result.FindDefinition("answer"));
    EXPECT_EQ("int", result.FindDefinition("answer")->type.ToString());
    ASSERT_NE(nullptr, result.FindDefinition("flag"));
    EXPECT_EQ("bool", result.FindDefinition("flag")->type.ToString());
    ASSERT_NE(nullptr, result.FindDefinition("shape"));
    EXPECT_EQ("{host: string, value: int}", result.FindDefinition("shape")->type.ToString());
    ASSERT_NE(nullptr, result.FindDefinition("f"));
    EXPECT_EQ("(r: dynamic) => dynamic", result.FindDefinition("f")->type.ToString());
    EXPECT_FALSE(result.expressions.empty());
    EXPECT_TRUE(result.TypeAt(2, 10).has_value());
}

TEST(SemanticAnalyzerTest, InfersArrayFromAndPipelineSchemas) {
    auto result = Analyze(R"(
import "array"

array.from(rows: [{_time: 2024-01-01T00:00:00Z, _value: 1, host: "a"}])
    |> keep(columns: ["_time", "_value"])
    |> rename(columns: {_value: "usage"})
)");
    ASSERT_FALSE(result.table_schemas.empty());
    const auto& schema = result.table_schemas.back();
    ASSERT_EQ(2, schema.columns.size());
    EXPECT_EQ("_time", schema.columns[0].name);
    EXPECT_EQ("time", schema.columns[0].type->ToString());
    EXPECT_EQ("usage", schema.columns[1].name);
    EXPECT_EQ("int", schema.columns[1].type->ToString());
}

TEST(SemanticAnalyzerTest, MapSchemaUsesFunctionReturnRecord) {
    auto result = Analyze(R"(
import "array"

array.from(rows: [{_value: 1, host: "a"}])
    |> map(fn: (r) => ({host: r.host, doubled: r._value * 2}))
)");
    ASSERT_FALSE(result.table_schemas.empty());
    const auto& schema = result.table_schemas.back();
    ASSERT_EQ(2, schema.columns.size());
    EXPECT_EQ("host", schema.columns[0].name);
    EXPECT_EQ("dynamic", schema.columns[0].type->ToString());
    EXPECT_EQ("doubled", schema.columns[1].name);
    EXPECT_EQ("dynamic", schema.columns[1].type->ToString());
}

TEST(SemanticAnalyzerTest, ReportsObviousTypeMismatches) {
    auto result = Analyze(R"(
x = "a" - 1
y = if 1 then "yes" else "no"
)");
    EXPECT_TRUE(HasDiagnostic(result, "operator `-` expects numeric operands"));
    EXPECT_TRUE(HasDiagnostic(result, "if condition expects bool"));
}

TEST(SemanticAnalyzerTest, AnalyzesPackageExportsAcrossFiles) {
    Package package;
    package.path = "example";
    package.package = "example";
    package.files.push_back(ParseFile("a = 1\n", "a.flux"));
    package.files.push_back(ParseFile("a = 2\nb = a\n", "b.flux"));

    auto result = PackageAnalyzer().Analyze(package);
    EXPECT_NE(nullptr, result.FindExport("a"));
    EXPECT_NE(nullptr, result.FindExport("b"));
    EXPECT_EQ(1, result.diagnostics.size());
    EXPECT_NE(std::string::npos, result.diagnostics[0].message.find("duplicate package export: a"));
}

} // namespace
} // namespace pl::flux::analysis
