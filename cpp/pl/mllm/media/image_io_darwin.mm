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

#include "cpp/pl/mllm/media/image_io.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <vector>

namespace pl::mllm::media {

namespace {

// Shared rasterization path: decode the first image of `src` into the RGB
// f32 representation. `what` labels errors (file path / "image data").
Result<Image> DecodeImageSource(CGImageSourceRef src, const std::string& what) {
    CGImageRef cg = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    if (cg == nullptr) {
        return Status::Error(ErrorCode::kInvalidFormat, "cannot decode image: " + what);
    }

    const size_t w = CGImageGetWidth(cg);
    const size_t h = CGImageGetHeight(cg);
    if (w == 0 || h == 0 || w > INT32_MAX || h > INT32_MAX) {
        CGImageRelease(cg);
        return Status::Error(ErrorCode::kInvalidFormat, "implausible image dimensions");
    }

    // Rasterize to tightly packed 8-bit RGBX (alpha ignored).
    std::vector<uint8_t> rgba(w * h * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(),
                                             w,
                                             h,
                                             8,
                                             w * 4,
                                             cs,
                                             kCGImageAlphaNoneSkipLast |
                                                 kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (ctx == nullptr) {
        CGImageRelease(cg);
        return Status::Error(ErrorCode::kInternal, "bitmap context creation failed");
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), cg);
    CGContextRelease(ctx);
    CGImageRelease(cg);

    return Image::FromRgba8(rgba.data(), static_cast<int32_t>(w), static_cast<int32_t>(h));
}

} // namespace

Result<Image> LoadImageFile(const std::string& path) {
    @autoreleasepool {
        NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
        if (ns_path == nil) {
            return Status::Error(ErrorCode::kInvalidArgument, "bad image path encoding");
        }
        CGImageSourceRef src = CGImageSourceCreateWithURL(
            (__bridge CFURLRef)[NSURL fileURLWithPath:ns_path], nullptr);
        if (src == nullptr) {
            return Status::Error(ErrorCode::kNotFound, "cannot open image: " + path);
        }
        Result<Image> result = DecodeImageSource(src, path);
        CFRelease(src);
        return result;
    }
}

Result<Image> LoadImageData(const void* data, size_t size) {
    @autoreleasepool {
        if (data == nullptr || size == 0) {
            return Status::Error(ErrorCode::kInvalidArgument, "empty image data");
        }
        CFDataRef cf_data =
            CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                        static_cast<const UInt8*>(data),
                                        static_cast<CFIndex>(size),
                                        kCFAllocatorNull);
        if (cf_data == nullptr) {
            return Status::Error(ErrorCode::kInternal, "CFData creation failed");
        }
        CGImageSourceRef src = CGImageSourceCreateWithData(cf_data, nullptr);
        CFRelease(cf_data);
        if (src == nullptr) {
            return Status::Error(ErrorCode::kInvalidFormat, "cannot decode image data");
        }
        Result<Image> result = DecodeImageSource(src, "image data");
        CFRelease(src);
        return result;
    }
}

} // namespace pl::mllm::media
