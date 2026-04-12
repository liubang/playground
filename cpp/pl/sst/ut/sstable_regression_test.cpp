// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/sst/sstable_builder.h"
#include "cpp/pl/sst/sstable_iterator.h"
#include "cpp/pl/sst/sstable_version_manager.h"
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pl {
namespace {

class MockIterator final : public Iterator {
public:
    explicit MockIterator(std::vector<CellRef> cells, Status status = Status::OK)
        : cells_(std::move(cells)), status_(std::move(status)) {}

    void first() override { index_ = cells_.empty() ? kInvalidIndex : 0; }

    void last() override { index_ = cells_.empty() ? kInvalidIndex : cells_.size() - 1; }

    void next() override {
        if (index_ == kInvalidIndex) {
            return;
        }
        ++index_;
        if (index_ >= cells_.size()) {
            index_ = kInvalidIndex;
            if (status_.isOk()) {
                status_ = Status(StatusCode::kKVStoreNotFound);
            }
        }
    }

    void prev() override {
        if (index_ == kInvalidIndex) {
            return;
        }
        if (index_ == 0) {
            index_ = kInvalidIndex;
            if (status_.isOk()) {
                status_ = Status(StatusCode::kKVStoreNotFound);
            }
            return;
        }
        --index_;
    }

    void seek(std::string_view target) override {
        index_ = kInvalidIndex;
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            if (cells_[i]->rowkey() >= target) {
                index_ = i;
                return;
            }
        }
        if (status_.isOk()) {
            status_ = Status(StatusCode::kKVStoreNotFound);
        }
    }

    [[nodiscard]] Status status() const override { return status_; }

    [[nodiscard]] bool valid() const override { return index_ != kInvalidIndex; }

    [[nodiscard]] CellRef cell() const override { return valid() ? cells_[index_] : nullptr; }

private:
    static constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

    std::vector<CellRef> cells_;
    Status status_;
    std::size_t index_{kInvalidIndex};
};

[[nodiscard]] CellRef makeCell(std::string_view rowkey, std::string_view value) {
    return std::make_shared<Cell>(CellType::CT_PUT, rowkey, "cf", "col", value, 1);
}

[[nodiscard]] std::filesystem::path makeTempDir(std::string_view name) {
    auto dir = std::filesystem::temp_directory_path() / std::filesystem::path(std::string(name));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "MAJOR");
    return dir;
}

[[nodiscard]] SSTableRef buildAndOpenTable(const std::filesystem::path& root,
                                           CompressionType compression,
                                           SSTId sst_id,
                                           const std::vector<CellRef>& cells = {
                                               makeCell("rk", "value")},
                                           std::size_t block_size = 4096,
                                           FilterPolicyType filter_type = FilterPolicyType::NONE,
                                           PatchId patch_id = 0) {
    auto build_options = std::make_shared<BuildOptions>();
    build_options->data_dir = root;
    build_options->block_size = block_size;
    build_options->compression_type = compression;
    build_options->sst_type = SSTType::MAJOR;
    build_options->sst_version = SSTVersion::V1;
    build_options->filter_type = filter_type;
    build_options->patch_id = patch_id;
    build_options->sst_id = sst_id;

    SSTableBuilder builder(build_options);
    auto open_result = builder.open();
    EXPECT_TRUE(open_result.hasValue()) << open_result.error().describe();
    for (const auto& cell : cells) {
        if (cell == nullptr) {
            ADD_FAILURE() << "test input contains null cell";
            return {};
        }
        EXPECT_TRUE(builder.add(*cell).hasValue());
    }

    auto finish_result = builder.finish();
    EXPECT_TRUE(finish_result.hasValue()) << finish_result.error().describe();

    auto table_result = SSTable::open(std::make_shared<ReadOptions>(),
                                      root / "MAJOR" / (std::to_string(sst_id) + ".sst"));
    EXPECT_TRUE(table_result.hasValue()) << table_result.error().describe();
    return table_result.hasValue() ? SSTableRef(std::move(table_result).value()) : SSTableRef{};
}

TEST(SSTableIteratorRegressionTest, ForwardScanStopsOnCorruptedDataBlock) {
    std::vector<CellRef> index_cells;
    index_cells.emplace_back(makeCell("a", "good-1"));
    index_cells.emplace_back(makeCell("b", "bad"));
    index_cells.emplace_back(makeCell("c", "good-2"));

    SSTableIterator iter(
        std::make_unique<MockIterator>(index_cells), [](std::string_view handle) -> IteratorPtr {
            if (handle == "good-1") {
                return std::make_unique<MockIterator>(std::vector<CellRef>{makeCell("a", "v1")});
            }
            if (handle == "good-2") {
                return std::make_unique<MockIterator>(std::vector<CellRef>{makeCell("c", "v2")});
            }
            return std::make_unique<MockIterator>(
                std::vector<CellRef>{},
                Status(StatusCode::kDataCorruption, "corrupted data block"));
        });

    iter.first();
    ASSERT_TRUE(iter.valid());
    EXPECT_EQ("a", iter.cell()->rowkey());

    iter.next();

    EXPECT_FALSE(iter.valid());
    EXPECT_EQ(StatusCode::kDataCorruption, iter.status().code());
}

TEST(SSTableIteratorRegressionTest, BackwardScanStopsOnCorruptedDataBlock) {
    std::vector<CellRef> index_cells;
    index_cells.emplace_back(makeCell("a", "good-1"));
    index_cells.emplace_back(makeCell("b", "bad"));
    index_cells.emplace_back(makeCell("c", "good-2"));

    SSTableIterator iter(
        std::make_unique<MockIterator>(index_cells), [](std::string_view handle) -> IteratorPtr {
            if (handle == "good-1") {
                return std::make_unique<MockIterator>(std::vector<CellRef>{makeCell("a", "v1")});
            }
            if (handle == "good-2") {
                return std::make_unique<MockIterator>(std::vector<CellRef>{makeCell("c", "v2")});
            }
            return std::make_unique<MockIterator>(
                std::vector<CellRef>{},
                Status(StatusCode::kDataCorruption, "corrupted data block"));
        });

    iter.last();
    ASSERT_TRUE(iter.valid());
    EXPECT_EQ("c", iter.cell()->rowkey());

    iter.prev();

    EXPECT_FALSE(iter.valid());
    EXPECT_EQ(StatusCode::kDataCorruption, iter.status().code());
}

TEST(SSTableIteratorRegressionTest, SeekSkipsEmptyDataBlocksAndFindsNextRow) {
    std::vector<CellRef> index_cells;
    index_cells.emplace_back(makeCell("a", "good-1"));
    index_cells.emplace_back(makeCell("b", "empty"));
    index_cells.emplace_back(makeCell("c", "good-2"));

    SSTableIterator iter(
        std::make_unique<MockIterator>(index_cells), [](std::string_view handle) -> IteratorPtr {
            if (handle == "good-1") {
                return std::make_unique<MockIterator>(std::vector<CellRef>{makeCell("a", "v1")});
            }
            if (handle == "good-2") {
                return std::make_unique<MockIterator>(std::vector<CellRef>{makeCell("c", "v2")});
            }
            return std::make_unique<MockIterator>(std::vector<CellRef>{});
        });

    iter.seek("b");

    ASSERT_TRUE(iter.valid());
    EXPECT_EQ("c", iter.cell()->rowkey());
    EXPECT_EQ(StatusCode::kOK, iter.status().code());
}

TEST(SSTableBuilderRegressionTest, ISALCompressionFailsFastWithoutWritingMislabelledBlocks) {
    const auto root = makeTempDir("pl_sst_isal_regression");

    auto build_options = std::make_shared<BuildOptions>();
    build_options->data_dir = root;
    build_options->compression_type = CompressionType::ISAL;
    build_options->sst_type = SSTType::MAJOR;
    build_options->sst_version = SSTVersion::V1;
    build_options->filter_type = FilterPolicyType::NONE;
    build_options->sst_id = 7;

    SSTableBuilder builder(build_options);
    auto open_result = builder.open();
    ASSERT_TRUE(open_result.hasValue()) << open_result.error().describe();
    ASSERT_TRUE(builder.add(Cell(CellType::CT_PUT, "rk", "cf", "col", "value", 1)).hasValue());

    auto finish_result = builder.finish();
    ASSERT_TRUE(finish_result.hasError());
    EXPECT_EQ(StatusCode::kNotImplemented, finish_result.error().code());

    std::filesystem::remove_all(root);
}

TEST(SSTableBuilderRegressionTest, RealTableRoundTripsAcrossBlocksWithCompressionAndFilter) {
    const auto root = makeTempDir("pl_sst_roundtrip_regression");
    const std::vector<CellRef> cells = {
        std::make_shared<Cell>(CellType::CT_PUT, "row-001", "cf", "col-a", "value-a2", 2),
        std::make_shared<Cell>(CellType::CT_PUT, "row-001", "cf", "col-b", "value-b1", 1),
        std::make_shared<Cell>(CellType::CT_PUT, "row-002", "cf", "col-a", std::string(48, '2'), 2),
        std::make_shared<Cell>(CellType::CT_PUT, "row-003", "cf", "col-a", std::string(48, '3'), 3),
        std::make_shared<Cell>(CellType::CT_PUT, "row-004", "cf", "col-a", std::string(48, '4'), 4),
    };
    auto table = buildAndOpenTable(root,
                                   CompressionType::SNAPPY,
                                   21,
                                   cells,
                                   80,
                                   FilterPolicyType::STANDARD_BLOOM_FILTER,
                                   9);
    ASSERT_NE(table, nullptr);

    EXPECT_EQ(21, table->sstId());
    EXPECT_EQ(9, table->patchId());
    ASSERT_NE(table->fileMeta(), nullptr);
    EXPECT_EQ(5, table->fileMeta()->cellNum());
    EXPECT_EQ(4, table->fileMeta()->rowNum());
    EXPECT_EQ("row-001", table->fileMeta()->minKey());
    EXPECT_LE(0, BytewiseComparator().compare(table->fileMeta()->maxKey(), "row-004"));

    Arena arena;
    auto get_result = table->get("row-001", &arena);
    ASSERT_TRUE(get_result.hasValue()) << get_result.error().describe();
    const auto& row = get_result.value();
    ASSERT_EQ(2, row.size());
    EXPECT_EQ("col-a", row[0]->col());
    EXPECT_EQ("value-a2", row[0]->value());
    EXPECT_EQ("col-b", row[1]->col());
    EXPECT_EQ("value-b1", row[1]->value());

    auto miss_result = table->get("row-999", &arena);
    ASSERT_TRUE(miss_result.hasError());
    EXPECT_EQ(StatusCode::kKVStoreNotFound, miss_result.error().code());

    auto iter = table->iterator();
    std::vector<std::string> rowkeys;
    for (iter->first(); iter->valid(); iter->next()) {
        rowkeys.emplace_back(iter->cell()->rowkey());
    }
    EXPECT_EQ((std::vector<std::string>{"row-001", "row-001", "row-002", "row-003", "row-004"}),
              rowkeys);

    iter->seek("row-002");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ("row-002", iter->cell()->rowkey());
    iter->next();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ("row-003", iter->cell()->rowkey());

    std::filesystem::remove_all(root);
}

TEST(SSTableVersionManagerRegressionTest, FirstEditStartsFromEmptyVersion) {
    const auto root = makeTempDir("pl_sst_version_manager_regression");
    auto table = buildAndOpenTable(root, CompressionType::NONE, 11);
    ASSERT_NE(table, nullptr);

    SSTableVersionManager version_manager;
    auto edit = std::make_shared<SSTableVersionEdit>();
    edit->addSSTable(table);

    version_manager.applyVersionEdit(edit);

    ASSERT_NE(version_manager.current_, nullptr);
    const auto tables = version_manager.current_->listSSTables();
    ASSERT_EQ(1, tables.size());
    EXPECT_EQ(table->sstId(), tables.front()->sstId());

    std::filesystem::remove_all(root);
}

TEST(SSTableVersionManagerRegressionTest, AppliesAddAndDeleteEditsInPatchOrder) {
    const auto root = makeTempDir("pl_sst_version_manager_patch_order");
    auto old_table = buildAndOpenTable(root, CompressionType::NONE, 31, {}, 4096,
                                       FilterPolicyType::NONE, 1);
    auto mid_table = buildAndOpenTable(root, CompressionType::NONE, 32, {}, 4096,
                                       FilterPolicyType::NONE, 2);
    auto new_table = buildAndOpenTable(root, CompressionType::NONE, 33, {}, 4096,
                                       FilterPolicyType::NONE, 3);
    ASSERT_NE(old_table, nullptr);
    ASSERT_NE(mid_table, nullptr);
    ASSERT_NE(new_table, nullptr);

    SSTableVersionManager version_manager;

    auto first_edit = std::make_shared<SSTableVersionEdit>();
    first_edit->addSSTable(old_table);
    first_edit->addSSTable(mid_table);
    version_manager.applyVersionEdit(first_edit);

    auto second_edit = std::make_shared<SSTableVersionEdit>();
    second_edit->addSSTable(new_table);
    second_edit->delSSTable(mid_table->sstId());
    version_manager.applyVersionEdit(second_edit);

    ASSERT_NE(version_manager.current_, nullptr);
    const auto tables = version_manager.current_->listSSTables();
    ASSERT_EQ(2, tables.size());
    EXPECT_EQ(33, tables[0]->sstId());
    EXPECT_EQ(31, tables[1]->sstId());
    EXPECT_EQ(3, tables[0]->patchId());
    EXPECT_EQ(1, tables[1]->patchId());

    std::filesystem::remove_all(root);
}

} // namespace
} // namespace pl
