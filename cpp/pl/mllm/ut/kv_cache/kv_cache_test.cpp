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

namespace {

// Write `token`'s K/V (f32) into `key`/`value` views of shape
// [1, num_kv_heads, head_dim]; K row = token*10 + channel index,
// V row = token*100 + channel index (values stay unique per token).
void fill_token_kv(int32_t token, TensorView key, TensorView value) {
    const int64_t n = key.shape().numel();
    auto* kd = key.data_as<float>();
    auto* vd = value.data_as<float>();
    for (int64_t i = 0; i < n; ++i) {
        kd[i] = static_cast<float>(token * 10) + static_cast<float>(i);
        vd[i] = static_cast<float>(token * 100) + static_cast<float>(i);
    }
}

// Sequential reference of the values `fill_token_kv` writes for token `t`.
float token_k_value(int32_t t, int64_t channel) {
    return static_cast<float>(t * 10) + static_cast<float>(channel);
}

// Row `i` of a contiguous [n, d1, d2] f32 tensor as a [1, d1, d2] view.
TensorView row_view(TensorView t, int32_t i) {
    const int64_t row_elems = t.shape().numel() / t.shape().dim(0);
    return TensorView(static_cast<char*>(t.data()) +
                          static_cast<size_t>(i) * static_cast<size_t>(row_elems) * sizeof(float),
                      t.dtype(),
                      Shape({1, t.shape().dim(1), t.shape().dim(2)}));
}

} // namespace

// Ring mode: appends beyond the window never fail; the oldest tokens are
// dropped in ceil(capacity/2) chunks and the window origin advances, so physical
// slot i always holds absolute position origin + i.

TEST(KvCacheTest, RingAppendWrapsWithoutError) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 4, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();
    EXPECT_TRUE(cache.ring());
    EXPECT_EQ(cache.capacity(), 4);

    std::vector<float> kb(2 * 8);
    std::vector<float> vb(2 * 8);
    TensorView kv(kb.data(), DType::kF32, {1, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {1, 2, 8});

    // Append far more tokens than the window holds.
    for (int32_t t = 0; t < 20; ++t) {
        fill_token_kv(t, kv, vv);
        ASSERT_TRUE(cache.Append(0, kv, vv).ok()) << "token " << t;
        cache.Advance();
    }
    EXPECT_EQ(cache.length(), 4);
    EXPECT_EQ(cache.window_origin(), 16); // 20 appended, 4 retained

    // Physical slots hold the newest 4 tokens in order.
    auto view = cache.View(0);
    ASSERT_EQ(view.seq_len, 4);
    const auto* kp = static_cast<const float*>(view.keys);
    for (int32_t slot = 0; slot < 4; ++slot) {
        EXPECT_FLOAT_EQ(kp[static_cast<size_t>(slot) * 16], token_k_value(16 + slot, 0))
            << "slot " << slot;
    }
}

TEST(KvCacheTest, RingCompactionShiftsEveryLayer) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 6, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> kb(16);
    std::vector<float> vb(16);
    TensorView kv(kb.data(), DType::kF32, {1, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {1, 2, 8});

    // Two layers, two distinguishable token sets.
    for (int32_t t = 0; t < 6; ++t) {
        fill_token_kv(t, kv, vv); // layer 0 tokens
        ASSERT_TRUE(cache.Append(0, kv, vv).ok());
        fill_token_kv(t + 100, kv, vv); // layer 1 tokens (offset 100)
        ASSERT_TRUE(cache.Append(1, kv, vv).ok());
        cache.Advance();
    }
    // 6 tokens == capacity 6: no overflow yet, no compaction.
    EXPECT_EQ(cache.length(), 6);
    EXPECT_EQ(cache.window_origin(), 0);

    // The 7th append would overflow capacity 6 -> drop ring_shift() = 3.
    fill_token_kv(6, kv, vv);
    ASSERT_TRUE(cache.Append(0, kv, vv).ok());
    fill_token_kv(106, kv, vv);
    ASSERT_TRUE(cache.Append(1, kv, vv).ok());
    cache.Advance();
    EXPECT_EQ(cache.length(), 4);
    EXPECT_EQ(cache.window_origin(), 3);

    // Layer 0 holds tokens 3..6; layer 1 holds 103..106.
    auto view0 = cache.View(0);
    const auto* k0 = static_cast<const float*>(view0.keys);
    EXPECT_FLOAT_EQ(k0[0], token_k_value(3, 0));
    EXPECT_FLOAT_EQ(k0[16], token_k_value(4, 0));
    EXPECT_FLOAT_EQ(k0[32], token_k_value(5, 0));
    EXPECT_FLOAT_EQ(k0[48], token_k_value(6, 0));
    auto view1 = cache.View(1);
    const auto* k1 = static_cast<const float*>(view1.keys);
    EXPECT_FLOAT_EQ(k1[0], token_k_value(103, 0));
    EXPECT_FLOAT_EQ(k1[16], token_k_value(104, 0));
    EXPECT_FLOAT_EQ(k1[32], token_k_value(105, 0));
    EXPECT_FLOAT_EQ(k1[48], token_k_value(106, 0));
}

TEST(KvCacheTest, RingAppendBatchCrossesCapacity) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 8, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    const int64_t nkv_hd = 2 * 8;
    std::vector<float> kb(6 * nkv_hd);
    std::vector<float> vb(6 * nkv_hd);
    TensorView kv(kb.data(), DType::kF32, {6, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {6, 2, 8});

    // Batch 1: tokens 0..5.
    for (int32_t t = 0; t < 6; ++t) {
        fill_token_kv(t, row_view(kv, t), row_view(vv, t));
    }
    ASSERT_TRUE(cache.AppendBatch(0, kv, vv).ok());
    cache.Advance(6);
    EXPECT_EQ(cache.length(), 6);

    // Batch 2: tokens 6..9 (6 + 4 > 8 -> drop 4 of the 6 retained).
    std::vector<float> kb2(4 * nkv_hd);
    std::vector<float> vb2(4 * nkv_hd);
    TensorView kv2(kb2.data(), DType::kF32, {4, 2, 8});
    TensorView vv2(vb2.data(), DType::kF32, {4, 2, 8});
    for (int32_t t = 6; t < 10; ++t) {
        fill_token_kv(t, row_view(kv2, t - 6), row_view(vv2, t - 6));
    }
    ASSERT_TRUE(cache.AppendBatch(0, kv2, vv2).ok());
    cache.Advance(4);
    EXPECT_EQ(cache.length(), 6);
    EXPECT_EQ(cache.window_origin(), 4);

    // Retained: old tokens 4,5 (slots 0,1) then new tokens 6..9 (slots 2..5).
    auto view = cache.View(0);
    ASSERT_EQ(view.seq_len, 6);
    const auto* kp = static_cast<const float*>(view.keys);
    const int32_t expect[] = {4, 5, 6, 7, 8, 9};
    for (int32_t s = 0; s < 6; ++s) {
        EXPECT_FLOAT_EQ(kp[static_cast<size_t>(s) * nkv_hd], token_k_value(expect[s], 0))
            << "slot " << s;
    }
}

TEST(KvCacheTest, RingAppendBatchLargerThanCapacity) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 8, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    const int64_t nkv_hd = 2 * 8;
    std::vector<float> kb(12 * nkv_hd);
    std::vector<float> vb(12 * nkv_hd);
    TensorView kv(kb.data(), DType::kF32, {12, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {12, 2, 8});
    for (int32_t t = 0; t < 12; ++t) {
        fill_token_kv(t, row_view(kv, t), row_view(vv, t));
    }
    // A single batch larger than the window keeps only its newest rows.
    ASSERT_TRUE(cache.AppendBatch(0, kv, vv).ok());
    cache.Advance(12);
    EXPECT_EQ(cache.length(), 8);
    EXPECT_EQ(cache.window_origin(), 4);

    auto view = cache.View(0);
    const auto* kp = static_cast<const float*>(view.keys);
    for (int32_t s = 0; s < 8; ++s) {
        EXPECT_FLOAT_EQ(kp[static_cast<size_t>(s) * nkv_hd], token_k_value(4 + s, 0))
            << "slot " << s;
    }
}

TEST(KvCacheTest, RingClearResetsOriginAndLength) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 4, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> kb(16);
    std::vector<float> vb(16);
    TensorView kv(kb.data(), DType::kF32, {1, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {1, 2, 8});
    for (int32_t t = 0; t < 6; ++t) {
        fill_token_kv(t, kv, vv);
        ASSERT_TRUE(cache.Append(0, kv, vv).ok());
        cache.Advance();
    }
    EXPECT_GT(cache.window_origin(), 0);

    cache.Clear();
    EXPECT_EQ(cache.length(), 0);
    EXPECT_EQ(cache.window_origin(), 0);
    // The cache is fully usable again from an empty window.
    for (int32_t t = 0; t < 6; ++t) {
        fill_token_kv(t, kv, vv);
        ASSERT_TRUE(cache.Append(0, kv, vv).ok());
        cache.Advance();
    }
    EXPECT_EQ(cache.length(), 4);
    EXPECT_EQ(cache.window_origin(), 2);
}

TEST(KvCacheTest, RingWindowShiftManual) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 8, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> kb(16);
    std::vector<float> vb(16);
    TensorView kv(kb.data(), DType::kF32, {1, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {1, 2, 8});
    for (int32_t t = 0; t < 6; ++t) {
        fill_token_kv(t, kv, vv);
        ASSERT_TRUE(cache.Append(0, kv, vv).ok());
        cache.Advance();
    }
    EXPECT_EQ(cache.length(), 6);
    EXPECT_EQ(cache.window_origin(), 0);

    // Out-of-range shifts are rejected.
    EXPECT_FALSE(cache.WindowShift(7).ok());
    EXPECT_FALSE(cache.WindowShift(-1).ok());

    // Manual shift of 2 mirrors what the Engine does for device KV.
    ASSERT_TRUE(cache.WindowShift(2).ok());
    EXPECT_EQ(cache.length(), 4);
    EXPECT_EQ(cache.window_origin(), 2);
    auto view = cache.View(0);
    const auto* kp = static_cast<const float*>(view.keys);
    EXPECT_FLOAT_EQ(kp[0], token_k_value(2, 0));
    EXPECT_FLOAT_EQ(kp[16], token_k_value(3, 0));
    EXPECT_FLOAT_EQ(kp[48], token_k_value(5, 0));
}

// Odd capacity: compaction drops ceil(capacity/2) tokens so the retained
// span (floor(capacity/2)) is never longer than the dropped one — device
// backends may then shift with non-overlapping copies.
TEST(KvCacheTest, RingCompactionRoundsUpForOddCapacity) {
    auto config = make_test_config();
    auto cache_result = KVCache::Create(config, 7, DType::kF32, KVCacheMode::kRing);
    ASSERT_TRUE(cache_result.ok());
    auto cache = std::move(cache_result).value();

    std::vector<float> kb(16);
    std::vector<float> vb(16);
    TensorView kv(kb.data(), DType::kF32, {1, 2, 8});
    TensorView vv(vb.data(), DType::kF32, {1, 2, 8});
    for (int32_t t = 0; t < 7; ++t) {
        fill_token_kv(t, kv, vv);
        ASSERT_TRUE(cache.Append(0, kv, vv).ok());
        cache.Advance();
    }
    EXPECT_EQ(cache.length(), 7);
    EXPECT_EQ(cache.window_origin(), 0);

    // The 8th append overflows capacity 7 -> drop ceil(7/2) = 4 tokens.
    fill_token_kv(7, kv, vv);
    ASSERT_TRUE(cache.Append(0, kv, vv).ok());
    cache.Advance();
    EXPECT_EQ(cache.length(), 4);
    EXPECT_EQ(cache.window_origin(), 4);

    // Physical slots hold the retained tail 4,5,6 followed by token 7.
    auto view = cache.View(0);
    ASSERT_EQ(view.seq_len, 4);
    const auto* kp = static_cast<const float*>(view.keys);
    for (int32_t s = 0; s < 4; ++s) {
        EXPECT_FLOAT_EQ(kp[static_cast<size_t>(s) * 16], token_k_value(4 + s, 0)) << "slot " << s;
    }
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
