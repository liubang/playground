// Copyright (c) 2026 The Authors. All rights reserved.
#include "cpp/pl/sstv2/merge/merge_iterator.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace pl::sstv2::merge {
namespace {

class VectorCursor final : public ForwardCursor {
public:
    explicit VectorCursor(std::vector<std::pair<std::string, std::string>> entries,
                          size_t fail_on_next = SIZE_MAX)
        : entries_(std::move(entries)), fail_on_next_(fail_on_next) {}

    absl::Status seek_to_first() override {
        index_ = 0;
        return absl::OkStatus();
    }
    absl::Status seek(std::string_view key) override {
        index_ = static_cast<size_t>(std::lower_bound(
                                         entries_.begin(),
                                         entries_.end(),
                                         key,
                                         [](const auto& entry, std::string_view target) {
                                             return entry.first < target;
                                         }) -
                                     entries_.begin());
        return absl::OkStatus();
    }
    absl::Status next() override {
        if (index_ == fail_on_next_) {
            return absl::DataLossError("injected cursor failure");
        }
        if (index_ < entries_.size()) {
            ++index_;
        }
        return absl::OkStatus();
    }
    bool valid() const override { return index_ < entries_.size(); }
    std::string_view key() const override {
        return valid() ? std::string_view(entries_[index_].first) : std::string_view{};
    }
    std::string_view value() const override {
        return valid() ? std::string_view(entries_[index_].second) : std::string_view{};
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
    size_t index_ = 0;
    size_t fail_on_next_;
};

Source source(std::vector<std::pair<std::string, std::string>> entries,
              uint32_t priority,
              size_t fail_on_next = SIZE_MAX) {
    return Source{.cursor = std::make_unique<VectorCursor>(std::move(entries), fail_on_next),
                  .priority = priority};
}

TEST(MergeIteratorTest, MergesInterleavedSourcesAndPreservesDuplicates) {
    std::vector<Source> sources;
    sources.push_back(source({{"a", "a0"}, {"c", "c0"}}, 0));
    sources.push_back(source({{"b", "b1"}, {"c", "c1"}, {"d", "d1"}}, 1));
    MergeIterator it(std::move(sources));

    ASSERT_TRUE(it.seek_to_first().ok());
    std::vector<std::string> rows;
    while (it.valid()) {
        rows.emplace_back(std::string(it.key()) + ":" + std::string(it.value()));
        ASSERT_TRUE(it.next().ok());
    }
    EXPECT_EQ(rows, (std::vector<std::string>{"a:a0", "b:b1", "c:c0", "c:c1", "d:d1"}));
}

TEST(MergeIteratorTest, SeekUsesLowerBoundAcrossAllSources) {
    std::vector<Source> sources;
    sources.push_back(source({{"a", "1"}, {"e", "5"}}, 0));
    sources.push_back(source({{"b", "2"}, {"d", "4"}}, 1));
    MergeIterator it(std::move(sources));

    ASSERT_TRUE(it.seek("c").ok());
    ASSERT_TRUE(it.valid());
    EXPECT_EQ(it.key(), "d");
    ASSERT_TRUE(it.next().ok());
    ASSERT_TRUE(it.valid());
    EXPECT_EQ(it.key(), "e");
}

TEST(MergeIteratorTest, EmptySourcesAndPastEndAreCleanlyExhausted) {
    MergeIterator empty({});
    EXPECT_TRUE(empty.seek_to_first().ok());
    EXPECT_FALSE(empty.valid());

    std::vector<Source> sources;
    sources.push_back(source({{"a", "1"}}, 0));
    MergeIterator it(std::move(sources));
    EXPECT_TRUE(it.seek("z").ok());
    EXPECT_FALSE(it.valid());
    EXPECT_EQ(it.next().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(MergeIteratorTest, ChildErrorIsSticky) {
    std::vector<Source> sources;
    sources.push_back(source({{"a", "1"}, {"b", "2"}}, 0, 0));
    MergeIterator it(std::move(sources));
    ASSERT_TRUE(it.seek_to_first().ok());
    const auto status = it.next();
    EXPECT_EQ(status.code(), absl::StatusCode::kDataLoss);
    EXPECT_FALSE(it.valid());
    EXPECT_EQ(it.next(), status);
}

TEST(MergeIteratorTest, RejectsNullCursor) {
    std::vector<Source> sources;
    sources.push_back(Source{.cursor = nullptr, .priority = 0});
    MergeIterator it(std::move(sources));
    EXPECT_EQ(it.seek_to_first().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace pl::sstv2::merge
