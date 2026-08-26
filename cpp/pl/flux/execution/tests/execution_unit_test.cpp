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

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/execution/task_executor.h"
#include "cpp/pl/flux/runtime/runtime_page.h"

namespace pl::flux::execution {
namespace {

// ---------------------------------------------------------------------------
// QueryMemoryContext
// ---------------------------------------------------------------------------

TEST(QueryMemoryContextTest, ReserveWithinLimitTracksUsageAndPeak) {
    QueryMemoryContext ctx(1000);

    ASSERT_TRUE(ctx.Reserve(400).ok());
    ASSERT_TRUE(ctx.Reserve(500).ok());

    const auto snapshot = ctx.Snapshot();
    EXPECT_EQ(900, snapshot.used_bytes);
    EXPECT_EQ(900, snapshot.peak_bytes);
    EXPECT_EQ(1000, snapshot.limit_bytes);
    EXPECT_FALSE(snapshot.limited);
}

TEST(QueryMemoryContextTest, FailedReserveRollsBackUsage) {
    QueryMemoryContext ctx(1000);
    ASSERT_TRUE(ctx.Reserve(800).ok());

    const auto status = ctx.Reserve(400);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(absl::StatusCode::kResourceExhausted, status.code());
    // The failed reservation must not hold quota: the remaining 200 bytes are
    // still available, while the `limited` flag records that the limit was hit.
    ASSERT_TRUE(ctx.Reserve(200).ok());
    const auto snapshot = ctx.Snapshot();
    EXPECT_EQ(1000, snapshot.used_bytes);
    EXPECT_TRUE(snapshot.limited);
}

TEST(QueryMemoryContextTest, ReleaseFreesQuotaForLaterReservations) {
    QueryMemoryContext ctx(1000);
    ASSERT_TRUE(ctx.Reserve(1000).ok());
    ASSERT_FALSE(ctx.Reserve(1).ok());

    ctx.Release(600);

    ASSERT_TRUE(ctx.Reserve(500).ok());
    EXPECT_EQ(900, ctx.Snapshot().used_bytes);
}

TEST(QueryMemoryContextTest, ReleaseSaturatesAtZero) {
    QueryMemoryContext ctx(1000);
    ASSERT_TRUE(ctx.Reserve(100).ok());

    ctx.Release(500);

    EXPECT_EQ(0, ctx.Snapshot().used_bytes);
}

// ---------------------------------------------------------------------------
// TaskExecutor
// ---------------------------------------------------------------------------

TEST(TaskExecutorTest, SubmitRunsTaskAndReturnsResult) {
    TaskExecutor executor(2);

    auto future = executor.Submit([] { return 42; });

    EXPECT_EQ(42, future.get());
}

TEST(TaskExecutorTest, RunsManyTasksAcrossWorkers) {
    TaskExecutor executor(4);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(100);

    for (int i = 0; i < 100; ++i) {
        futures.push_back(executor.Submit([&counter] { counter.fetch_add(1); }));
    }
    for (auto& future : futures) {
        future.get();
    }

    EXPECT_EQ(100, counter.load());
}

TEST(TaskExecutorTest, SubmitAfterShutdownThrows) {
    TaskExecutor executor(1);
    executor.Shutdown();

    EXPECT_THROW((void)executor.Submit([] {}), std::runtime_error);
    // Shutdown is idempotent and the destructor handles a second call.
    executor.Shutdown();
}

// ---------------------------------------------------------------------------
// ExchangeBuffer
// ---------------------------------------------------------------------------

runtime::Page MakeTestPage(size_t rows) {
    std::vector<std::shared_ptr<runtime::ObjectValue>> row_objects;
    row_objects.reserve(rows);
    for (size_t i = 0; i < rows; ++i) {
        row_objects.push_back(std::make_shared<runtime::ObjectValue>(
            std::vector<std::pair<std::string, runtime::Value>>{
                {"v", runtime::Value::integer(static_cast<int64_t>(i))}}));
    }
    return runtime::PageFromRows("test", std::move(row_objects));
}

TEST(ExchangeBufferTest, AddAndPopPagesInOrder) {
    ExchangeBuffer buffer;

    ASSERT_TRUE(buffer.AddPage(MakeTestPage(2)).ok());
    ASSERT_TRUE(buffer.AddPage(MakeTestPage(3)).ok());
    EXPECT_EQ(2, buffer.page_count());
    EXPECT_EQ(5, buffer.row_count());

    auto first = buffer.PopPage();
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ(2, (*first)->row_count());

    auto second = buffer.PopPage();
    ASSERT_TRUE(second.ok()) << second.status();
    ASSERT_TRUE(second->has_value());
    EXPECT_EQ(3, (*second)->row_count());
    EXPECT_EQ(0, buffer.page_count());
}

TEST(ExchangeBufferTest, AddPageBlocksWhenFullUntilConsumerPops) {
    ExchangeBuffer buffer(/*max_pages=*/2);
    ASSERT_TRUE(buffer.AddPage(MakeTestPage(1)).ok());
    ASSERT_TRUE(buffer.AddPage(MakeTestPage(1)).ok());

    std::atomic<bool> third_added{false};
    std::thread producer([&] {
        const auto status = buffer.AddPage(MakeTestPage(1));
        third_added.store(status.ok());
    });

    // Backpressure: the buffer is full, so the producer must still be blocked.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(third_added.load());
    EXPECT_EQ(2, buffer.page_count());

    auto popped = buffer.PopPage();
    ASSERT_TRUE(popped.ok()) << popped.status();
    producer.join();
    EXPECT_TRUE(third_added.load());
    EXPECT_EQ(2, buffer.page_count());
}

TEST(ExchangeBufferTest, PopReturnsNulloptOnlyAfterAllProducersFinish) {
    ExchangeBuffer buffer;
    buffer.SetProducerCount(2);

    ASSERT_TRUE(buffer.Finish().ok());
    EXPECT_FALSE(buffer.finished());
    ASSERT_TRUE(buffer.Finish().ok());
    EXPECT_TRUE(buffer.finished());

    auto page = buffer.PopPage();
    ASSERT_TRUE(page.ok()) << page.status();
    EXPECT_FALSE(page->has_value());

    // Adding after finish is an error.
    EXPECT_FALSE(buffer.AddPage(MakeTestPage(1)).ok());
}

TEST(ExchangeBufferTest, MarkErrorFailsPendingConsumersAndProducers) {
    ExchangeBuffer buffer;

    buffer.MarkError(absl::InternalError("boom"));

    auto popped = buffer.PopPage();
    ASSERT_FALSE(popped.ok());
    EXPECT_EQ(absl::StatusCode::kFailedPrecondition, popped.status().code());
    EXPECT_FALSE(buffer.AddPage(MakeTestPage(1)).ok());
}

TEST(ExchangeBufferTest, CloseFailsProducers) {
    ExchangeBuffer buffer;

    buffer.Close();

    EXPECT_TRUE(buffer.closed());
    EXPECT_FALSE(buffer.AddPage(MakeTestPage(1)).ok());
}

TEST(ExchangeBufferTest, TransfersPagesBetweenProducerAndConsumerThreads) {
    ExchangeBuffer buffer(/*max_pages=*/4);
    constexpr int kPages = 50;

    std::atomic<bool> producer_ok{true};
    std::thread producer([&] {
        for (int i = 0; i < kPages && producer_ok.load(); ++i) {
            producer_ok.store(buffer.AddPage(MakeTestPage(2)).ok());
        }
        producer_ok.store(producer_ok.load() && buffer.Finish().ok());
    });

    size_t rows = 0;
    int pages = 0;
    for (;;) {
        auto next = buffer.PopPage();
        ASSERT_TRUE(next.ok()) << next.status();
        if (!next->has_value()) {
            break;
        }
        ++pages;
        rows += (*next)->row_count();
    }
    producer.join();

    EXPECT_TRUE(producer_ok.load());
    EXPECT_EQ(kPages, pages);
    EXPECT_EQ(kPages * 2, rows);
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

class StaticOperator final : public Operator {
public:
    explicit StaticOperator(std::vector<runtime::Page> pages) : pages_(std::move(pages)) {}

    [[nodiscard]] std::string name() const override { return "StaticOperator"; }

    absl::StatusOr<std::optional<runtime::Page>> NextPage() override {
        if (next_ >= pages_.size()) {
            return std::nullopt;
        }
        return pages_[next_++];
    }

private:
    std::vector<runtime::Page> pages_;
    size_t next_ = 0;
};

class FailingOperator final : public Operator {
public:
    [[nodiscard]] std::string name() const override { return "FailingOperator"; }

    absl::StatusOr<std::optional<runtime::Page>> NextPage() override {
        return absl::InternalError("static failure");
    }
};

Pipeline MakeStaticPipeline(std::string id, std::string role, std::vector<runtime::Page> pages) {
    Pipeline pipeline;
    pipeline.id = std::move(id);
    pipeline.name = pipeline.id;
    pipeline.role = std::move(role);
    pipeline.root = std::make_unique<StaticOperator>(std::move(pages));
    return pipeline;
}

TEST(SchedulerTest, RejectsEmptyTask) {
    ExecutionTask task;

    auto result_or = Scheduler().Run(std::move(task));

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result_or.status().code());
}

TEST(SchedulerTest, RejectsMissingDependency) {
    ExecutionTask task;
    auto pipeline = MakeStaticPipeline("main", "root", {MakeTestPage(1)});
    pipeline.dependencies = {"missing"};
    task.pipelines.push_back(std::move(pipeline));

    auto result_or = Scheduler().Run(std::move(task));

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result_or.status().code());
}

TEST(SchedulerTest, RejectsDependencyCycle) {
    ExecutionTask task;
    auto left = MakeStaticPipeline("left", "producer", {});
    left.dependencies = {"right"};
    auto right = MakeStaticPipeline("right", "producer", {});
    right.dependencies = {"left"};
    task.pipelines.push_back(std::move(left));
    task.pipelines.push_back(std::move(right));

    auto result_or = Scheduler().Run(std::move(task));

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kFailedPrecondition, result_or.status().code());
}

TEST(SchedulerTest, RunsSinglePipelineAndCollectsPages) {
    ExecutionTask task;
    task.pipelines.push_back(
        MakeStaticPipeline("main", "root", {MakeTestPage(2), MakeTestPage(3)}));

    auto result_or = Scheduler().Run(std::move(task));

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(runtime::Value::Type::Table, result_or->type());
    EXPECT_EQ(5, result_or->as_table().rows.size());
}

TEST(SchedulerTest, PropagatesOperatorFailure) {
    ExecutionTask task;
    Pipeline pipeline;
    pipeline.id = "main";
    pipeline.name = "main";
    pipeline.role = "root";
    pipeline.root = std::make_unique<FailingOperator>();
    task.pipelines.push_back(std::move(pipeline));

    auto result_or = Scheduler().Run(std::move(task));

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInternal, result_or.status().code());
}

} // namespace
} // namespace pl::flux::execution
