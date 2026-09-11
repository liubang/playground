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

#include "cpp/pl/mllm/vision/paddleocr_tower.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/media/image.h"

namespace pl::mllm::vision {

namespace {

// ---------------------------------------------------------------------------
// Weight lookup helpers
// ---------------------------------------------------------------------------

TensorView find_weight(std::span<const WeightEntry> weights, std::string_view name) {
    for (const auto& w : weights) {
        if (w.name == name) {
            return w.view;
        }
    }
    return {};
}

Status require_weight(std::span<const WeightEntry> weights, std::string_view name) {
    if (!find_weight(weights, name).valid()) {
        return Status::Error(ErrorCode::kNotFound, "missing vision weight: " + std::string(name));
    }
    return {};
}

// Elementwise-consumed weights (norms, biases) must be dense floats.
Status require_dense_weight(std::span<const WeightEntry> weights, std::string_view name) {
    const TensorView v = find_weight(weights, name);
    if (!v.valid()) {
        return Status::Error(ErrorCode::kNotFound, "missing vision weight: " + std::string(name));
    }
    if (v.dtype() != DType::kF32 && v.dtype() != DType::kF16 && v.dtype() != DType::kBF16) {
        return Status::Error(ErrorCode::kUnsupported,
                             "vision weight must be f32/f16/bf16: " + std::string(name));
    }
    return {};
}

// Scalar element access for f32/f16/bf16 views (position table interpolation).
float elem_f32(const TensorView& v, int64_t idx) {
    if (v.dtype() == DType::kF32) {
        return v.data_as<const float>()[static_cast<size_t>(idx)];
    }
    if (v.dtype() == DType::kBF16) {
        return bf16_to_fp32(v.data_as<const uint16_t>()[static_cast<size_t>(idx)]);
    }
    return fp16_to_fp32(v.data_as<const uint16_t>()[static_cast<size_t>(idx)]);
}

// Row-major f32 matrix view over a vector.
TensorView view2d(std::vector<float>& buf, int64_t rows, int64_t cols) {
    return TensorView(buf.data(), DType::kF32, Shape({rows, cols}));
}
TensorView view3d(std::vector<float>& buf, int64_t n, int64_t heads, int64_t dim) {
    return TensorView(buf.data(), DType::kF32, Shape({n, heads, dim}));
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct PatchGeometry {
    int32_t width;     // resized image width
    int32_t height;    // resized image height
    int32_t grid_w;    // patch columns
    int32_t grid_h;    // patch rows
    int32_t n_patches; // grid_w * grid_h
    int32_t n_merged;  // tokens after spatial merge
};

Result<PatchGeometry> patch_geometry(const VisionConfig& cfg, int32_t width, int32_t height) {
    const int32_t factor = cfg.patch_size * cfg.spatial_merge_size;
    const auto [w, h] = media::SmartResize(width, height, factor, cfg.min_pixels, cfg.max_pixels);
    if (w <= 0 || h <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "vision: image cannot be resized");
    }
    const int32_t gw = w / cfg.patch_size;
    const int32_t gh = h / cfg.patch_size;
    const int32_t m = cfg.spatial_merge_size;
    return PatchGeometry{
        .width = w,
        .height = h,
        .grid_w = gw,
        .grid_h = gh,
        .n_patches = gh * gw,
        .n_merged = (gh / m) * (gw / m),
    };
}

// Patchify: image [H, W, 3] -> [n_patches, 3 * p * p], channel-major within
// each patch (c, y, x) so the flattened conv kernel can act as a GEMM matrix.
std::vector<float> patchify(const media::Image& img, int32_t patch) {
    const size_t p = static_cast<size_t>(patch);
    const size_t gw = static_cast<size_t>(img.width) / p;
    const size_t gh = static_cast<size_t>(img.height) / p;
    const size_t dim = 3 * p * p;
    std::vector<float> out(gh * gw * dim);
    for (size_t gy = 0; gy < gh; ++gy) {
        for (size_t gx = 0; gx < gw; ++gx) {
            float* dst = out.data() + (gy * gw + gx) * dim;
            for (size_t c = 0; c < 3; ++c) {
                for (size_t py = 0; py < p; ++py) {
                    const float* src_row = img.row(static_cast<int32_t>(gy * p + py));
                    for (size_t px = 0; px < p; ++px) {
                        dst[(c * p + py) * p + px] = src_row[(gx * p + px) * 3 + c];
                    }
                }
            }
        }
    }
    return out;
}

// Bilinear interpolation of the learned position table from its square
// reference grid onto the (gh, gw) patch grid (align_corners = false).
std::vector<float> interpolate_pos_embd(const TensorView& table,
                                        int32_t hidden,
                                        int32_t gh,
                                        int32_t gw) {
    const int32_t ref =
        static_cast<int32_t>(std::lround(std::sqrt(static_cast<double>(table.shape().dim(0)))));
    std::vector<float> out(static_cast<size_t>(gh) * static_cast<size_t>(gw) *
                           static_cast<size_t>(hidden));
    const float sy = static_cast<float>(ref) / static_cast<float>(gh);
    const float sx = static_cast<float>(ref) / static_cast<float>(gw);
    for (int32_t y = 0; y < gh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(fy)), 0, ref - 1);
        const int32_t y1 = std::min(y0 + 1, ref - 1);
        const float wy = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (int32_t x = 0; x < gw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(fx)), 0, ref - 1);
            const int32_t x1 = std::min(x0 + 1, ref - 1);
            const float wx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            const size_t out_row =
                (static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)) *
                static_cast<size_t>(hidden);
            float* dst = out.data() + out_row;
            const int64_t e_row = static_cast<int64_t>(ref) * static_cast<int64_t>(hidden);
            const int64_t x0h = static_cast<int64_t>(x0) * hidden;
            const int64_t x1h = static_cast<int64_t>(x1) * hidden;
            const int64_t r00 = static_cast<int64_t>(y0) * e_row + x0h;
            const int64_t r01 = static_cast<int64_t>(y0) * e_row + x1h;
            const int64_t r10 = static_cast<int64_t>(y1) * e_row + x0h;
            const int64_t r11 = static_cast<int64_t>(y1) * e_row + x1h;
            for (int32_t d = 0; d < hidden; ++d) {
                const float p00 = elem_f32(table, r00 + d);
                const float p01 = elem_f32(table, r01 + d);
                const float p10 = elem_f32(table, r10 + d);
                const float p11 = elem_f32(table, r11 + d);
                dst[d] = (p00 * (1.0f - wx) + p01 * wx) * (1.0f - wy) +
                         (p10 * (1.0f - wx) + p11 * wx) * wy;
            }
        }
    }
    return out;
}

// 2D rotary tables (Qwen2-VL vision convention, neox pairing): pair i of
// head_dim uses the row coordinate for i < pairs/2 and the column
// coordinate otherwise. cos/sin rows are [cos(freqs), cos(freqs)] doubled
// for the rotate-half application.
void build_rope2d_tables(const VisionConfig& cfg,
                         int32_t gh,
                         int32_t gw,
                         std::vector<float>& cos_out,
                         std::vector<float>& sin_out) {
    const int32_t hd = cfg.head_dim();
    const int32_t pairs = hd / 2;   // neox pairs
    const int32_t sect = pairs / 2; // pairs per spatial axis
    const size_t n = static_cast<size_t>(gh) * static_cast<size_t>(gw);
    const size_t row = static_cast<size_t>(hd);
    cos_out.resize(n * row);
    sin_out.resize(n * row);
    for (int32_t gy = 0; gy < gh; ++gy) {
        for (int32_t gx = 0; gx < gw; ++gx) {
            const size_t base =
                (static_cast<size_t>(gy) * static_cast<size_t>(gw) + static_cast<size_t>(gx)) * row;
            for (int32_t i = 0; i < pairs; ++i) {
                // Frequency of pair i within its half: inv_freq = theta^(-2i/hd_half)
                const int32_t j = i % sect;
                const float inv = std::pow(
                    cfg.rope_freq_base, -2.0f * static_cast<float>(j) / static_cast<float>(pairs));
                const float angle =
                    (i < sect ? static_cast<float>(gy) : static_cast<float>(gx)) * inv;
                const size_t idx = base + static_cast<size_t>(i);
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                cos_out[idx] = c;
                cos_out[idx + static_cast<size_t>(pairs)] = c;
                sin_out[idx] = s;
                sin_out[idx + static_cast<size_t>(pairs)] = s;
            }
        }
    }
}

// Spatial merge (pixel unshuffle): [gh*gw, hidden] -> [n_merged, m^2*hidden]
// with the (block_row, block_col) inner order of the reference
// implementations (m1 index first, then m2).
std::vector<float> merge_patches(
    const std::vector<float>& x, int32_t gh, int32_t gw, int32_t hidden, int32_t m) {
    const size_t sm = static_cast<size_t>(m);
    const size_t sgw = static_cast<size_t>(gw);
    const size_t shidden = static_cast<size_t>(hidden);
    const size_t mb_h = static_cast<size_t>(gh) / sm;
    const size_t mb_w = sgw / sm;
    std::vector<float> out(mb_h * mb_w * sm * sm * shidden);
    for (size_t hb = 0; hb < mb_h; ++hb) {
        for (size_t wb = 0; wb < mb_w; ++wb) {
            float* dst = out.data() + (hb * mb_w + wb) * sm * sm * shidden;
            for (size_t i = 0; i < sm; ++i) {
                for (size_t j = 0; j < sm; ++j) {
                    const float* src = x.data() + ((hb * sm + i) * sgw + (wb * sm + j)) * shidden;
                    std::memcpy(dst + (i * sm + j) * shidden, src, shidden * sizeof(float));
                }
            }
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// PaddleOcrTower
// ---------------------------------------------------------------------------

Result<std::unique_ptr<PaddleOcrTower>> PaddleOcrTower::Create(
    VisionConfig config, std::span<const WeightEntry> weights) {
    if (auto s = config.Validate(); !s.ok()) {
        return s;
    }

    auto tower = std::unique_ptr<PaddleOcrTower>(new PaddleOcrTower());
    tower->config_ = config;

    // Top-level weights.
    if (auto s = require_weight(weights, "v.patch_embd.weight"); !s.ok()) {
        return s;
    }
    if (auto s = require_dense_weight(weights, "v.patch_embd.bias"); !s.ok()) {
        return s;
    }
    if (auto s = require_dense_weight(weights, "v.position_embd.weight"); !s.ok()) {
        return s;
    }
    if (auto s = require_dense_weight(weights, "v.post_ln.weight"); !s.ok()) {
        return s;
    }
    if (auto s = require_dense_weight(weights, "v.post_ln.bias"); !s.ok()) {
        return s;
    }

    // patch_embd + fc1 + fc2 + 6 matmul names per layer. Under-reserving
    // would reallocate mid-loop and dangle every stored string_view.
    tower->name_storage_.reserve(3 + 6 * static_cast<size_t>(config.num_layers));
    auto store_name = [&](std::string name) -> std::string_view {
        tower->name_storage_.push_back(std::move(name));
        return tower->name_storage_.back();
    };

    tower->patch_embd_w_ = store_name("v.patch_embd.weight");
    tower->patch_embd_b_ = find_weight(weights, "v.patch_embd.bias");
    tower->pos_embd_ = find_weight(weights, "v.position_embd.weight");
    tower->post_ln_w_ = find_weight(weights, "v.post_ln.weight");
    tower->post_ln_b_ = find_weight(weights, "v.post_ln.bias");

    // Per-layer weights (SigLIP conventions: LayerNorm + biased projections).
    for (int32_t l = 0; l < config.num_layers; ++l) {
        const std::string p = "v.blk." + std::to_string(l) + ".";
        LayerWeights lw;
        lw.q_w = store_name(p + "attn_q.weight");
        lw.k_w = store_name(p + "attn_k.weight");
        lw.v_w = store_name(p + "attn_v.weight");
        lw.o_w = store_name(p + "attn_out.weight");
        lw.up_w = store_name(p + "ffn_up.weight");
        lw.down_w = store_name(p + "ffn_down.weight");

        for (const char* name : {"attn_q.weight",
                                 "attn_k.weight",
                                 "attn_v.weight",
                                 "attn_out.weight",
                                 "ffn_up.weight",
                                 "ffn_down.weight"}) {
            if (auto s = require_weight(weights, p + name); !s.ok()) {
                return s;
            }
        }
        for (const char* name : {"attn_q.bias",
                                 "attn_k.bias",
                                 "attn_v.bias",
                                 "attn_out.bias",
                                 "ffn_up.bias",
                                 "ffn_down.bias",
                                 "ln1.weight",
                                 "ln1.bias",
                                 "ln2.weight",
                                 "ln2.bias"}) {
            if (auto s = require_dense_weight(weights, p + name); !s.ok()) {
                return s;
            }
        }
        lw.q_b = find_weight(weights, p + "attn_q.bias");
        lw.k_b = find_weight(weights, p + "attn_k.bias");
        lw.v_b = find_weight(weights, p + "attn_v.bias");
        lw.o_b = find_weight(weights, p + "attn_out.bias");
        lw.up_b = find_weight(weights, p + "ffn_up.bias");
        lw.down_b = find_weight(weights, p + "ffn_down.bias");
        lw.ln1_w = find_weight(weights, p + "ln1.weight");
        lw.ln1_b = find_weight(weights, p + "ln1.bias");
        lw.ln2_w = find_weight(weights, p + "ln2.weight");
        lw.ln2_b = find_weight(weights, p + "ln2.bias");
        tower->layers_.push_back(lw);
    }

    // Projector ("mlp_AR"): input norm + two linears. The GGUF numbering of
    // the linear pair follows the converter; probe the known variants and
    // validate by shape (fc1 in = merge^2 * hidden; fc2 out = output_dim).
    const int64_t merged_dim = static_cast<int64_t>(config.spatial_merge_size) *
                               config.spatial_merge_size * config.hidden_size;

    TensorView proj_norm_w = find_weight(weights, "mm.input_norm.weight");
    TensorView proj_norm_b = find_weight(weights, "mm.input_norm.bias");
    if (!proj_norm_w.valid()) {
        proj_norm_w = find_weight(weights, "mm.pre_norm.weight");
        proj_norm_b = find_weight(weights, "mm.pre_norm.bias");
    }
    if (!proj_norm_w.valid()) {
        return Status::Error(ErrorCode::kNotFound,
                             "missing vision weight: mm.input_norm.weight (or mm.pre_norm.weight)");
    }
    tower->proj_norm_w_ = proj_norm_w;
    tower->proj_norm_b_ = proj_norm_b;

    TensorView fc1, fc2;
    std::string_view fc1_name, fc2_name;
    for (const char* cand : {"mm.0.weight", "mm.1.weight"}) {
        const TensorView v = find_weight(weights, cand);
        if (v.valid() && v.shape().rank() == 2 && v.shape().dim(1) == merged_dim) {
            fc1 = v;
            fc1_name = cand;
            break;
        }
    }
    for (const char* cand : {"mm.2.weight", "mm.1.weight", "mm.3.weight"}) {
        const TensorView v = find_weight(weights, cand);
        if (v.valid() && v.shape().rank() == 2 && v.shape().dim(1) == fc1.shape().dim(0)) {
            fc2 = v;
            fc2_name = cand;
            break;
        }
    }
    if (!fc1.valid() || !fc2.valid()) {
        return Status::Error(ErrorCode::kNotFound,
                             "cannot resolve projector linears (mm.*) with expected shapes");
    }
    tower->proj_fc1_w_ = store_name(std::string(fc1_name));
    tower->proj_fc2_w_ = store_name(std::string(fc2_name));
    // Projector biases are optional (".weight" -> ".bias" sibling name).
    auto bias_of = [](std::string_view wname) {
        std::string b(wname);
        b.replace(b.size() - 7, 7, ".bias");
        return b;
    };
    tower->proj_fc1_b_ = find_weight(weights, bias_of(fc1_name));
    tower->proj_fc2_b_ = find_weight(weights, bias_of(fc2_name));

    // Projector output width doubles as the LM hidden size. When the mmproj
    // metadata declared it (clip.vision.projection_dim), cross-check;
    // otherwise adopt the weight shape.
    const int64_t fc2_out = fc2.shape().dim(0);
    if (config.output_dim > 0 && config.output_dim != fc2_out) {
        return Status::Error(ErrorCode::kInvalidFormat,
                             "projector output dim mismatch vs clip.vision.projection_dim");
    }
    tower->config_.output_dim = static_cast<int32_t>(fc2_out);
    return tower;
}

std::vector<std::string> PaddleOcrTower::weight_names() const {
    std::vector<std::string> names;
    names.emplace_back(patch_embd_w_);
    names.emplace_back(proj_fc1_w_);
    names.emplace_back(proj_fc2_w_);
    for (const auto& lw : layers_) {
        names.emplace_back(lw.q_w);
        names.emplace_back(lw.k_w);
        names.emplace_back(lw.v_w);
        names.emplace_back(lw.o_w);
        names.emplace_back(lw.up_w);
        names.emplace_back(lw.down_w);
    }
    return names;
}

Result<int32_t> PaddleOcrTower::TokenCount(int32_t width, int32_t height) const {
    auto geo = patch_geometry(config_, width, height);
    if (!geo.ok()) {
        return geo.status();
    }
    return geo.value().n_merged;
}

Result<VisionOutput> PaddleOcrTower::Encode(const media::Image& image, Backend& backend) const {
    if (!image.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "vision: invalid image");
    }
    const VisionConfig& cfg = config_;
    const int32_t hidden = cfg.hidden_size;
    const int32_t inter = cfg.intermediate_size;
    const int32_t heads = cfg.num_heads;
    const int32_t hd = cfg.head_dim();
    const int32_t m = cfg.spatial_merge_size;

    auto geo_result = patch_geometry(cfg, image.width, image.height);
    if (!geo_result.ok()) {
        return geo_result.status();
    }
    const PatchGeometry geo = geo_result.value();
    const int32_t n = geo.n_patches;

    // 1. Preprocess: smart resize (bicubic) + CLIP normalization.
    auto resized = media::ResizeBicubic(image, geo.width, geo.height);
    if (!resized.ok()) {
        return resized.status();
    }
    media::NormalizeInPlace(resized.value(), cfg.image_mean, cfg.image_std);

    // 2. Patchify and embed: x = patches @ Wp^T + bp  (+ interpolated pos emb)
    std::vector<float> patches = patchify(resized.value(), cfg.patch_size);
    std::vector<float> x(static_cast<size_t>(n) * static_cast<size_t>(hidden));
    {
        TensorView pv = view2d(patches, n, 3 * cfg.patch_size * cfg.patch_size);
        TensorView xv = view2d(x, n, hidden);
        if (auto s = backend.MatMul(xv, pv, patch_embd_w_); !s.ok()) {
            return s;
        }
        if (auto s = backend.AddBiasInPlace(xv, patch_embd_b_); !s.ok()) {
            return s;
        }
        std::vector<float> pos = interpolate_pos_embd(pos_embd_, hidden, geo.grid_h, geo.grid_w);
        TensorView posv = view2d(pos, n, hidden);
        if (auto s = backend.AddInPlace(xv, posv); !s.ok()) {
            return s;
        }
    }

    // 3. 2D rope tables for this grid.
    std::vector<float> rope_cos, rope_sin;
    build_rope2d_tables(cfg, geo.grid_h, geo.grid_w, rope_cos, rope_sin);
    TensorView cos_v = view2d(rope_cos, n, hd);
    TensorView sin_v = view2d(rope_sin, n, hd);

    // 4. ViT blocks.
    const auto rows = [](int64_t count, int64_t width) {
        return std::vector<float>(static_cast<size_t>(count) * static_cast<size_t>(width));
    };
    std::vector<float> hbuf = rows(n, hidden);
    std::vector<float> q = rows(n, heads * hd);
    std::vector<float> kbuf = rows(n, heads * hd);
    std::vector<float> vbuf = rows(n, heads * hd);
    std::vector<float> attn = rows(n, heads * hd);
    std::vector<float> proj = rows(n, hidden);
    std::vector<float> fbuf = rows(n, inter);
    std::vector<float> fout = rows(n, hidden);

    for (const auto& lw : layers_) {
        TensorView xv = view2d(x, n, hidden);
        // ln1 -> qkv (+bias) -> rope -> attention -> o proj (+bias) -> residual
        {
            TensorView hv = view2d(hbuf, n, hidden);
            if (auto s = backend.LayerNorm(hv, xv, lw.ln1_w, lw.ln1_b, cfg.layer_norm_eps);
                !s.ok()) {
                return s;
            }
            TensorView qv = view2d(q, n, heads * hd);
            TensorView kv = view2d(kbuf, n, heads * hd);
            TensorView vv = view2d(vbuf, n, heads * hd);
            if (auto s = backend.MatMul(qv, hv, lw.q_w); !s.ok()) {
                return s;
            }
            if (auto s = backend.MatMul(kv, hv, lw.k_w); !s.ok()) {
                return s;
            }
            if (auto s = backend.MatMul(vv, hv, lw.v_w); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddBiasInPlace(qv, lw.q_b); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddBiasInPlace(kv, lw.k_b); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddBiasInPlace(vv, lw.v_b); !s.ok()) {
                return s;
            }
            TensorView q3 = view3d(q, n, heads, hd);
            TensorView k3 = view3d(kbuf, n, heads, hd);
            TensorView v3 = view3d(vbuf, n, heads, hd);
            if (auto s = backend.RopeApply(q3, k3, cos_v, sin_v); !s.ok()) {
                return s;
            }
            TensorView av = view2d(attn, n, heads * hd);
            AttentionConfig acfg{
                .num_heads = heads,
                .num_kv_heads = heads,
                .head_dim = hd,
                .scale = 1.0f / std::sqrt(static_cast<float>(hd)),
            };
            if (auto s = backend.AttentionFull(av, q3, k3, v3, acfg); !s.ok()) {
                return s;
            }
            TensorView pv = view2d(proj, n, hidden);
            if (auto s = backend.MatMul(pv, av, lw.o_w); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddBiasInPlace(pv, lw.o_b); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddInPlace(xv, pv); !s.ok()) {
                return s;
            }
        }
        // ln2 -> up (+bias) -> gelu -> down (+bias) -> residual
        {
            TensorView hv = view2d(hbuf, n, hidden);
            if (auto s = backend.LayerNorm(hv, xv, lw.ln2_w, lw.ln2_b, cfg.layer_norm_eps);
                !s.ok()) {
                return s;
            }
            TensorView fv = view2d(fbuf, n, inter);
            if (auto s = backend.MatMul(fv, hv, lw.up_w); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddBiasInPlace(fv, lw.up_b); !s.ok()) {
                return s;
            }
            if (auto s = backend.GeluInPlace(fv, cfg.gelu_tanh); !s.ok()) {
                return s;
            }
            TensorView ov = view2d(fout, n, hidden);
            if (auto s = backend.MatMul(ov, fv, lw.down_w); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddBiasInPlace(ov, lw.down_b); !s.ok()) {
                return s;
            }
            if (auto s = backend.AddInPlace(xv, ov); !s.ok()) {
                return s;
            }
        }
    }

    // 5. Post LayerNorm.
    {
        TensorView xv = view2d(x, n, hidden);
        TensorView hv = view2d(hbuf, n, hidden);
        if (auto s = backend.LayerNorm(hv, xv, post_ln_w_, post_ln_b_, cfg.layer_norm_eps);
            !s.ok()) {
            return s;
        }
        x.swap(hbuf);
    }

    // 6. Projector: LN -> spatial merge -> fc1 -> gelu -> fc2.
    std::vector<float> merged;
    {
        TensorView xv = view2d(x, n, hidden);
        TensorView hv = view2d(hbuf, n, hidden);
        if (auto s = backend.LayerNorm(hv, xv, proj_norm_w_, proj_norm_b_, cfg.projector_norm_eps);
            !s.ok()) {
            return s;
        }
        // merge_patches reads hbuf on the host — pull the normalized result
        // back first (no-op on backends without device-resident activations).
        if (auto s = backend.SyncToHost(hv); !s.ok()) {
            return s;
        }
        merged = merge_patches(hbuf, geo.grid_h, geo.grid_w, hidden, m);
    }
    const int32_t n_merged = geo.n_merged;
    const int32_t merged_dim = m * m * hidden;
    std::vector<float> z = rows(n_merged, merged_dim);
    std::vector<float> out = rows(n_merged, cfg.output_dim);
    {
        TensorView mv = view2d(merged, n_merged, merged_dim);
        TensorView zv = view2d(z, n_merged, merged_dim);
        if (auto s = backend.MatMul(zv, mv, proj_fc1_w_); !s.ok()) {
            return s;
        }
        if (proj_fc1_b_.valid()) {
            if (auto s = backend.AddBiasInPlace(zv, proj_fc1_b_); !s.ok()) {
                return s;
            }
        }
        if (auto s = backend.GeluInPlace(zv, cfg.gelu_tanh); !s.ok()) {
            return s;
        }
        TensorView ov = view2d(out, n_merged, cfg.output_dim);
        if (auto s = backend.MatMul(ov, zv, proj_fc2_w_); !s.ok()) {
            return s;
        }
        if (proj_fc2_b_.valid()) {
            if (auto s = backend.AddBiasInPlace(ov, proj_fc2_b_); !s.ok()) {
                return s;
            }
        }
    }

    // The final matrix output lives in `out` (device-side on GPU backends);
    // synchronize before the host-side memcpy below.
    if (auto s = backend.SyncToHost(view2d(out, n_merged, cfg.output_dim)); !s.ok()) {
        return s;
    }

    // Move the result into an owned buffer for the caller.
    auto buf = OwnedBuffer::AllocateCpu(out.size() * sizeof(float), 64);
    if (!buf.ok()) {
        return buf.status();
    }
    VisionOutput result;
    result.storage = std::move(buf).value();
    std::memcpy(result.storage.data(), out.data(), out.size() * sizeof(float));
    result.embeddings =
        TensorView(result.storage.data(), DType::kF32, Shape({n_merged, cfg.output_dim}));
    result.n_tokens = n_merged;
    result.grid_h = geo.grid_h / m;
    result.grid_w = geo.grid_w / m;
    return result;
}

} // namespace pl::mllm::vision
