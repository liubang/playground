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


// Metal backend implementation (Objective-C++). All Metal types are confined
// to this file (and shader_source.h) — the public header stays pure C++.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/pl/mllm/backend/metal/metal_backend.h"
#include "cpp/pl/mllm/backend/metal/shader_source.h"
#include "cpp/pl/mllm/core/dtype.h"

namespace pl::mllm {

namespace {

// f32 conversion helper (mirrors CPU backend's elem_to_f32)

struct Q8Block {
    uint16_t scale; // fp16
    int8_t qs[32];
};
static_assert(sizeof(Q8Block) == kQ8_0TypeSize);

float elem_to_f32(const void* base, DType dtype, int64_t idx) {
    switch (dtype) {
    case DType::kF32:
        return static_cast<const float*>(base)[static_cast<size_t>(idx)];
    case DType::kF16:
        return fp16_to_fp32(static_cast<const uint16_t*>(base)[static_cast<size_t>(idx)]);
    case DType::kQ8_0: {
        const auto* blocks = static_cast<const Q8Block*>(base);
        const int64_t block_idx = idx / kQ8_0BlockSize;
        const int64_t in_block = idx % kQ8_0BlockSize;
        const float scale = fp16_to_fp32(blocks[block_idx].scale);
        return scale * static_cast<float>(blocks[block_idx].qs[in_block]);
    }
    default:
        return 0.0f;
    }
}

// Convert any supported tensor (f32/f16/Q8_0) into a host f32 vector.
std::vector<float> to_f32(const TensorView& t) {
    const int64_t numel = t.shape().numel();
    std::vector<float> out(static_cast<size_t>(numel));
    for (int64_t i = 0; i < numel; ++i) {
        out[static_cast<size_t>(i)] = elem_to_f32(t.data(), t.dtype(), i);
    }
    return out;
}

// MPS rowBytes must be 16-byte aligned.
size_t align16(size_t n) { return (n + 15u) & ~static_cast<size_t>(15u); }

} // namespace

// Impl

struct MetalBackend::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    id<MTLComputePipelineState> rmsnorm_ps = nil;
    id<MTLComputePipelineState> rope_ps = nil;
    id<MTLComputePipelineState> attention_ps = nil;
    id<MTLComputePipelineState> swiglu_ps = nil;
    id<MTLComputePipelineState> add_ps = nil;
    id<MTLComputePipelineState> add_bias_ps = nil;
    id<MTLComputePipelineState> gemv_ps = nil;

    // Weight table. f32/f16 weights are converted to f32 at import; Q8_0
    // weights are kept in their raw block layout for the fused GEMV kernel.
    struct Weight {
        id<MTLBuffer> buf = nil;
        Shape shape;
        DType dtype = DType::kF32;
    };
    std::unordered_map<std::string, Weight> weights_;

    // Constructor errors surface on the first op call.
    Status init_error;

    explicit Impl() { init(); }

private:
    void init() {
        device = MTLCreateSystemDefaultDevice();
        if (!device) {
            init_error = Status::Error(ErrorCode::kBackendFailure,
                                       "MetalBackend: no Metal device");
            return;
        }
        queue = [device newCommandQueue];
        NSError* err = nil;
        library = [device newLibraryWithSource:@(metal::kShaderSource)
                                       options:nil
                                         error:&err];
        if (!library) {
            init_error = Status::Error(
                ErrorCode::kBackendFailure,
                "MetalBackend: shader compile failed: " +
                    std::string(err.localizedDescription.UTF8String));
            return;
        }
        auto make_ps = [&](const char* name) -> id<MTLComputePipelineState> {
            id<MTLFunction> fn = [library newFunctionWithName:@(name)];
            if (!fn) {
                return nil;
            }
            NSError* ps_err = nil;
            id<MTLComputePipelineState> ps =
                [device newComputePipelineStateWithFunction:fn error:&ps_err];
            if (!ps) {
                init_error = Status::Error(
                    ErrorCode::kBackendFailure,
                    "MetalBackend: pipeline " + std::string(name) + " failed: " +
                        std::string(ps_err.localizedDescription.UTF8String));
                return nil;
            }
            return ps;
        };
        rmsnorm_ps = make_ps("mllm_rmsnorm");
        rope_ps = make_ps("mllm_rope");
        attention_ps = make_ps("mllm_attention");
        swiglu_ps = make_ps("mllm_swiglu");
        add_ps = make_ps("mllm_add_inplace");
        add_bias_ps = make_ps("mllm_add_bias");
        gemv_ps = make_ps("mllm_gemv_q8_0");
        if (!init_error.ok()) {
            return;
        }
        if (!rmsnorm_ps || !rope_ps || !attention_ps || !swiglu_ps || !add_ps ||
            !add_bias_ps || !gemv_ps) {
            init_error = Status::Error(ErrorCode::kBackendFailure,
                                       "MetalBackend: missing pipeline state");
        }
    }
};

// Construction

MetalBackend::MetalBackend() : impl_(new Impl()) {}
MetalBackend::~MetalBackend() = default;

namespace {

Status ensure_ready(MetalBackend::Impl& impl) {
    return impl.init_error;
}

Status check_contig_valid(const TensorView& t, std::string_view op) {
    if (!t.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             std::string(op) + ": invalid tensor");
    }
    if (!t.is_contiguous()) {
        return Status::Error(ErrorCode::kUnsupported,
                             std::string(op) + ": non-contiguous tensor");
    }
    return {};
}

id<MTLBuffer> upload(MetalBackend::Impl& impl, const std::vector<float>& host) {
    return [impl.device newBufferWithBytes:host.data()
                                    length:host.size() * sizeof(float)
                                   options:MTLResourceStorageModeShared];
}

} // namespace

// ImportWeights

Status MetalBackend::ImportWeights(std::span<const TensorView> weights,
                                   std::span<const std::string_view> names) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (weights.size() != names.size()) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "MetalBackend::ImportWeights: size mismatch");
    }
    for (size_t i = 0; i < weights.size(); ++i) {
        const TensorView& w = weights[i];
        if (!w.valid()) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MetalBackend::ImportWeights: invalid weight");
        }
        Impl::Weight wg;
        wg.shape = w.shape();
        if (w.dtype() == DType::kQ8_0) {
            // Keep the raw quantized block layout for the fused GEMV kernel.
            id<MTLBuffer> buf = [impl_->device
                newBufferWithBytes:w.data()
                            length:w.byte_size()
                           options:MTLResourceStorageModeShared];
            if (!buf) {
                return Status::Error(ErrorCode::kBackendFailure,
                                     "MetalBackend: buffer alloc failed");
            }
            wg.buf = buf;
            wg.dtype = DType::kQ8_0;
        } else {
            // Convert f16 -> f32 and upload.
            const std::vector<float> host = to_f32(w);
            id<MTLBuffer> buf = [impl_->device
                newBufferWithBytes:host.data()
                            length:host.size() * sizeof(float)
                           options:MTLResourceStorageModeShared];
            if (!buf) {
                return Status::Error(ErrorCode::kBackendFailure,
                                     "MetalBackend: buffer alloc failed");
            }
            wg.buf = buf;
            wg.dtype = DType::kF32;
        }
        impl_->weights_[std::string(names[i])] = wg;
    }
    return {};
}

// MatMul (MPS)

Status MetalBackend::MatMul(TensorView out, TensorView x,
                            std::string_view weight_name) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "MatMul"); !s.ok()) return s;
    if (auto s = check_contig_valid(x, "MatMul"); !s.ok()) return s;

    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t in_dim = static_cast<int32_t>(x.shape().dim(1));

    auto it = impl_->weights_.find(std::string(weight_name));
    if (it == impl_->weights_.end()) {
        return Status::Error(ErrorCode::kNotFound,
                             "MatMul: weight '" + std::string(weight_name) + "' not found");
    }
    const Impl::Weight& w = it->second;
    const int32_t out_dim = static_cast<int32_t>(w.shape.dim(0));

    if (out.shape().dim(0) != batch || out.shape().dim(1) != out_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "MatMul: output shape mismatch");
    }
    if (w.shape.dim(1) != in_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "MatMul: in_dim mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported,
                             "MatMul: output must be f32");
    }

    // Upload x (f32) and allocate output buffer.
    id<MTLBuffer> xbuf = upload(*impl_, to_f32(x));
    id<MTLBuffer> obuf = [impl_->device
        newBufferWithLength:static_cast<size_t>(batch) * static_cast<size_t>(out_dim) * sizeof(float)
                    options:MTLResourceStorageModeShared];
    if (!xbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "MatMul: buffer alloc failed");
    }

    // Q8_0 weights: fused dequant GEMV kernel (decode path).
    if (w.dtype == DType::kQ8_0) {
        if (batch != 1) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: Q8_0 GEMV requires batch == 1");
        }
        if (in_dim % kQ8_0BlockSize != 0) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMul: in_dim not Q8_0 block-aligned");
        }
        const uint32_t in_d = static_cast<uint32_t>(in_dim);
        const uint32_t out_d = static_cast<uint32_t>(out_dim);

        id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:impl_->gemv_ps];
        [enc setBuffer:obuf offset:0 atIndex:0];
        [enc setBuffer:xbuf offset:0 atIndex:1];
        [enc setBuffer:w.buf offset:0 atIndex:2];
        [enc setBytes:&in_d length:sizeof(in_d) atIndex:3];
        [enc setBytes:&out_d length:sizeof(out_d) atIndex:4];
        [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(out_dim), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error != nil) {
            return Status::Error(ErrorCode::kBackendFailure,
                                 "MatMul: command buffer error");
        }
        std::memcpy(out.data(), obuf.contents,
                    static_cast<size_t>(out_dim) * sizeof(float));
        return {};
    }

    // Plain weights: MPS matrix multiply (C = x * W^T).
    const size_t a_row = align16(static_cast<size_t>(in_dim) * sizeof(float));
    const size_t b_row = align16(static_cast<size_t>(in_dim) * sizeof(float));
    const size_t c_row = align16(static_cast<size_t>(out_dim) * sizeof(float));

    MPSMatrixDescriptor* a_desc = [MPSMatrixDescriptor
        matrixDescriptorWithRows:static_cast<NSUInteger>(batch)
                         columns:static_cast<NSUInteger>(in_dim)
                        rowBytes:a_row
                        dataType:MPSDataTypeFloat32];
    MPSMatrixDescriptor* b_desc = [MPSMatrixDescriptor
        matrixDescriptorWithRows:static_cast<NSUInteger>(out_dim)
                         columns:static_cast<NSUInteger>(in_dim)
                        rowBytes:b_row
                        dataType:MPSDataTypeFloat32];
    MPSMatrixDescriptor* c_desc = [MPSMatrixDescriptor
        matrixDescriptorWithRows:static_cast<NSUInteger>(batch)
                         columns:static_cast<NSUInteger>(out_dim)
                        rowBytes:c_row
                        dataType:MPSDataTypeFloat32];

    MPSMatrix* mA = [[MPSMatrix alloc] initWithBuffer:xbuf descriptor:a_desc];
    MPSMatrix* mB = [[MPSMatrix alloc] initWithBuffer:w.buf descriptor:b_desc];
    MPSMatrix* mC = [[MPSMatrix alloc] initWithBuffer:obuf descriptor:c_desc];

    MPSMatrixMultiplication* mul = [[MPSMatrixMultiplication alloc]
        initWithDevice:impl_->device
         transposeLeft:NO
        transposeRight:YES
          resultRows:static_cast<NSUInteger>(batch)
       resultColumns:static_cast<NSUInteger>(out_dim)
     interiorColumns:static_cast<NSUInteger>(in_dim)
                alpha:1.0
                 beta:0.0];
    if (!mul) {
        return Status::Error(ErrorCode::kBackendFailure, "MatMul: MPS init failed");
    }

    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    [mul encodeToCommandBuffer:cb leftMatrix:mA rightMatrix:mB resultMatrix:mC];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "MatMul: command buffer error");
    }

    std::memcpy(out.data(), obuf.contents,
                static_cast<size_t>(batch) * static_cast<size_t>(out_dim) * sizeof(float));
    return {};
}

// RmsNorm

Status MetalBackend::RmsNorm(TensorView out, TensorView x, TensorView weight,
                             float eps) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "RmsNorm"); !s.ok()) return s;
    if (auto s = check_contig_valid(x, "RmsNorm"); !s.ok()) return s;
    if (auto s = check_contig_valid(weight, "RmsNorm"); !s.ok()) return s;

    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t hidden = static_cast<int32_t>(x.shape().dim(1));
    if (out.shape() != x.shape() || weight.shape().numel() != hidden) {
        return Status::Error(ErrorCode::kInvalidArgument, "RmsNorm: shape mismatch");
    }
    if (!(eps > 0.0f)) {
        return Status::Error(ErrorCode::kInvalidArgument, "RmsNorm: eps must be positive");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "RmsNorm: output must be f32");
    }

    id<MTLBuffer> xbuf = upload(*impl_, to_f32(x));
    id<MTLBuffer> wbuf = upload(*impl_, to_f32(weight));
    id<MTLBuffer> obuf = [impl_->device
        newBufferWithLength:static_cast<size_t>(batch) * static_cast<size_t>(hidden) * sizeof(float)
                    options:MTLResourceStorageModeShared];
    if (!xbuf || !wbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "RmsNorm: buffer alloc failed");
    }

    const uint32_t n = static_cast<uint32_t>(hidden);
    const float e = eps;

    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:impl_->rmsnorm_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:xbuf offset:0 atIndex:1];
    [enc setBuffer:wbuf offset:0 atIndex:2];
    [enc setBytes:&n length:sizeof(n) atIndex:3];
    [enc setBytes:&e length:sizeof(e) atIndex:4];
    const uint32_t tsize = static_cast<uint32_t>(std::min<int64_t>(
        hidden, static_cast<int64_t>(impl_->device.maxThreadsPerThreadgroup.width)));
    [enc setThreadgroupMemoryLength:tsize * sizeof(float) atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake(static_cast<NSUInteger>(batch), 1, 1)
         threadsPerThreadgroup:MTLSizeMake(tsize, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "RmsNorm: command buffer error");
    }

    std::memcpy(out.data(), obuf.contents,
                static_cast<size_t>(batch) * static_cast<size_t>(hidden) * sizeof(float));
    return {};
}

// RoPE

Status MetalBackend::RoPE(TensorView q, TensorView k, int64_t position,
                          const RopeConfig& config) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(q, "RoPE"); !s.ok()) return s;
    if (auto s = check_contig_valid(k, "RoPE"); !s.ok()) return s;

    if (q.shape().rank() != 3 || k.shape().rank() != 3) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "RoPE: expected 3D tensors [batch, heads, head_dim]");
    }
    const int32_t num_heads = static_cast<int32_t>(q.shape().dim(1));
    const int32_t num_kv_heads = static_cast<int32_t>(k.shape().dim(1));
    const int32_t head_dim = config.head_dim > 0
        ? config.head_dim
        : static_cast<int32_t>(q.shape().dim(2));
    if (q.shape().dim(2) != head_dim || k.shape().dim(2) != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: head_dim mismatch");
    }
    if (position < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: negative position");
    }
    if (head_dim % 2 != 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: head_dim must be even");
    }
    if (q.dtype() != k.dtype()) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: q/k dtype mismatch");
    }

    id<MTLBuffer> qbuf = upload(*impl_, to_f32(q));
    id<MTLBuffer> kbuf = upload(*impl_, to_f32(k));
    if (!qbuf || !kbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "RoPE: buffer alloc failed");
    }

    const int64_t q_elems = q.shape().numel();
    const int64_t k_elems = k.shape().numel();
    const uint32_t qe = static_cast<uint32_t>(q_elems);
    const uint32_t qh = static_cast<uint32_t>(num_heads);
    const uint32_t kh = static_cast<uint32_t>(num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(head_dim);
    const float fb = config.freq_base;
    const int32_t pos = static_cast<int32_t>(position);

    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:impl_->rope_ps];
    [enc setBuffer:qbuf offset:0 atIndex:0];
    [enc setBuffer:kbuf offset:0 atIndex:1];
    [enc setBytes:&hd length:sizeof(hd) atIndex:2];
    [enc setBytes:&fb length:sizeof(fb) atIndex:3];
    [enc setBytes:&pos length:sizeof(pos) atIndex:4];
    [enc setBytes:&qe length:sizeof(qe) atIndex:5];
    [enc setBytes:&qh length:sizeof(qh) atIndex:6];
    [enc setBytes:&kh length:sizeof(kh) atIndex:7];

    const uint32_t pairs = static_cast<uint32_t>((q_elems + k_elems) / 2);
    const uint32_t tsize = 256;
    [enc dispatchThreads:MTLSizeMake(pairs, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(tsize, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "RoPE: command buffer error");
    }

    std::memcpy(q.data(), qbuf.contents, static_cast<size_t>(q_elems) * sizeof(float));
    std::memcpy(k.data(), kbuf.contents, static_cast<size_t>(k_elems) * sizeof(float));
    return {};
}

// Attention

Status MetalBackend::Attention(TensorView out, TensorView q, const KVCacheView& kv,
                               const AttentionConfig& config) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "Attention"); !s.ok()) return s;
    if (auto s = check_contig_valid(q, "Attention"); !s.ok()) return s;

    if (q.shape().rank() != 3 || q.shape().dim(0) != 1) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "Attention: q must be [1, num_heads, head_dim]");
    }
    const int32_t num_heads = config.num_heads;
    const int32_t num_kv_heads = config.num_kv_heads;
    const int32_t head_dim = config.head_dim;
    const int32_t seq_len = kv.seq_len;
    const float scale = config.scale > 0.0f
        ? config.scale
        : 1.0f / std::sqrt(static_cast<float>(head_dim));

    if (kv.keys == nullptr || kv.values == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "Attention: null KV cache");
    }
    if (kv.num_kv_heads != num_kv_heads || kv.head_dim != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "Attention: KV cache shape mismatch");
    }
    if (q.shape().dim(1) != num_heads || q.shape().dim(2) != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "Attention: q head config mismatch");
    }
    if (out.shape().dim(0) != 1 || out.shape().dim(1) != num_heads * head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "Attention: output shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "Attention: output must be f32");
    }

    const int64_t kv_elems = static_cast<int64_t>(seq_len) * num_kv_heads * head_dim;

    // Upload q/keys/values as f32 (KV cache may be f16 in other callers).
    id<MTLBuffer> qbuf = upload(*impl_, to_f32(q));
    std::vector<float> keys_host(static_cast<size_t>(kv_elems));
    std::vector<float> values_host(static_cast<size_t>(kv_elems));
    for (int64_t i = 0; i < kv_elems; ++i) {
        keys_host[static_cast<size_t>(i)] = elem_to_f32(kv.keys, kv.dtype, i);
        values_host[static_cast<size_t>(i)] = elem_to_f32(kv.values, kv.dtype, i);
    }
    id<MTLBuffer> kbuf = upload(*impl_, keys_host);
    id<MTLBuffer> vbuf = upload(*impl_, values_host);
    id<MTLBuffer> obuf = [impl_->device
        newBufferWithLength:static_cast<size_t>(num_heads) * static_cast<size_t>(head_dim) * sizeof(float)
                    options:MTLResourceStorageModeShared];
    if (!qbuf || !kbuf || !vbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "Attention: buffer alloc failed");
    }

    const uint32_t nh = static_cast<uint32_t>(num_heads);
    const uint32_t nkh = static_cast<uint32_t>(num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(head_dim);
    const uint32_t sl = static_cast<uint32_t>(seq_len);
    const float sc = scale;

    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:impl_->attention_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:qbuf offset:0 atIndex:1];
    [enc setBuffer:kbuf offset:0 atIndex:2];
    [enc setBuffer:vbuf offset:0 atIndex:3];
    [enc setBytes:&nh length:sizeof(nh) atIndex:4];
    [enc setBytes:&nkh length:sizeof(nkh) atIndex:5];
    [enc setBytes:&hd length:sizeof(hd) atIndex:6];
    [enc setBytes:&sl length:sizeof(sl) atIndex:7];
    [enc setBytes:&sc length:sizeof(sc) atIndex:8];

    const uint32_t grid = static_cast<uint32_t>(num_heads) * static_cast<uint32_t>(head_dim);
    const uint32_t tsize = 256;
    [enc dispatchThreads:MTLSizeMake(grid, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(tsize, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "Attention: command buffer error");
    }

    std::memcpy(out.data(), obuf.contents,
                static_cast<size_t>(num_heads) * static_cast<size_t>(head_dim) * sizeof(float));
    return {};
}

// SwiGLU

Status MetalBackend::SwiGLU(TensorView out, TensorView gate, TensorView up) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "SwiGLU"); !s.ok()) return s;
    if (auto s = check_contig_valid(gate, "SwiGLU"); !s.ok()) return s;
    if (auto s = check_contig_valid(up, "SwiGLU"); !s.ok()) return s;

    if (out.shape() != gate.shape() || out.shape() != up.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument, "SwiGLU: shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "SwiGLU: output must be f32");
    }

    const int64_t n = out.shape().numel();
    id<MTLBuffer> gbuf = upload(*impl_, to_f32(gate));
    id<MTLBuffer> ubuf = upload(*impl_, to_f32(up));
    id<MTLBuffer> obuf = [impl_->device
        newBufferWithLength:static_cast<size_t>(n) * sizeof(float)
                    options:MTLResourceStorageModeShared];
    if (!gbuf || !ubuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "SwiGLU: buffer alloc failed");
    }

    const uint32_t ne = static_cast<uint32_t>(n);
    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:impl_->swiglu_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:gbuf offset:0 atIndex:1];
    [enc setBuffer:ubuf offset:0 atIndex:2];
    [enc setBytes:&ne length:sizeof(ne) atIndex:3];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(n), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "SwiGLU: command buffer error");
    }

    std::memcpy(out.data(), obuf.contents, static_cast<size_t>(n) * sizeof(float));
    return {};
}

// AddInPlace

Status MetalBackend::AddInPlace(TensorView x, TensorView residual) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(x, "AddInPlace"); !s.ok()) return s;
    if (auto s = check_contig_valid(residual, "AddInPlace"); !s.ok()) return s;

    if (x.shape() != residual.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument, "AddInPlace: shape mismatch");
    }
    if (x.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "AddInPlace: x must be f32");
    }

    const int64_t n = x.shape().numel();
    id<MTLBuffer> xbuf = upload(*impl_, to_f32(x));
    id<MTLBuffer> rbuf = upload(*impl_, to_f32(residual));
    if (!xbuf || !rbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "AddInPlace: buffer alloc failed");
    }

    const uint32_t ne = static_cast<uint32_t>(n);
    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:impl_->add_ps];
    [enc setBuffer:xbuf offset:0 atIndex:0];
    [enc setBuffer:rbuf offset:0 atIndex:1];
    [enc setBytes:&ne length:sizeof(ne) atIndex:2];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(n), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "AddInPlace: command buffer error");
    }

    std::memcpy(x.data(), xbuf.contents, static_cast<size_t>(n) * sizeof(float));
    return {};
}

// AddBiasInPlace

Status MetalBackend::AddBiasInPlace(TensorView x, TensorView bias) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(x, "AddBiasInPlace"); !s.ok()) return s;
    if (auto s = check_contig_valid(bias, "AddBiasInPlace"); !s.ok()) return s;

    if (x.shape().rank() != 2 || bias.shape().numel() != x.shape().dim(1)) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AddBiasInPlace: expected x[batch, n] + bias[n]");
    }
    if (x.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "AddBiasInPlace: x must be f32");
    }

    const int64_t total = x.shape().numel();
    id<MTLBuffer> xbuf = upload(*impl_, to_f32(x));
    id<MTLBuffer> bbuf = upload(*impl_, to_f32(bias));
    if (!xbuf || !bbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "AddBiasInPlace: buffer alloc failed");
    }

    const uint32_t n = static_cast<uint32_t>(x.shape().dim(1));
    id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:impl_->add_bias_ps];
    [enc setBuffer:xbuf offset:0 atIndex:0];
    [enc setBuffer:bbuf offset:0 atIndex:1];
    [enc setBytes:&n length:sizeof(n) atIndex:2];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(total), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error != nil) {
        return Status::Error(ErrorCode::kBackendFailure, "AddBiasInPlace: command buffer error");
    }

    std::memcpy(x.data(), xbuf.contents, static_cast<size_t>(total) * sizeof(float));
    return {};
}

// Synchronize

Status MetalBackend::Synchronize() { return {}; }

} // namespace pl::mllm
