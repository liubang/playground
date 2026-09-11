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
// Created: 2026/09/11

// Tiny-fixture vision tower test: builds a minimal PaddleOCR-VL mmproj GGUF
// in memory (1 ViT block, hidden 8, patch 2, merge 2), creates the tower
// through the factory, and runs Encode on the CPU reference backend.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#include "cpp/pl/mllm/loader/gguf.h"
#include "cpp/pl/mllm/media/image.h"
#include "cpp/pl/mllm/ut/testdata/gguf_writer.h"
#include "cpp/pl/mllm/vision/vision_tower.h"
#include "gtest/gtest.h"

namespace pl::mllm::vision {
namespace {

namespace td = pl::mllm::testdata;

class TempFile {
public:
    explicit TempFile(std::vector<uint8_t> bytes)
        : path_(std::filesystem::temp_directory_path() / "mllm_vision_test_XXXXXX") {
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() {
        if (!path_.empty()) {
            std::filesystem::remove(path_);
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempFile& operator=(TempFile&&) = delete;
    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Tiny tower geometry:
//   hidden 8, inter 16, 1 layer, 2 heads (head_dim 4), patch 2, merge 2
//   -> resize factor 4; an 8x8 image gives a 4x4 patch grid, 16 patches,
//      2x2 = 4 merged tokens; projector output (LM hidden) = 8.
constexpr int32_t kHidden = 8;
constexpr int32_t kInter = 16;
constexpr int32_t kHeads = 2;
constexpr int32_t kPatch = 2;
constexpr int32_t kMerge = 2;
constexpr int32_t kOutDim = 8;
constexpr int32_t kRefGrid = 4;                           // position table is kRefGrid^2 x kHidden
constexpr int32_t kPatchDim = 3 * kPatch * kPatch;        // 12
constexpr int32_t kMergedDim = kMerge * kMerge * kHidden; // 32

std::vector<uint8_t> f32_bytes(const std::vector<float>& values) {
    std::vector<uint8_t> out;
    out.reserve(values.size() * 4);
    for (float v : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        out.push_back(static_cast<uint8_t>(bits & 0xFF));
        out.push_back(static_cast<uint8_t>(bits >> 8));
        out.push_back(static_cast<uint8_t>(bits >> 16));
        out.push_back(static_cast<uint8_t>(bits >> 24));
    }
    return out;
}

// Deterministic small weights (same LCG as the engine fixture).
float lcg_next(uint32_t& state) {
    state = state * 1103515245u + 12345u;
    return static_cast<float>(state) / static_cast<float>(0xFFFFFFFFu) * 2.0f - 1.0f;
}

std::vector<float> make_weights(uint32_t seed, size_t count) {
    uint32_t state = seed;
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = lcg_next(state) * 0.1f;
    }
    return out;
}

std::vector<float> ones(size_t count) {
    return std::vector<float>(count, 1.0f);
}
std::vector<float> zeros(size_t count) {
    return std::vector<float>(count, 0.0f);
}

struct TinyMmprojOptions {
    bool pre_norm_naming = false;   // mm.pre_norm.* instead of mm.input_norm.*
    bool zero_projector = false;    // fc1/fc2 weights and biases all zero
    bool drop_post_ln_bias = false; // exercise the missing-weight failure
    int32_t projection_dim = kOutDim;
    std::string projector_type = "paddleocr";
};

td::GgufWriter make_tiny_mmproj_writer(const TinyMmprojOptions& opts = {}) {
    td::GgufWriter w("clip");
    w.meta_string("clip.projector_type", opts.projector_type);
    w.meta_u32("clip.vision.embedding_length", kHidden);
    w.meta_u32("clip.vision.feed_forward_length", kInter);
    w.meta_u32("clip.vision.block_count", 1);
    w.meta_u32("clip.vision.attention.head_count", kHeads);
    w.meta_u32("clip.vision.patch_size", kPatch);
    w.meta_u32("clip.vision.spatial_merge_size", kMerge);
    w.meta_u32("clip.vision.projection_dim", static_cast<uint32_t>(opts.projection_dim));
    w.meta_f32("clip.vision.attention.layer_norm_epsilon", 1e-6f);

    uint32_t seed = 7;
    auto add = [&](std::string name,
                   std::initializer_list<uint64_t> ggml_dims,
                   const std::vector<float>& values) {
        w.tensor({std::move(name),
                  std::vector<uint64_t>(ggml_dims),
                  td::GgufType::kF32,
                  f32_bytes(values)});
    };
    auto weights = [&](std::string name, std::initializer_list<uint64_t> ggml_dims, size_t count) {
        add(std::move(name), ggml_dims, make_weights(seed++, count));
    };

    // GGML dims are column-major (reversed on load):
    // loaded [out_dim, in_dim] <- ggml {in_dim, out_dim}.
    weights("v.patch_embd.weight", {kPatchDim, kHidden}, kHidden * kPatchDim);
    weights("v.patch_embd.bias", {kHidden}, kHidden);
    weights(
        "v.position_embd.weight", {kHidden, kRefGrid * kRefGrid}, kRefGrid * kRefGrid * kHidden);
    weights("v.post_ln.weight", {kHidden}, kHidden);
    if (!opts.drop_post_ln_bias) {
        add("v.post_ln.bias", {kHidden}, zeros(kHidden));
    }

    const std::string p = "v.blk.0.";
    weights(p + "attn_q.weight", {kHidden, kHidden}, kHidden * kHidden);
    weights(p + "attn_k.weight", {kHidden, kHidden}, kHidden * kHidden);
    weights(p + "attn_v.weight", {kHidden, kHidden}, kHidden * kHidden);
    weights(p + "attn_out.weight", {kHidden, kHidden}, kHidden * kHidden);
    weights(p + "ffn_up.weight", {kHidden, kInter}, kInter * kHidden);
    weights(p + "ffn_down.weight", {kInter, kHidden}, kHidden * kInter);
    for (const char* b : {"attn_q.bias", "attn_k.bias", "attn_v.bias", "attn_out.bias"}) {
        add(p + b, {kHidden}, zeros(kHidden));
    }
    add(p + "ffn_up.bias", {kInter}, zeros(kInter));
    add(p + "ffn_down.bias", {kHidden}, zeros(kHidden));
    // LayerNorm affine: weight 1, bias 0 keeps signals in range.
    add(p + "ln1.weight", {kHidden}, ones(kHidden));
    add(p + "ln1.bias", {kHidden}, zeros(kHidden));
    add(p + "ln2.weight", {kHidden}, ones(kHidden));
    add(p + "ln2.bias", {kHidden}, zeros(kHidden));

    const std::string norm = opts.pre_norm_naming ? "mm.pre_norm." : "mm.input_norm.";
    add(norm + "weight", {kHidden}, ones(kHidden));
    add(norm + "bias", {kHidden}, zeros(kHidden));

    if (opts.zero_projector) {
        add("mm.0.weight", {kMergedDim, kMergedDim}, zeros(kMergedDim * kMergedDim));
        add("mm.0.bias", {kMergedDim}, zeros(kMergedDim));
        add("mm.2.weight", {kMergedDim, kOutDim}, zeros(kOutDim * kMergedDim));
        add("mm.2.bias", {kOutDim}, zeros(kOutDim));
    } else {
        weights("mm.0.weight", {kMergedDim, kMergedDim}, kMergedDim * kMergedDim);
        add("mm.0.bias", {kMergedDim}, zeros(kMergedDim));
        weights("mm.2.weight", {kMergedDim, kOutDim}, kOutDim * kMergedDim);
        add("mm.2.bias", {kOutDim}, zeros(kOutDim));
    }
    return w;
}

// Deterministic ramp image (values in [0, 1]).
media::Image ramp_image(int32_t w, int32_t h) {
    media::Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (size_t i = 0; i < img.pixels.size(); ++i) {
        img.pixels[i] = static_cast<float>((i * 37) % 251) / 251.0f;
    }
    return img;
}

struct TinyTower {
    TempFile file;
    std::shared_ptr<GGUFFile> gguf;
    std::vector<WeightEntry> entries;
    std::unique_ptr<VisionTower> tower;
};

std::unique_ptr<TinyTower> make_tower(const TinyMmprojOptions& opts = {}) {
    auto t = std::make_unique<TinyTower>(
        TinyTower{TempFile(make_tiny_mmproj_writer(opts).build(32)), nullptr, {}, nullptr});
    auto f = GGUFFile::Open(t->file.path());
    EXPECT_TRUE(f.ok()) << f.status().message;
    t->gguf = std::move(f).value();
    for (const auto& ti : t->gguf->tensors()) {
        auto v = t->gguf->tensor(ti.name);
        EXPECT_TRUE(v.ok()) << ti.name;
        t->entries.push_back({ti.name, v.value()});
    }
    auto tower = CreateVisionTower(*t->gguf, t->entries);
    EXPECT_TRUE(tower.ok()) << tower.status().message;
    t->tower = std::move(tower).value();
    return t;
}

// Import every mmproj tensor into the CPU backend (matmul weights are
// resolved by name inside the tower).
void import_all(const TinyTower& t, CpuBackend& backend) {
    std::vector<TensorView> views;
    std::vector<std::string_view> names;
    views.reserve(t.entries.size());
    names.reserve(t.entries.size());
    for (const auto& e : t.entries) {
        views.push_back(e.view);
        names.push_back(e.name);
    }
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());
}

// ---------------------------------------------------------------------------

TEST(VisionTowerTest, CreateParsesConfig) {
    auto t = make_tower();
    const VisionConfig& cfg = t->tower->config();
    EXPECT_EQ(cfg.projector_type, "paddleocr");
    EXPECT_EQ(cfg.hidden_size, kHidden);
    EXPECT_EQ(cfg.intermediate_size, kInter);
    EXPECT_EQ(cfg.num_layers, 1);
    EXPECT_EQ(cfg.num_heads, kHeads);
    EXPECT_EQ(cfg.patch_size, kPatch);
    EXPECT_EQ(cfg.spatial_merge_size, kMerge);
    // output_dim is cross-checked against (and inferred from) fc2.
    EXPECT_EQ(cfg.output_dim, kOutDim);

    // 3 top-level matmuls (patch embd + fc1 + fc2) + 6 per layer.
    EXPECT_EQ(t->tower->weight_names().size(), 3u + 6u * 1u);
}

TEST(VisionTowerTest, TokenCountMatchesGeometry) {
    auto t = make_tower();
    // 8x8 -> 4x4 patches -> 2x2 merged tokens.
    auto n = t->tower->TokenCount(8, 8);
    ASSERT_TRUE(n.ok()) << n.status().message;
    EXPECT_EQ(n.value(), 4);
    // 16x8 -> 8x4 patches -> 4x2 = 8 tokens.
    auto n2 = t->tower->TokenCount(16, 8);
    ASSERT_TRUE(n2.ok()) << n2.status().message;
    EXPECT_EQ(n2.value(), 8);
    // Extreme aspect ratio is rejected by smart resize.
    EXPECT_FALSE(t->tower->TokenCount(1, 300).ok());
}

TEST(VisionTowerTest, EncodeShapeGridAndDeterminism) {
    auto t = make_tower();
    CpuBackend backend;
    import_all(*t, backend);

    const media::Image img = ramp_image(8, 8);
    auto out = t->tower->Encode(img, backend);
    ASSERT_TRUE(out.ok()) << out.status().message;
    const VisionOutput& o = out.value();
    EXPECT_EQ(o.n_tokens, 4);
    EXPECT_EQ(o.grid_h, 2);
    EXPECT_EQ(o.grid_w, 2);
    EXPECT_EQ(o.embeddings.shape(), Shape({4, kOutDim}));
    const float* p = o.embeddings.data_as<const float>();
    for (int64_t i = 0; i < 4 * kOutDim; ++i) {
        EXPECT_TRUE(std::isfinite(p[i])) << "idx " << i;
    }

    // Same image, same weights -> bitwise identical embeddings.
    auto out2 = t->tower->Encode(img, backend);
    ASSERT_TRUE(out2.ok()) << out2.status().message;
    const float* p2 = out2.value().embeddings.data_as<const float>();
    for (int64_t i = 0; i < 4 * kOutDim; ++i) {
        EXPECT_EQ(p[i], p2[i]) << "idx " << i;
    }

    // A different image must change the embeddings (the tower is not
    // collapsing its input).
    const media::Image other = ramp_image(8, 8);
    media::Image shifted;
    shifted.width = 8;
    shifted.height = 8;
    shifted.pixels = other.pixels;
    for (float& v : shifted.pixels) {
        v = 1.0f - v;
    }
    auto out3 = t->tower->Encode(shifted, backend);
    ASSERT_TRUE(out3.ok()) << out3.status().message;
    const float* p3 = out3.value().embeddings.data_as<const float>();
    float max_diff = 0.0f;
    for (int64_t i = 0; i < 4 * kOutDim; ++i) {
        max_diff = std::max(max_diff, std::fabs(p[i] - p3[i]));
    }
    EXPECT_GT(max_diff, 1e-6f);
}

TEST(VisionTowerTest, EncodeRejectsInvalidImage) {
    auto t = make_tower();
    CpuBackend backend;
    import_all(*t, backend);
    const media::Image empty;
    auto out = t->tower->Encode(empty, backend);
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code, ErrorCode::kInvalidArgument);
}

TEST(VisionTowerTest, PreNormNamingFallback) {
    TinyMmprojOptions opts;
    opts.pre_norm_naming = true;
    auto t = make_tower(opts);
    CpuBackend backend;
    import_all(*t, backend);
    auto out = t->tower->Encode(ramp_image(8, 8), backend);
    ASSERT_TRUE(out.ok()) << out.status().message;
    EXPECT_EQ(out.value().n_tokens, 4);
}

TEST(VisionTowerTest, ZeroProjectorCollapsesOutput) {
    // fc1/fc2 == 0 -> z = gelu(merged @ 0 + 0) = 0 -> out = 0 exactly,
    // independent of the input image. Anchors the projector wiring.
    TinyMmprojOptions opts;
    opts.zero_projector = true;
    auto t = make_tower(opts);
    CpuBackend backend;
    import_all(*t, backend);
    auto out = t->tower->Encode(ramp_image(8, 8), backend);
    ASSERT_TRUE(out.ok()) << out.status().message;
    const float* p = out.value().embeddings.data_as<const float>();
    for (int64_t i = 0; i < 4 * kOutDim; ++i) {
        EXPECT_FLOAT_EQ(p[i], 0.0f) << "idx " << i;
    }
}

TEST(VisionTowerTest, UnknownProjectorTypeRejected) {
    TinyMmprojOptions opts;
    opts.projector_type = "siglip";
    TempFile tmp(make_tiny_mmproj_writer(opts).build(32));
    auto f = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(f.ok()) << f.status().message;
    auto tower = CreateVisionTower(*f.value(), {});
    EXPECT_FALSE(tower.ok());
    EXPECT_EQ(tower.status().code, ErrorCode::kUnsupported);
}

TEST(VisionTowerTest, MissingWeightFailsFast) {
    TinyMmprojOptions opts;
    opts.drop_post_ln_bias = true;
    TempFile tmp(make_tiny_mmproj_writer(opts).build(32));
    auto f = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(f.ok()) << f.status().message;
    std::vector<WeightEntry> entries;
    for (const auto& ti : f.value()->tensors()) {
        auto v = f.value()->tensor(ti.name);
        ASSERT_TRUE(v.ok());
        entries.push_back({ti.name, v.value()});
    }
    auto tower = CreateVisionTower(*f.value(), entries);
    EXPECT_FALSE(tower.ok());
    EXPECT_EQ(tower.status().code, ErrorCode::kNotFound);
}

TEST(VisionTowerTest, ProjectionDimMismatchRejected) {
    TinyMmprojOptions opts;
    opts.projection_dim = 16; // metadata says 16, fc2 produces 8
    TempFile tmp(make_tiny_mmproj_writer(opts).build(32));
    auto f = GGUFFile::Open(tmp.path());
    ASSERT_TRUE(f.ok()) << f.status().message;
    std::vector<WeightEntry> entries;
    for (const auto& ti : f.value()->tensors()) {
        auto v = f.value()->tensor(ti.name);
        ASSERT_TRUE(v.ok());
        entries.push_back({ti.name, v.value()});
    }
    auto tower = CreateVisionTower(*f.value(), entries);
    EXPECT_FALSE(tower.ok());
    EXPECT_EQ(tower.status().code, ErrorCode::kInvalidFormat);
}

} // namespace
} // namespace pl::mllm::vision
