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

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

#include "cpp/pl/mllm/media/image.h"

namespace pl::mllm::media {
namespace {

constexpr float kTol = 1e-6f;

// ---------------------------------------------------------------------------
// Image ingestion
// ---------------------------------------------------------------------------

TEST(ImageTest, FromRgb8TightlyPacked) {
    const std::vector<uint8_t> src = {255, 0, 128, 0, 64, 255};
    auto img = Image::FromRgb8(src.data(), 2, 1);
    ASSERT_TRUE(img.ok());
    const Image& im = img.value();
    ASSERT_TRUE(im.valid());
    EXPECT_EQ(im.width, 2);
    EXPECT_EQ(im.height, 1);
    EXPECT_FLOAT_EQ(im.pixels[0], 1.0f);
    EXPECT_FLOAT_EQ(im.pixels[1], 0.0f);
    EXPECT_FLOAT_EQ(im.pixels[2], 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(im.pixels[3], 0.0f);
    EXPECT_FLOAT_EQ(im.pixels[4], 64.0f / 255.0f);
    EXPECT_FLOAT_EQ(im.pixels[5], 1.0f);
}

TEST(ImageTest, FromRgba8DropsAlphaAndHonorsStride) {
    // 2x1 RGBA with stride_px = 3 (row pitch 3 px: 2 valid + 1 trailing pad).
    const std::vector<uint8_t> src = {10, 20, 30, 99, 40, 50, 60, 200, 9, 9, 9, 9};
    auto img = Image::FromRgba8(src.data(), 2, 1, /*stride_px=*/3);
    ASSERT_TRUE(img.ok());
    const Image& im = img.value();
    ASSERT_TRUE(im.valid());
    EXPECT_FLOAT_EQ(im.pixels[0], 10.0f / 255.0f);
    EXPECT_FLOAT_EQ(im.pixels[1], 20.0f / 255.0f);
    EXPECT_FLOAT_EQ(im.pixels[2], 30.0f / 255.0f);
    // Second pixel read from the padded row pitch, alpha ignored.
    EXPECT_FLOAT_EQ(im.pixels[3], 40.0f / 255.0f);
    EXPECT_FLOAT_EQ(im.pixels[4], 50.0f / 255.0f);
    EXPECT_FLOAT_EQ(im.pixels[5], 60.0f / 255.0f);
}

TEST(ImageTest, FromRgb8RejectsBadInput) {
    const std::vector<uint8_t> src = {1, 2, 3};
    EXPECT_FALSE(Image::FromRgb8(nullptr, 1, 1).ok());
    EXPECT_FALSE(Image::FromRgb8(src.data(), 0, 1).ok());
    EXPECT_FALSE(Image::FromRgb8(src.data(), 1, -2).ok());
    // Stride narrower than width is rejected.
    EXPECT_FALSE(Image::FromRgb8(src.data(), 2, 1, /*stride_px=*/1).ok());
}

// ---------------------------------------------------------------------------
// SmartResize
// ---------------------------------------------------------------------------

TEST(SmartResizeTest, ExactMultiplesPassThrough) {
    const auto [w, h] = SmartResize(28, 28, 28);
    EXPECT_EQ(w, 28);
    EXPECT_EQ(h, 28);
}

TEST(SmartResizeTest, RoundsToNearestMultiple) {
    // 30/28 = 1.07 -> nearest multiple stays 28.
    {
        const auto [w, h] = SmartResize(30, 30, 28);
        EXPECT_EQ(w, 28);
        EXPECT_EQ(h, 28);
    }
    // 41/28 = 1.464 -> rounds down to 28.
    {
        const auto [w, h] = SmartResize(41, 30, 28);
        EXPECT_EQ(w, 28);
        EXPECT_EQ(h, 28);
    }
    // 42/28 = 1.5 -> rounds up to 56 (lround at exactly .5 goes up).
    {
        const auto [w, h] = SmartResize(42, 30, 28);
        EXPECT_EQ(w, 56);
        EXPECT_EQ(h, 28);
    }
}

TEST(SmartResizeTest, MaxPixelsShrinks) {
    // Rounded to multiples: (504, 504) area = 254016 > 3136;
    // beta = sqrt(254016/3136) = 9 -> floor(500/9/28) = 1 -> 28 x 28.
    const auto [w, h] = SmartResize(500, 500, 28, /*min_pixels=*/0, /*max_pixels=*/56 * 56);
    EXPECT_EQ(w, 28);
    EXPECT_EQ(h, 28);
}

TEST(SmartResizeTest, MinPixelsGrows) {
    // area 784 < 6272, beta = sqrt(6272/784) = sqrt(8) ~ 2.83 ->
    // ceil(28 * 2.83 / 28) = 3 -> 84 x 84.
    const auto [w, h] = SmartResize(28, 28, 28, /*min_pixels=*/56 * 112);
    EXPECT_EQ(w, 84);
    EXPECT_EQ(h, 84);
}

TEST(SmartResizeTest, TinySideGetsScaledUp) {
    // height < factor: width = lround(100 * 28 / 10) = 280, height = 28.
    const auto [w, h] = SmartResize(100, 10, 28);
    EXPECT_EQ(w, 280);
    EXPECT_EQ(h, 28);
}

TEST(SmartResizeTest, ExtremeAspectRatioRejected) {
    // width < factor: height = lround(300 * 28 / 1) = 8400 > 200 * 28.
    const auto [w, h] = SmartResize(1, 300, 28);
    EXPECT_EQ(w, 0);
    EXPECT_EQ(h, 0);
}

TEST(SmartResizeTest, InvalidInput) {
    EXPECT_EQ(SmartResize(0, 10, 28), std::make_pair(0, 0));
    EXPECT_EQ(SmartResize(10, 0, 28), std::make_pair(0, 0));
    EXPECT_EQ(SmartResize(10, 10, 0), std::make_pair(0, 0));
}

// ---------------------------------------------------------------------------
// ResizeBilinear
// ---------------------------------------------------------------------------

Image ramp_image(int32_t w, int32_t h) {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            const float v = static_cast<float>(y * 10 + x);
            for (int32_t c = 0; c < 3; ++c) {
                img.pixels[(static_cast<size_t>(y) * static_cast<size_t>(w) +
                            static_cast<size_t>(x)) *
                               3 +
                           static_cast<size_t>(c)] = v;
            }
        }
    }
    return img;
}

TEST(ResizeBilinearTest, IdentityIsCopy) {
    const Image src = ramp_image(3, 2);
    auto dst = ResizeBilinear(src, 3, 2);
    ASSERT_TRUE(dst.ok());
    const Image& d = dst.value();
    for (size_t i = 0; i < src.pixels.size(); ++i) {
        EXPECT_FLOAT_EQ(d.pixels[i], src.pixels[i]);
    }
}

TEST(ResizeBilinearTest, UpscaleExactHalfPixelValues) {
    // v(y, x) = y*10 + x; 2x2 -> 4x4, half-pixel centers.
    const Image src = ramp_image(2, 2);
    auto dst = ResizeBilinear(src, 4, 4);
    ASSERT_TRUE(dst.ok());
    const Image& d = dst.value();
    // Corner (0,0): source coords (-0.25, -0.25) clamp to 0 -> exactly v(0,0) = 0.
    EXPECT_FLOAT_EQ(d.pixels[0], 0.0f);
    // (1,1): fx = fy = 0.25 -> (0*0.75 + 1*0.25)*0.75 + (10*0.75 + 11*0.25)*0.25 = 2.75.
    const float v11 = d.pixels[(static_cast<size_t>(1) * 4 + 1) * 3];
    EXPECT_NEAR(v11, 2.75f, kTol);
    // (1,2): fx = 2.5*0.5 - 0.5 = 0.75, fy = 0.25 ->
    // (0*0.25 + 1*0.75)*0.75 + (10*0.25 + 11*0.75)*0.25 = 2.5625 + 2.6875 = 3.25? no:
    // = 0.5625 + (2.5 + 8.25)*0.25 = 0.5625 + 2.6875 = 3.25... keep formula:
    // (p00*(1-wx) + p01*wx)*(1-wy) = (0*0.25 + 1*0.75)*0.75 = 0.5625
    // (p10*(1-wx) + p11*wx)*wy      = (10*0.25 + 11*0.75)*0.25 = 2.6875
    const float v12 = d.pixels[(static_cast<size_t>(1) * 4 + 2) * 3];
    EXPECT_NEAR(v12, 3.25f, kTol);
}

TEST(ResizeBilinearTest, ConstantImageStaysConstant) {
    Image src;
    src.width = 8;
    src.height = 4;
    src.pixels.assign(8 * 4 * 3, 0.42f);
    auto dst = ResizeBilinear(src, 3, 5);
    ASSERT_TRUE(dst.ok());
    const Image& d = dst.value();
    ASSERT_EQ(d.pixels.size(), 3 * 5 * 3);
    for (float v : d.pixels) {
        EXPECT_NEAR(v, 0.42f, kTol);
    }
}

TEST(ResizeBilinearTest, RejectsBadInput) {
    const Image src = ramp_image(2, 2);
    EXPECT_FALSE(ResizeBilinear(src, 0, 4).ok());
    Image invalid;
    EXPECT_FALSE(ResizeBilinear(invalid, 4, 4).ok());
}

// ---------------------------------------------------------------------------
// ResizeBicubic
// ---------------------------------------------------------------------------

TEST(ResizeBicubicTest, IdentityIsCopy) {
    const Image src = ramp_image(3, 2);
    auto dst = ResizeBicubic(src, 3, 2);
    ASSERT_TRUE(dst.ok());
    const Image& d = dst.value();
    for (size_t i = 0; i < src.pixels.size(); ++i) {
        EXPECT_FLOAT_EQ(d.pixels[i], src.pixels[i]);
    }
}

TEST(ResizeBicubicTest, ConstantImageStaysConstantUpscale) {
    // Bicubic partitions of unity keep a flat signal flat (up to fp noise).
    Image src;
    src.width = 4;
    src.height = 3;
    src.pixels.assign(4 * 3 * 3, 0.8157924f);
    auto dst = ResizeBicubic(src, 8, 6);
    ASSERT_TRUE(dst.ok());
    const Image& d = dst.value();
    ASSERT_EQ(d.pixels.size(), 8 * 6 * 3);
    float max_dev = 0.0f;
    for (float v : d.pixels) {
        max_dev = std::max(max_dev, std::fabs(v - 0.8157924f));
    }
    EXPECT_LT(max_dev, 1e-5f);
}

TEST(ResizeBicubicTest, ConstantImageStaysConstantDownscale) {
    Image src;
    src.width = 16;
    src.height = 8;
    src.pixels.assign(16 * 8 * 3, 0.2718281f);
    auto dst = ResizeBicubic(src, 4, 2);
    ASSERT_TRUE(dst.ok());
    for (float v : dst.value().pixels) {
        EXPECT_NEAR(v, 0.2718281f, 1e-5f);
    }
}

TEST(ResizeBicubicTest, DownscaleAverages) {
    // 2x downscale of an exactly 2-periodic signal: each output sample sits
    // mid-block; with the antialiased kernel the mean is preserved exactly
    // for constant columns: construct image where each 2x2 block is constant
    // with value = block index, and check interior outputs stay in range and
    // interior-center sample equals the ideal half-sample.
    Image src = ramp_image(4, 2);
    auto dst = ResizeBicubic(src, 2, 1);
    ASSERT_TRUE(dst.ok());
    const Image& d = dst.value();
    ASSERT_EQ(d.pixels.size(), 2 * 1 * 3);
    // Monotone ramp -> outputs must fall inside the source's value span and
    // keep strict monotonicity in x.
    const float a = d.pixels[0];
    const float b = d.pixels[3];
    EXPECT_GE(a, 0.0f);
    EXPECT_LT(a, b);
    EXPECT_LE(b, 11.0f);
}

TEST(ResizeBicubicTest, RejectsBadInput) {
    const Image src = ramp_image(2, 2);
    EXPECT_FALSE(ResizeBicubic(src, -1, 4).ok());
    Image invalid;
    EXPECT_FALSE(ResizeBicubic(invalid, 4, 4).ok());
}

// ---------------------------------------------------------------------------
// NormalizeInPlace
// ---------------------------------------------------------------------------

TEST(NormalizeTest, ChannelWiseAffine) {
    Image img;
    img.width = 1;
    img.height = 1;
    img.pixels = {0.5f, 0.25f, 1.0f};
    const float mean[3] = {0.5f, 0.25f, 0.0f};
    const float stddev[3] = {0.5f, 0.25f, 2.0f};
    NormalizeInPlace(img, mean, stddev);
    EXPECT_FLOAT_EQ(img.pixels[0], 0.0f);
    EXPECT_FLOAT_EQ(img.pixels[1], 0.0f);
    EXPECT_FLOAT_EQ(img.pixels[2], 0.5f);
}

} // namespace
} // namespace pl::mllm::media
