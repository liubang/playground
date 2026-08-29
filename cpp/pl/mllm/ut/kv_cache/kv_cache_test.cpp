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

#include <gtest/gtest.h>

#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/model/config.h"

using namespace pl::mllm;

namespace {

ModelConfig make_test_config() {
    return ModelConfig{
        .architecture = "llama",
        .vocab_size = 100,
        .hidden_size = 32,
        .intermediate_size = 64,
        .num_layers = 2,
        .num_attention_heads = 4,
        .num_kv_heads = 2,
        .head_dim = 8,
        .context_length = 512,
        .rms_norm_eps = 1e-5f,
        .rope_freq_base = 10000.0f,
    };
}

} // namespace

TEST(KvCacheTest, CreateAndBasicProps) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 64);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();
    EXPECT_EQ(cache.num_layers(), 2);
    EXPECT_EQ(cache.num_kv_heads(), 2);
    EXPECT_EQ(cache.head_dim(), 8);
    EXPECT_EQ(cache.capacity(), 64);
    EXPECT_EQ(cache.length(), 0);
}

TEST(KvCacheTest, AppendAndView) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 16, DType::kF32);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> k(2 * 8, 1.0f);
    std::vector<float> v(2 * 8, 2.0f);
    TensorView kv(k.data(), DType::kF32, {1, 2, 8});
    TensorView vv(v.data(), DType::kF32, {1, 2, 8});

    ASSERT_TRUE(cache.Append(0, kv, vv).ok());
    cache.Advance();
    EXPECT_EQ(cache.length(), 1);

    auto view = cache.View(0);
    EXPECT_EQ(view.seq_len, 1);
    EXPECT_EQ(view.num_kv_heads, 2);
    EXPECT_EQ(view.head_dim, 8);

    const auto* kp = static_cast<const float*>(view.keys);
    EXPECT_FLOAT_EQ(kp[0], 1.0f);
    EXPECT_FLOAT_EQ(kp[15], 1.0f);
    const auto* vp = static_cast<const float*>(view.values);
    EXPECT_FLOAT_EQ(vp[0], 2.0f);
}

TEST(KvCacheTest, CapacityOverflow) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 2, DType::kF32);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> k(16, 0.0f);
    std::vector<float> v(16, 0.0f);
    TensorView kv(k.data(), DType::kF32, {1, 2, 8});
    TensorView vv(v.data(), DType::kF32, {1, 2, 8});

    ASSERT_TRUE(cache.Append(0, kv, vv).ok());
    cache.Advance();
    ASSERT_TRUE(cache.Append(0, kv, vv).ok());
    cache.Advance();
    EXPECT_EQ(cache.length(), 2);

    auto s = cache.Append(0, kv, vv);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

TEST(KvCacheTest, LayerOutOfRange) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 8);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> k(16, 0.0f);
    std::vector<float> v(16, 0.0f);
    TensorView kv(k.data(), DType::kF16, {1, 2, 8});
    TensorView vv(v.data(), DType::kF16, {1, 2, 8});

    auto s = cache.Append(5, kv, vv);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

TEST(KvCacheTest, Clear) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 8, DType::kF32);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> k(16, 1.0f);
    std::vector<float> v(16, 2.0f);
    TensorView kv_v(k.data(), DType::kF32, {1, 2, 8});
    TensorView vv_v(v.data(), DType::kF32, {1, 2, 8});

    cache.Append(0, kv_v, vv_v);
    cache.Advance();
    EXPECT_EQ(cache.length(), 1);

    cache.Clear();
    EXPECT_EQ(cache.length(), 0);
}

TEST(KvCacheTest, MultiLayerIndependentViews) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 16, DType::kF32);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> k0(16, 10.0f);
    std::vector<float> v0(16, 10.0f);
    std::vector<float> k1(16, 20.0f);
    std::vector<float> v1(16, 20.0f);
    TensorView k0v(k0.data(), DType::kF32, {1, 2, 8});
    TensorView v0v(v0.data(), DType::kF32, {1, 2, 8});
    TensorView k1v(k1.data(), DType::kF32, {1, 2, 8});
    TensorView v1v(v1.data(), DType::kF32, {1, 2, 8});

    cache.Append(0, k0v, v0v);
    cache.Append(1, k1v, v1v);
    cache.Advance();
    EXPECT_EQ(cache.length(), 1);

    auto view0 = cache.View(0);
    auto view1 = cache.View(1);
    const auto* k0p = static_cast<const float*>(view0.keys);
    const auto* k1p = static_cast<const float*>(view1.keys);
    EXPECT_FLOAT_EQ(k0p[0], 10.0f);
    EXPECT_FLOAT_EQ(k1p[0], 20.0f);
}
