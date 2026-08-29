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
// Created: 2026/08/29 22:15

#include <cmath>
#include <cstring>
#include <memory>

#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/shape.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "gtest/gtest.h"

namespace pl::mllm {
namespace {

TEST(StatusTest, OkAndError) {
    const Status ok{};
    EXPECT_TRUE(ok.ok());

    const auto err = Status::Error(ErrorCode::kNotFound, "missing");
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code, ErrorCode::kNotFound);
    EXPECT_EQ(err.message, "missing");
}

TEST(ResultTest, ValueChannel) {
    Result<int> r = 42;
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(std::move(r).value(), 42);
}

TEST(ResultTest, ErrorChannel) {
    Result<int> r = Status::Error(ErrorCode::kInvalidArgument, "bad");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code, ErrorCode::kInvalidArgument);
}

TEST(ResultTest, OkStatusBecomesInternalError) {
    Result<int> r = Status{};
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code, ErrorCode::kInternal);
}

TEST(ResultTest, MoveOnlyValue) {
    Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
    ASSERT_TRUE(r.ok());
    auto ptr = std::move(r).value();
    EXPECT_EQ(*ptr, 7);
}

TEST(ShapeTest, BasicDims) {
    const Shape s({2, 3, 4});
    EXPECT_EQ(s.rank(), 3);
    EXPECT_EQ(s.dim(0), 2);
    EXPECT_EQ(s.dim(2), 4);
    EXPECT_EQ(s.dim(3), 0); // out of rank
    EXPECT_EQ(s.dim(-1), 0);
    EXPECT_EQ(s.numel(), 24);
    EXPECT_EQ(s.dims().size(), 3u);
}

TEST(ShapeTest, RankOverflowRejected) {
    const std::array<int64_t, 5> dims{1, 2, 3, 4, 5};
    const Shape s{std::span<const int64_t>(dims)};
    EXPECT_EQ(s.rank(), 0);
    EXPECT_TRUE(s.empty());
}

TEST(ShapeTest, NegativeDimRejected) {
    const Shape s({4, -1, 2});
    EXPECT_EQ(s.rank(), 0);
}

TEST(ShapeTest, NumelOverflow) {
    const Shape s({INT64_MAX, 2});
    EXPECT_EQ(s.numel(), -1);
}

TEST(ShapeTest, ZeroDim) {
    const Shape s({0, 8});
    EXPECT_EQ(s.numel(), 0);
    EXPECT_TRUE(s.empty());
}

TEST(DTypeTest, ByteSizes) {
    EXPECT_EQ(dtype_nbytes(DType::kF32, 8), 32u);
    EXPECT_EQ(dtype_nbytes(DType::kF16, 8), 16u);
    EXPECT_EQ(dtype_nbytes(DType::kQ8_0, 32), 34u);
    EXPECT_EQ(dtype_nbytes(DType::kQ8_0, 33), 0u); // not block-aligned
    EXPECT_EQ(dtype_nbytes(DType::kQ4_0, 32), 18u);
    EXPECT_TRUE(is_quantized(DType::kQ8_0));
    EXPECT_FALSE(is_quantized(DType::kF16));
}

TEST(Fp16Test, RoundTrip) {
    for (const float v : {0.0f, 1.0f, -1.0f, 0.5f, 65504.0f, -65504.0f, 1e-3f, 3.14159f}) {
        const uint16_t h = fp32_to_fp16(v);
        const float back = fp16_to_fp32(h);
        const float tol = std::fabs(v) * 1e-3f + 1e-6f;
        EXPECT_NEAR(back, v, tol) << "v=" << v;
    }
}

TEST(Fp16Test, SpecialValues) {
    EXPECT_EQ(fp16_to_fp32(fp32_to_fp16(0.0f)), 0.0f);
    const uint16_t inf = 0x7C00;
    EXPECT_TRUE(std::isinf(fp16_to_fp32(inf)));
    const uint16_t nan = 0x7E00;
    EXPECT_TRUE(std::isnan(fp16_to_fp32(nan)));
    EXPECT_TRUE(std::isinf(fp16_to_fp32(fp32_to_fp16(1e30f)))); // overflow -> inf
}

TEST(TensorViewTest, BasicProperties) {
    alignas(8) std::array<float, 6> storage{};
    const TensorView tv(storage.data(), DType::kF32, Shape({2, 3}));
    EXPECT_TRUE(tv.valid());
    EXPECT_TRUE(tv.is_contiguous());
    EXPECT_EQ(tv.byte_size(), 24u);
    EXPECT_EQ(tv.span_as<float>().size(), 6u);
    const auto strides = tv.strides();
    ASSERT_EQ(strides.size(), 2u);
    EXPECT_EQ(strides[0], 3);
    EXPECT_EQ(strides[1], 1);
}

TEST(TensorViewTest, Reshape) {
    std::array<float, 6> storage{0, 1, 2, 3, 4, 5};
    const TensorView tv(storage.data(), DType::kF32, Shape({2, 3}));

    auto ok = tv.reshape(Shape({3, 2}));
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value().shape(), Shape({3, 2}));
    EXPECT_EQ(ok.value().data(), tv.data());

    auto bad = tv.reshape(Shape({4, 2}));
    EXPECT_FALSE(bad.ok());
}

TEST(TensorViewTest, Slice) {
    std::array<float, 6> storage{0, 1, 2, 3, 4, 5};
    const TensorView tv(storage.data(), DType::kF32, Shape({2, 3}));

    auto row1 = tv.slice(0, 1, 2);
    ASSERT_TRUE(row1.ok());
    EXPECT_EQ(row1.value().shape(), Shape({1, 3}));
    EXPECT_FLOAT_EQ(row1.value().span_as<const float>()[0], 3.0f);

    auto col = tv.slice(1, 1, 3);
    ASSERT_TRUE(col.ok());
    EXPECT_EQ(col.value().shape(), Shape({2, 2}));

    EXPECT_FALSE(tv.slice(2, 0, 1).ok());  // dim overflow
    EXPECT_FALSE(tv.slice(0, 1, 5).ok());  // end overflow
    EXPECT_FALSE(tv.slice(0, -1, 1).ok()); // negative begin
}

TEST(OwnedBufferTest, AllocateAndMove) {
    auto buf = OwnedBuffer::AllocateCpu(128, 64);
    ASSERT_TRUE(buf.ok());
    EXPECT_EQ(buf.value().size(), 128u);
    EXPECT_NE(buf.value().data(), nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.value().data()) % 64u, 0u);

    OwnedBuffer moved = std::move(buf).value();
    EXPECT_EQ(moved.size(), 128u);

    std::memset(moved.data(), 0xAB, moved.size()); // writable, ASan watches bounds
    EXPECT_EQ(static_cast<const uint8_t*>(moved.data())[127], 0xAB);
}

TEST(OwnedBufferTest, InvalidArgs) {
    EXPECT_FALSE(OwnedBuffer::AllocateCpu(0, 64).ok());
    EXPECT_FALSE(OwnedBuffer::AllocateCpu(64, 3).ok()); // non power-of-two
}

TEST(ScratchArenaTest, AllocateAndReset) {
    auto arena = ScratchArena::Create(4096);
    ASSERT_TRUE(arena.ok());
    auto& a = arena.value();

    auto t1 = a.AllocateTensor(Shape({4, 8}), DType::kF32);
    ASSERT_TRUE(t1.ok());
    EXPECT_GT(a.used(), 0u);
    const uintptr_t p1 = reinterpret_cast<uintptr_t>(t1.value().data());
    EXPECT_EQ(p1 % ScratchArena::kAlignment, 0u);

    auto t2 = a.AllocateTensor(Shape({16}), DType::kF16);
    ASSERT_TRUE(t2.ok());
    EXPECT_NE(t2.value().data(), t1.value().data());

    a.Reset();
    EXPECT_EQ(a.used(), 0u);
    auto t3 = a.AllocateTensor(Shape({4, 8}), DType::kF32);
    ASSERT_TRUE(t3.ok());
    EXPECT_EQ(t3.value().data(), t1.value().data()); // storage reused
}

TEST(ScratchArenaTest, Exhaustion) {
    auto arena = ScratchArena::Create(128);
    ASSERT_TRUE(arena.ok());
    auto& a = arena.value();

    auto t1 = a.AllocateTensor(Shape({16}), DType::kF32); // 64B
    ASSERT_TRUE(t1.ok());
    auto t2 = a.AllocateTensor(Shape({16}), DType::kF32); // 64B
    ASSERT_TRUE(t2.ok());
    auto t3 = a.AllocateTensor(Shape({16}), DType::kF32); // out of space
    EXPECT_FALSE(t3.ok());
    EXPECT_EQ(t3.status().code, ErrorCode::kOutOfMemory);
}

TEST(ScratchArenaTest, BadShape) {
    auto arena = ScratchArena::Create(128);
    ASSERT_TRUE(arena.ok());
    EXPECT_FALSE(arena.value().AllocateTensor(Shape({0}), DType::kF32).ok());
    EXPECT_FALSE(arena.value().AllocateTensor(Shape({33}), DType::kQ8_0).ok());
}

} // namespace
} // namespace pl::mllm
