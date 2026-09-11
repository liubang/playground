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

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm::media {

// Value-semantic interleaved RGB image, f32, values in [0, 1], HWC layout.
//
// This module is deliberately decode-free: hosts hand us raw pixels
// (macOS: ImageIO for files, ScreenCaptureKit for screen capture), keeping
// the core library pure C++20 with zero third-party dependencies.
struct Image {
    int32_t width = 0;
    int32_t height = 0;
    // Row-major HWC pixels, size = width * height * 3.
    std::vector<float> pixels;

    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    }

    // Pointer to the first pixel of row y (3 * width floats).
    [[nodiscard]] const float* row(int32_t y) const {
        return pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
    }

    // Adopt raw 8-bit pixels (RGB or RGBA; alpha is dropped) into the f32
    // [0, 1] representation. stride_px is the source row pitch in pixels
    // (0 = tightly packed).
    [[nodiscard]] static Result<Image> FromRgb8(const uint8_t* data,
                                                int32_t width,
                                                int32_t height,
                                                int32_t stride_px = 0);
    [[nodiscard]] static Result<Image> FromRgba8(const uint8_t* data,
                                                 int32_t width,
                                                 int32_t height,
                                                 int32_t stride_px = 0);
};

// Qwen2-VL family "smart resize": both dimensions become multiples of
// `factor` (patch_size * spatial_merge_size), the area is clamped into
// [min_pixels, max_pixels] (0 = unbounded), and the aspect ratio is kept
// as closely as possible. Returns {0, 0} on invalid input or an extreme
// aspect ratio (> 200:1).
[[nodiscard]] std::pair<int32_t, int32_t> SmartResize(
    int32_t width, int32_t height, int32_t factor, int64_t min_pixels = 0, int64_t max_pixels = 0);

// Bilinear resize (half-pixel centers, like torch interpolate
// align_corners=false).
[[nodiscard]] Result<Image> ResizeBilinear(const Image& src, int32_t dst_w, int32_t dst_h);

// PIL-compatible bicubic resize (cubic convolution, a = -0.5, support
// scaled by the downscale factor). This matches the default resampling of
// the Qwen2-VL / PaddleOCR-VL image processors.
[[nodiscard]] Result<Image> ResizeBicubic(const Image& src, int32_t dst_w, int32_t dst_h);

// Channel-wise affine normalization: p = (p - mean[c]) / stddev[c].
void NormalizeInPlace(Image& img, const float mean[3], const float stddev[3]);

} // namespace pl::mllm::media
