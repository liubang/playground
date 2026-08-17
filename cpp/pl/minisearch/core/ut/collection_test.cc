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
#include <string>
#include <vector>

#include "cpp/pl/minisearch/core/collection.h"

namespace pmc = pl::minisearch::core;

namespace {

pmc::Schema make_schema() {
    pmc::Schema schema;
    pmc::FieldDef title;
    title.name = "title";
    title.type = pmc::FieldType::kText;
    schema.fields["title"] = title;
    pmc::FieldDef tags;
    tags.name = "tags";
    tags.type = pmc::FieldType::kKeyword;
    schema.fields["tags"] = tags;
    pmc::FieldDef vec;
    vec.name = "content_vec";
    vec.type = pmc::FieldType::kVector;
    vec.dims = 4;
    vec.indexed = false;
    schema.fields["content_vec"] = vec;
    return schema;
}

pmc::Document make_doc(std::string id, int64_t version) {
    pmc::Document doc;
    doc.id = std::move(id);
    doc.version = version;
    doc.fields["title"] = std::string("标题");
    doc.fields["tags"] = std::string("wiki");
    doc.fields["content_vec"] = std::vector<float>{0.1f, 0.2f, 0.3f, 0.4f};
    return doc;
}

} // namespace

TEST(CollectionTest, UpsertNewDocumentAssignsVersionAndDocid) {
    pmc::Collection collection("loom-kb", make_schema());
    auto result = collection.Upsert(make_doc("doc1", 0));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_GE(result.internal_docid, 0);
    EXPECT_EQ(result.superseded_docid, -1);

    pmc::Document stored;
    ASSERT_TRUE(collection.Get("doc1", &stored));
    EXPECT_GT(stored.version, 0);
    EXPECT_EQ(stored.internal_docid, result.internal_docid);
    EXPECT_EQ(std::get<std::string>(stored.fields.at("title")), "标题");
    EXPECT_EQ(collection.ActiveCount(), 1u);
}

TEST(CollectionTest, UpsertRejectsUnknownField) {
    pmc::Collection collection("loom-kb", make_schema());
    pmc::Document doc = make_doc("doc1", 0);
    doc.fields["nope"] = std::string("x");
    auto result = collection.Upsert(doc);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unknown field"), std::string::npos);
}

TEST(CollectionTest, UpsertRejectsTypeMismatch) {
    pmc::Collection collection("loom-kb", make_schema());
    pmc::Document doc = make_doc("doc1", 0);
    doc.fields["tags"] = 1.5; // keyword expects string
    EXPECT_FALSE(collection.Upsert(doc).ok);
}

TEST(CollectionTest, UpsertRejectsVectorDimsMismatch) {
    pmc::Collection collection("loom-kb", make_schema());
    pmc::Document doc = make_doc("doc1", 0);
    doc.fields["content_vec"] = std::vector<float>{0.1f, 0.2f};
    auto result = collection.Upsert(doc);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("dims mismatch"), std::string::npos);
}

TEST(CollectionTest, ReUpsertSupersedesPreviousDocid) {
    pmc::Collection collection("loom-kb", make_schema());
    const auto first = collection.Upsert(make_doc("doc1", 0));
    ASSERT_TRUE(first.ok);
    pmc::Document next = make_doc("doc1", 0);
    next.fields["title"] = std::string("新标题");
    const auto second = collection.Upsert(std::move(next));
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(second.superseded_docid, first.internal_docid);
    EXPECT_NE(second.internal_docid, first.internal_docid);
    EXPECT_EQ(collection.ActiveCount(), 1u);
    EXPECT_EQ(collection.TombstoneSnapshot().size(), 1u);

    pmc::Document stored;
    ASSERT_TRUE(collection.Get("doc1", &stored));
    EXPECT_EQ(std::get<std::string>(stored.fields.at("title")), "新标题");
    EXPECT_EQ(stored.internal_docid, second.internal_docid);
}

TEST(CollectionTest, ExplicitVersionFollowsLastWriteWins) {
    pmc::Collection collection("loom-kb", make_schema());
    ASSERT_TRUE(collection.Upsert(make_doc("doc1", 100)).ok);

    auto stale = collection.Upsert(make_doc("doc1", 99));
    EXPECT_FALSE(stale.ok);
    EXPECT_NE(stale.error.find("stale version"), std::string::npos);

    auto fresh = collection.Upsert(make_doc("doc1", 101));
    EXPECT_TRUE(fresh.ok);
}

TEST(CollectionTest, DeleteRemovesDocument) {
    pmc::Collection collection("loom-kb", make_schema());
    ASSERT_TRUE(collection.Upsert(make_doc("doc1", 0)).ok);
    EXPECT_TRUE(collection.Delete("doc1"));
    pmc::Document stored;
    EXPECT_FALSE(collection.Get("doc1", &stored));
    EXPECT_EQ(collection.ActiveCount(), 0u);
    EXPECT_FALSE(collection.Delete("doc1"));
}

TEST(CollectionTest, ForEachActiveSkipsSupersededAndDeleted) {
    pmc::Collection collection("loom-kb", make_schema());
    ASSERT_TRUE(collection.Upsert(make_doc("doc1", 0)).ok);
    ASSERT_TRUE(collection.Upsert(make_doc("doc1", 0)).ok); // supersede
    ASSERT_TRUE(collection.Upsert(make_doc("doc2", 0)).ok);
    ASSERT_TRUE(collection.Delete("doc2"));

    size_t visited = 0;
    collection.ForEachActive([&visited](const pmc::Document& doc) {
        EXPECT_EQ(doc.id, "doc1");
        ++visited;
    });
    EXPECT_EQ(visited, 1u);
}
