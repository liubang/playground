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

#include "cpp/pl/mllm/media/image.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace pl::mllm::media {

namespace {

// Shared u8 -> f32 [0, 1] ingest for interleaved sources with `channels`
// per pixel (alpha is ignored when channels == 4).
Result<Image> from_u8(
    const uint8_t* data, int32_t width, int32_t height, int32_t channels, int32_t stride_px) {
    if (data == nullptr || width <= 0 || height <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "image: bad input");
    }
    const int32_t stride = stride_px > 0 ? stride_px : width;
    if (stride < width) {
        return Status::Error(ErrorCode::kInvalidArgument, "image: stride < width");
    }
    Image img;
    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);

    constexpr float kScale = 1.0f / 255.0f;
    for (int32_t y = 0; y < height; ++y) {
        const uint8_t* src = data + static_cast<size_t>(y) * static_cast<size_t>(stride) *
                                        static_cast<size_t>(channels);
        float* dst = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
        for (int32_t x = 0; x < width; ++x) {
            dst[x * 3 + 0] = static_cast<float>(src[x * channels + 0]) * kScale;
            dst[x * 3 + 1] = static_cast<float>(src[x * channels + 1]) * kScale;
            dst[x * 3 + 2] = static_cast<float>(src[x * channels + 2]) * kScale;
        }
    }
    return img;
}

} // namespace

Result<Image> Image::FromRgb8(const uint8_t* data,
                              int32_t width,
                              int32_t height,
                              int32_t stride_px) {
    return from_u8(data, width, height, /*channels=*/3, stride_px);
}

Result<Image> Image::FromRgba8(const uint8_t* data,
                               int32_t width,
                               int32_t height,
                               int32_t stride_px) {
    return from_u8(data, width, height, /*channels=*/4, stride_px);
}

// Qwen2-VL family smart resize (mirrors the reference Python semantics:
// round-to-nearest multiple, then shrink via floor or grow via ceil when
// the area leaves the [min_pixels, max_pixels] band).
std::pair<int32_t, int32_t> SmartResize(
    int32_t width, int32_t height, int32_t factor, int64_t min_pixels, int64_t max_pixels) {
    if (width <= 0 || height <= 0 || factor <= 0) {
        return {0, 0};
    }
    const double f = static_cast<double>(factor);
    if (height < factor) {
        width = static_cast<int32_t>(std::lround(static_cast<double>(width) * f / height));
        height = factor;
    }
    if (width < factor) {
        height = static_cast<int32_t>(std::lround(static_cast<double>(height) * f / width));
        width = factor;
    }
    if (std::max(height, width) > 200 * std::min(height, width)) {
        return {0, 0}; // extreme aspect ratio
    }
    auto round_to = [&](int32_t v) {
        return std::max(factor,
                        static_cast<int32_t>(std::lround(static_cast<double>(v) / f)) * factor);
    };
    int32_t h = round_to(height);
    int32_t w = round_to(width);

    const int64_t area = static_cast<int64_t>(h) * w;
    if (max_pixels > 0 && area > max_pixels) {
        const double beta = std::sqrt(static_cast<double>(area) / static_cast<double>(max_pixels));
        h = static_cast<int32_t>(std::floor(height / beta / f)) * factor;
        w = static_cast<int32_t>(std::floor(width / beta / f)) * factor;
    } else if (min_pixels > 0 && area < min_pixels) {
        const double beta = std::sqrt(static_cast<double>(min_pixels) / static_cast<double>(area));
        h = static_cast<int32_t>(std::ceil(height * beta / f)) * factor;
        w = static_cast<int32_t>(std::ceil(width * beta / f)) * factor;
    }
    return {w, h};
}

namespace {

// Cubic convolution kernel (Keys, a = -0.5) — PIL bicubic.
float bicubic_kernel(float x) {
    const float ax = std::fabs(x);
    if (ax < 1.0f) {
        return ((1.5f * ax - 2.5f) * ax) * ax + 1.0f;
    }
    if (ax < 2.0f) {
        return (((-0.5f * ax) + 2.5f) * ax - 4.0f) * ax + 2.0f;
    }
    return 0.0f;
}

// One-dimensional separable resize pass (PIL-compatible bicubic).
Image resize_bicubic_1d(const Image& src, int32_t dst_w, int32_t dst_h, bool horizontal) {
    const int32_t n_src = horizontal ? src.width : src.height;
    const int32_t n_dst = horizontal ? dst_w : dst_h;

    Image out;
    out.width = dst_w;
    out.height = dst_h;
    out.pixels.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 3);

    const double scale = static_cast<double>(n_src) / static_cast<double>(n_dst);
    // Downscaling widens the kernel footprint (PIL antialiasing).
    const double filter_scale = std::max(1.0, scale);
    const double support = 2.0 * filter_scale;

    for (int32_t d = 0; d < n_dst; ++d) {
        const double center = (static_cast<double>(d) + 0.5) * scale;
        const int32_t lo = std::max(0, static_cast<int32_t>(std::ceil(center - support)));
        const int32_t hi = std::min(n_src, static_cast<int32_t>(std::floor(center + support)) + 1);

        // Gather + normalize the kernel weights for this output coordinate.
        std::vector<std::pair<int32_t, float>> taps;
        taps.reserve(static_cast<size_t>(std::max(0, hi - lo)));
        float wsum = 0.0f;
        for (int32_t s = lo; s < hi; ++s) {
            const float wgt = bicubic_kernel(
                static_cast<float>((static_cast<double>(s) + 0.5 - center) / filter_scale));
            if (wgt != 0.0f) {
                taps.emplace_back(s, wgt);
                wsum += wgt;
            }
        }
        if (wsum != 0.0f) {
            for (auto& [idx, wgt] : taps) {
                wgt /= wsum;
            }
        }

        const int32_t sweep = horizontal ? src.height : dst_w;
        for (int32_t other = 0; other < sweep; ++other) {
            const int32_t oy = horizontal ? other : d;
            const int32_t ox = horizontal ? d : other;
            float* dst = out.pixels.data() + (static_cast<size_t>(oy) * static_cast<size_t>(dst_w) +
                                              static_cast<size_t>(ox)) *
                                                 3;
            for (int32_t c = 0; c < 3; ++c) {
                double acc = 0.0;
                for (const auto& [idx, wgt] : taps) {
                    const int32_t sy = horizontal ? other : idx;
                    const int32_t sx = horizontal ? idx : other;
                    acc += static_cast<double>(wgt) * static_cast<double>(src.row(sy)[sx * 3 + c]);
                }
                dst[c] = static_cast<float>(acc);
            }
        }
    }
    return out;
}

} // namespace

Result<Image> ResizeBicubic(const Image& src, int32_t dst_w, int32_t dst_h) {
    if (!src.valid() || dst_w <= 0 || dst_h <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "resize: bad input");
    }
    if (dst_w == src.width && dst_h == src.height) {
        return src;
    }
    // Two separable passes: horizontal, then vertical.
    Image tmp = resize_bicubic_1d(src, dst_w, src.height, /*horizontal=*/true);
    return resize_bicubic_1d(tmp, dst_w, dst_h, /*horizontal=*/false);
}

Result<Image> ResizeBilinear(const Image& src, int32_t dst_w, int32_t dst_h) {
    if (!src.valid() || dst_w <= 0 || dst_h <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "resize: bad input");
    }
    if (dst_w == src.width && dst_h == src.height) {
        return src;
    }

    Image out;
    out.width = dst_w;
    out.height = dst_h;
    out.pixels.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 3);

    // Half-pixel-center mapping (what vision preprocessors use).
    const float sx = static_cast<float>(src.width) / static_cast<float>(dst_w);
    const float sy = static_cast<float>(src.height) / static_cast<float>(dst_h);

    for (int32_t oy = 0; oy < dst_h; ++oy) {
        const float fy = (static_cast<float>(oy) + 0.5f) * sy - 0.5f;
        const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(fy)), 0, src.height - 1);
        const int32_t y1 = std::min(y0 + 1, src.height - 1);
        const float wy = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);

        for (int32_t ox = 0; ox < dst_w; ++ox) {
            const float fx = (static_cast<float>(ox) + 0.5f) * sx - 0.5f;
            const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(fx)), 0, src.width - 1);
            const int32_t x1 = std::min(x0 + 1, src.width - 1);
            const float wx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);

            const float* r0 = src.row(y0);
            const float* r1 = src.row(y1);
            float* dst = out.pixels.data() + (static_cast<size_t>(oy) * static_cast<size_t>(dst_w) +
                                              static_cast<size_t>(ox)) *
                                                 3;
            for (int32_t c = 0; c < 3; ++c) {
                const float p00 = r0[x0 * 3 + c];
                const float p01 = r0[x1 * 3 + c];
                const float p10 = r1[x0 * 3 + c];
                const float p11 = r1[x1 * 3 + c];

                dst[c] = (p00 * (1.0f - wx) + p01 * wx) * (1.0f - wy) +
                         (p10 * (1.0f - wx) + p11 * wx) * wy;
            }
        }
    }
    return out;
}

void NormalizeInPlace(Image& img, const float mean[3], const float stddev[3]) {
    const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    for (size_t i = 0; i < n; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            img.pixels[i * 3 + c] = (img.pixels[i * 3 + c] - mean[c]) / stddev[c];
        }
    }
}

} // namespace pl::mllm::media
