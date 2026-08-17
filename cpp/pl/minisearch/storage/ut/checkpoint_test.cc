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

// CheckpointStore round-trip tests: document/vector persistence, tombstone
// compaction (deleted and superseded documents are absent from the snapshot),
// seq rolling retention, registry-level restore and the inverted-index
// rebuild that follows a restore.

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <json2pb/json_to_pb.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "cpp/pl/minisearch/server/collection_registry.h"
#include "cpp/pl/minisearch/storage/checkpoint.h"

namespace pms = pl::minisearch::storage;
namespace pmc = pl::minisearch::core;
namespace pmsrv = pl::minisearch::server;
namespace pmp = pl::minisearch::proto;

namespace {

const char* kSpecJson = R"({"name":"kb","default_analyzer":"cjk_jieba","fields":[)"
                        R"({"name":"title","type":"text","indexed":true,"stored":true},)"
                        R"({"name":"vec","type":"vector","indexed":false,"stored":true,"dims":4,)"
                        R"("metric":"cosine","mode":"client"}]})";

pmc::Document make_doc(const std::string& id, int64_t version, float x) {
    pmc::Document doc;
    doc.id = id;
    doc.version = version;
    doc.fields["title"] = "标题-" + id;
    doc.fields["vec"] = std::vector<float>{x, 0.0f, 0.0f, 0.0f};
    return doc;
}

class CheckpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::string(getenv("TEST_TMPDIR") ? getenv("TEST_TMPDIR") : "/tmp");
        root_ += "/minisearch_checkpoint_test_XXXXXX";
        std::vector<char> buf(root_.begin(), root_.end());
        buf.push_back('\0');
        ASSERT_NE(nullptr, ::mkdtemp(buf.data()));
        root_ = buf.data();
        std::string err;
        ASSERT_TRUE(json2pb::JsonToProtoMessage(kSpecJson, &spec_, &err)) << err;
    }

    void TearDown() override {
        std::string cmd = "rm -rf '" + root_ + "'";
        ASSERT_EQ(0, system(cmd.c_str()));
    }

    std::unique_ptr<pmsrv::CollectionRegistry> make_registry() {
        return std::make_unique<pmsrv::CollectionRegistry>(
            std::make_unique<pms::CheckpointStore>(root_));
    }

    std::string root_;
    pmp::CollectionSpec spec_;
};

TEST_F(CheckpointTest, SaveLoadRoundTrip) {
    pmsrv::CollectionRegistry registry; // 纯内存：手动驱动 store.Save
    ASSERT_TRUE(registry.Create(spec_).ok);

    auto entry = registry.Find("kb");
    ASSERT_TRUE(entry->docs.Upsert(make_doc("doc1", 1, 1.0f)).ok);
    ASSERT_TRUE(entry->docs.Upsert(make_doc("doc2", 1, 0.0f)).ok);
    // supersede doc1 and delete doc2: both must be absent from the snapshot
    ASSERT_TRUE(entry->docs.Upsert(make_doc("doc1", 2, 0.5f)).ok);
    ASSERT_TRUE(entry->docs.Delete("doc2"));
    pmc::Document current;
    ASSERT_TRUE(entry->docs.Get("doc1", &current));

    pms::CheckpointStore store(root_);
    ASSERT_TRUE(store.Save("kb", spec_, "Flat", entry->docs, entry->index.get()));

    auto loaded = store.Load("kb");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->documents.size(), 1u); // tombstones compacted away
    EXPECT_EQ(loaded->documents[0].id, "doc1");
    EXPECT_EQ(loaded->documents[0].version, 2);
    EXPECT_EQ(loaded->documents[0].internal_docid, current.internal_docid);
    EXPECT_FALSE(loaded->faiss_path.empty());
}

TEST_F(CheckpointTest, RegistryRestoreAcrossInstances) {
    {
        auto registry = make_registry();
        ASSERT_TRUE(registry->Create(spec_).ok);
        auto entry = registry->Find("kb");
        // Mimic the HTTP write path: core upsert + vector/inverted index writes.
        auto put = [&](const pmc::Document& doc, float x) {
            auto result = entry->docs.Upsert(doc);
            EXPECT_TRUE(result.ok);
            const float vec[4] = {x, 0.0f, 0.0f, 0.0f};
            entry->index->add(result.internal_docid, vec);
            entry->IndexText(result.internal_docid, doc);
            return result.internal_docid;
        };
        put(make_doc("doc1", 1, 1.0f), 1.0f);
        put(make_doc("gone", 1, 1.0f), 1.0f);
        ASSERT_TRUE(entry->docs.Delete("gone"));
        ASSERT_TRUE(registry->Checkpoint("kb"));
    }

    auto registry2 = make_registry();
    EXPECT_EQ(registry2->LoadFromDisk(), 1u);

    pmc::Document doc;
    ASSERT_TRUE(registry2->Find("kb")->docs.Get("doc1", &doc));
    const int64_t restored_doc1 = doc.internal_docid;
    EXPECT_EQ(doc.fields.size(), 2u);
    EXPECT_EQ(std::get<std::string>(doc.fields.at("title")), "标题-doc1");
    // deleted document stays gone
    EXPECT_FALSE(registry2->Find("kb")->docs.Get("gone", &doc));

    // docid high-water mark survives: a new upsert must not reuse ids
    auto upserted = registry2->Find("kb")->docs.Upsert(make_doc("fresh", 1, 1.0f));
    ASSERT_TRUE(upserted.ok);
    EXPECT_GT(upserted.internal_docid, restored_doc1);
    const float fresh_vec[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    registry2->Find("kb")->index->add(upserted.internal_docid, fresh_vec);

    // vector search works on the restored index (doc1 and fresh share x=1.0)
    const float query[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    auto hits = registry2->Find("kb")->index->search(query, 5);
    ASSERT_FALSE(hits.empty());
    bool found = false;
    for (const auto& hit : hits) {
        found = found || hit.id == restored_doc1 || hit.id == upserted.internal_docid;
    }
    EXPECT_TRUE(found);

    // 倒排索引在恢复时被重建：BM25 路在重启后立即可用，
    // 且 tombstone（gone / 旧版本）不会出现在 posting 里。
    auto restored_entry = registry2->Find("kb");
    const auto postings = restored_entry->inverted->Find("标题");
    ASSERT_EQ(postings.size(), 1u);
    EXPECT_EQ(postings[0].docid, restored_doc1);
    EXPECT_EQ(restored_entry->inverted->DocCount(), 1);
}

TEST_F(CheckpointTest, RollingRetention) {
    auto registry = make_registry();
    ASSERT_TRUE(registry->Create(spec_).ok);
    auto entry = registry->Find("kb");

    for (int i = 0; i < 3; ++i) {
        entry->docs.Upsert(make_doc("doc1", 2 + i, 1.0f));
        ASSERT_TRUE(registry->Checkpoint("kb"));
    }
    // seq 1..4 written (Create persists seq 1); retention keeps the newest two
    std::string listing;
    ASSERT_EQ(0, system(("ls '" + root_ + "/kb' > '" + root_ + "/ls.txt'").c_str()));
    std::ifstream ls(root_ + "/ls.txt");
    int checkpoint_files = 0;
    std::string line;
    while (std::getline(ls, line)) {
        if (line.find("checkpoint.") == 0) {
            ++checkpoint_files;
        }
    }
    EXPECT_EQ(checkpoint_files, 4); // 2 seq x (docs + faiss)
}

TEST_F(CheckpointTest, DropRemovesDirectory) {
    auto registry = make_registry();
    ASSERT_TRUE(registry->Create(spec_).ok);
    ASSERT_TRUE(registry->Drop("kb"));
    pms::CheckpointStore reader(root_);
    EXPECT_TRUE(reader.ListCollections().empty());
}

TEST_F(CheckpointTest, InvalidCollectionNameRejected) {
    pmsrv::CollectionRegistry registry;
    pmp::CollectionSpec bad = spec_;
    bad.set_name("../escape");
    auto result = registry.Create(bad);
    EXPECT_FALSE(result.ok);
    bad.set_name("with space");
    EXPECT_FALSE(registry.Create(bad).ok);
}

} // namespace
