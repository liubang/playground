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
// Created: 2026/08/17

#include <gtest/gtest.h>

#include "cpp/pl/minisearch/core/schema.h"

namespace pmc = pl::minisearch::core;

namespace {

pmc::FieldDef text(std::string name) {
    pmc::FieldDef def;
    def.name = std::move(name);
    def.type = pmc::FieldType::kText;
    return def;
}

pmc::FieldDef vec(std::string name, int dims) {
    pmc::FieldDef def;
    def.name = std::move(name);
    def.type = pmc::FieldType::kVector;
    def.dims = dims;
    def.indexed = false;
    return def;
}

pmc::Schema valid_schema() {
    pmc::Schema schema;
    schema.default_analyzer = "cjk_jieba";
    schema.fields["title"] = text("title");
    pmc::FieldDef source;
    source.name = "source";
    source.type = pmc::FieldType::kKeyword;
    schema.fields["source"] = source;
    pmc::FieldDef created;
    created.name = "created";
    created.type = pmc::FieldType::kNumeric;
    created.indexed = false;
    schema.fields["created"] = created;
    pmc::FieldDef embedding = vec("content_vec", 1024);
    embedding.server_embedded = true;
    embedding.source_field = "content";
    schema.fields["content_vec"] = embedding;
    pmc::FieldDef content = text("content");
    schema.fields["content"] = content;
    return schema;
}

} // namespace

TEST(SchemaTest, AcceptsValidSchema) {
    EXPECT_EQ(valid_schema().Validate(), std::nullopt);
}

TEST(SchemaTest, RejectsVectorWithoutDims) {
    pmc::Schema schema = valid_schema();
    schema.fields["content_vec"].dims = 0;
    EXPECT_NE(schema.Validate(), std::nullopt);
}

TEST(SchemaTest, RejectsUnknownMetric) {
    pmc::Schema schema = valid_schema();
    schema.fields["content_vec"].metric = "manhattan";
    EXPECT_NE(schema.Validate(), std::nullopt);
}

TEST(SchemaTest, RejectsIndexedVector) {
    pmc::Schema schema = valid_schema();
    schema.fields["content_vec"].indexed = true;
    EXPECT_NE(schema.Validate(), std::nullopt);
}

TEST(SchemaTest, RejectsNonTextSourceField) {
    pmc::Schema schema = valid_schema();
    schema.fields["content_vec"].source_field = "source";
    EXPECT_NE(schema.Validate(), std::nullopt);
}

TEST(SchemaTest, RejectsNameMismatch) {
    pmc::Schema schema = valid_schema();
    schema.fields["renamed"] = schema.fields["title"];
    EXPECT_NE(schema.Validate(), std::nullopt);
}
