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

// Metal backend implementation (Objective-C++).
//
// Architecture:
//   * ShadowBuffer cache: host pointer -> MTLBuffer mapping so that the
//     same activation (e.g. hidden state) is uploaded once and reused across
//     ops within a forward pass.  NotifyHostWrite invalidates entries.
//   * DeferredCmd: a single command buffer stays open across ops; only
//     flush() on SyncToHost/Synchronize actually commits and waits.
//   * DeviceKV: K/V MTLBuffers allocated at ConfigureDeviceKV time;
//     AppendKV copies one token's K/V into the device buffer via a blit or
//     compute shader; AttentionKV runs the attention kernel directly on
//     the device buffer (no host round-trip).
//   * Output persistence: output MTLBuffers are registered in the shadow
//     cache so the next op finds them already on-device.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include <algorithm>
#include <array>
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

struct Q4Block {
    uint16_t scale; // fp16
    uint8_t qs[16];
};
static_assert(sizeof(Q4Block) == kQ4_0TypeSize);

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
    case DType::kQ4_0: {
        const auto* blocks = static_cast<const Q4Block*>(base);
        const int64_t block_idx = idx / kQ4_0BlockSize;
        const int64_t in_block = idx % kQ4_0BlockSize;
        const float scale = fp16_to_fp32(blocks[block_idx].scale);
        const uint8_t packed = blocks[block_idx].qs[in_block % 16];
        const int32_t nibble = (in_block < 16) ? (packed & 0x0F) : (packed >> 4);
        return scale * static_cast<float>(nibble - 8);
    }
    }
    return 0.0f;
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

// =========================================================================
// Impl — all Metal/Objective-C state lives here.
// =========================================================================

struct MetalBackend::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;

    // Pipeline states for compute kernels.
    id<MTLComputePipelineState> rmsnorm_ps = nil;
    id<MTLComputePipelineState> add_rmsnorm_ps = nil; // fused residual-add + rmsnorm
    id<MTLComputePipelineState> rope_qknorm_ps = nil; // fused per-head qk-norm + rope (decode)
    id<MTLComputePipelineState> rope_ps = nil;
    id<MTLComputePipelineState> attention_flash_ps = nil; // unified flash-attention (host + device KV)
    id<MTLComputePipelineState> attention_decode_ps = nil; // simdgroup decode flash (single row)
    id<MTLComputePipelineState> attention_flash_batch_ps = nil; // batched prefill attention
    id<MTLComputePipelineState> swiglu_ps = nil;
    id<MTLComputePipelineState> add_ps = nil;
    id<MTLComputePipelineState> add_bias_ps = nil;
    id<MTLComputePipelineState> gemv_q8_0_ps = nil;
    id<MTLComputePipelineState> gemv_q4_0_ps = nil;
    id<MTLComputePipelineState> gemv_f16_ps = nil;
    id<MTLComputePipelineState> gemv_f32_ps = nil;
    id<MTLComputePipelineState> gemv_q8_0_fused_ps = nil;
    id<MTLComputePipelineState> gemv_q4_0_fused_ps = nil;
    id<MTLComputePipelineState> gemv_f16_fused_ps = nil;
    id<MTLComputePipelineState> gemv_f32_fused_ps = nil;
    id<MTLComputePipelineState> append_kv_ps = nil;
    id<MTLComputePipelineState> dequant_q8_0_f16_ps = nil; // q8_0 -> f16 for MPS prefill
    id<MTLComputePipelineState> dequant_q4_0_f16_ps = nil; // q4_0 -> f16 for MPS prefill
    id<MTLComputePipelineState> cvt_f32_to_f16_ps = nil;   // GEMM A conversion
    id<MTLComputePipelineState> cvt_f16_to_f32_ps = nil;   // GEMM C conversion

    // Weight table. f16 weights are kept as f16 (half bandwidth); Q8_0 kept
    // raw; f32 kept as-is. `f16_buf` is a lazily materialized f16 copy used
    // by the batched-prefill MPS path (dequantized once on first use).
    struct Weight {
        id<MTLBuffer> buf = nil;
        Shape shape;
        DType dtype = DType::kF32;
        id<MTLBuffer> f16_buf = nil; // cached f16 version for MPS GEMMs
    };
    // Heterogeneous string_view lookup (no per-op std::string temp).
    StringKeyMap<Weight> weights_;

    // --- Shadow buffer cache ------------------------------------------------
    // Maps host data pointer -> device buffer so that repeated ops on the same
    // activation avoid re-uploading.  Entries are invalidated by NotifyHostWrite.
    //
    // Key = static_cast<uintptr_t>(host_ptr)
    struct ShadowEntry {
        id<MTLBuffer> buf = nil;
        size_t byte_size = 0;
        bool is_output = false; // true if this buffer was created as an output
                                // (not a host-upload), so we don't memcpy back
                                // unless SyncToHost is called.
    };
    std::unordered_map<uintptr_t, ShadowEntry> shadow_;

    // --- Deferred command buffer -------------------------------------------
    // A single command buffer is kept open; ops encode into it without
    // committing.  flush() commits + waits.  This eliminates per-op
    // round-trip latency (~5-10 µs per commit+wait on Apple Silicon).
    id<MTLCommandBuffer> deferred_cb = nil;
    id<MTLComputeCommandEncoder> deferred_enc = nil;

    // --- f16 MPS prefill scratch -------------------------------------------
    // Grow-on-demand buffers for the GEMM input (A) / output (C) half-
    // precision staging.  Sized with the MPS row pitch.
    id<MTLBuffer> mps_scratch_a_ = nil;
    size_t mps_scratch_a_bytes_ = 0;
    id<MTLBuffer> mps_scratch_c_ = nil;
    size_t mps_scratch_c_bytes_ = 0;

    // Returns `old` when it is big enough, otherwise allocates a fresh
    // buffer and updates `cap`.
    id<MTLBuffer> ensure_scratch(size_t bytes, id<MTLBuffer> old, size_t& cap) {
        if (old != nil && cap >= bytes) {
            return old;
        }
        id<MTLBuffer> buf = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        cap = (buf != nil) ? bytes : 0;
        return buf;
    }

    // --- Device KV cache ----------------------------------------------------
    struct DeviceKV {
        id<MTLBuffer> keys = nil;   // [num_layers, capacity, num_kv_heads, head_dim] f32
        id<MTLBuffer> values = nil; // same layout
        int32_t num_layers = 0;
        int32_t num_kv_heads = 0;
        int32_t head_dim = 0;
        int32_t capacity = 0;
    } device_kv_;

    // Constructor errors surface on the first op call.
    Status init_error;

    // Sticky GPU error: once ANY committed command buffer finishes with an
    // error the backend is unusable and every subsequent op/sync surfaces the
    // same error (fail loud). Previously flush() cleared deferred_cb BEFORE
    // checking its status, so command-buffer errors were permanently lost.
    Status gpu_error_ = {};

    // Record a command buffer error as the sticky GPU error and return it.
    Status note_command_buffer_error(id<MTLCommandBuffer> cb) {
        NSString* desc = (cb.error != nil) ? cb.error.localizedDescription : @"status=error";
        gpu_error_ = Status::Error(
            ErrorCode::kBackendFailure,
            "Metal command buffer error: " + std::string(desc.UTF8String));
        return gpu_error_;
    }

    explicit Impl() { init(); }

    ~Impl() {
        (void)flush();
    }

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
        add_rmsnorm_ps = make_ps("mllm_add_rmsnorm");
        rope_qknorm_ps = make_ps("mllm_rope_qknorm");
        rope_ps = make_ps("mllm_rope");
        attention_flash_ps = make_ps("mllm_attention_flash");
        attention_decode_ps = make_ps("mllm_attention_decode");
        attention_flash_batch_ps = make_ps("mllm_attention_flash_batch");
        swiglu_ps = make_ps("mllm_swiglu");
        add_ps = make_ps("mllm_add_inplace");
        add_bias_ps = make_ps("mllm_add_bias");
        gemv_q8_0_ps = make_ps("mllm_gemv_q8_0");
        gemv_q4_0_ps = make_ps("mllm_gemv_q4_0");
        gemv_f16_ps = make_ps("mllm_gemv_f16");
        gemv_f32_ps = make_ps("mllm_gemv_f32");
        gemv_q8_0_fused_ps = make_ps("mllm_gemv_q8_0_fused");
        gemv_q4_0_fused_ps = make_ps("mllm_gemv_q4_0_fused");
        gemv_f16_fused_ps = make_ps("mllm_gemv_f16_fused");
        gemv_f32_fused_ps = make_ps("mllm_gemv_f32_fused");
        append_kv_ps = make_ps("mllm_append_kv");
        dequant_q8_0_f16_ps = make_ps("mllm_dequant_q8_0_f16");
        dequant_q4_0_f16_ps = make_ps("mllm_dequant_q4_0_f16");
        cvt_f32_to_f16_ps = make_ps("mllm_cvt_f32_to_f16");
        cvt_f16_to_f32_ps = make_ps("mllm_cvt_f16_to_f32");
        if (!init_error.ok()) {
            return;
        }
        if (!rmsnorm_ps || !add_rmsnorm_ps || !rope_qknorm_ps || !rope_ps ||
            !attention_flash_ps ||
            !attention_decode_ps ||
            !attention_flash_batch_ps ||
            !swiglu_ps || !add_ps || !add_bias_ps ||
            !gemv_q8_0_ps || !gemv_q4_0_ps || !gemv_f16_ps || !gemv_f32_ps ||
            !gemv_q8_0_fused_ps || !gemv_q4_0_fused_ps || !gemv_f16_fused_ps ||
            !gemv_f32_fused_ps ||
            !append_kv_ps || !dequant_q8_0_f16_ps || !dequant_q4_0_f16_ps ||
            !cvt_f32_to_f16_ps ||
            !cvt_f16_to_f32_ps) {
            init_error = Status::Error(ErrorCode::kBackendFailure,
                                       "MetalBackend: missing pipeline state");
        }
    }

public:
    // --- Deferred command buffer management ---

    // Returns the active compute encoder, creating a command buffer if needed.
    id<MTLComputeCommandEncoder> encoder() {
        if (!deferred_cb) {
            deferred_cb = [queue commandBuffer];
        }
        if (!deferred_enc) {
            deferred_enc = [deferred_cb computeCommandEncoder];
        }
        return deferred_enc;
    }

    // Commit pending work and wait for completion.  A GPU-side error becomes
    // sticky (gpu_error_) and is returned to the caller.
    Status flush() {
        if (deferred_enc) {
            [deferred_enc endEncoding];
            deferred_enc = nil;
        }
        if (deferred_cb) {
            id<MTLCommandBuffer> cb = deferred_cb;
            deferred_cb = nil;
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error != nil) {
                return note_command_buffer_error(cb);
            }
        }
        return gpu_error_;
    }

    // --- Shadow buffer cache ---

    // Get or create a device buffer for the given host pointer.
    // If the host data is already cached (same pointer + same size), returns
    // the existing buffer. Otherwise uploads the data and caches it.
    id<MTLBuffer> get_or_upload(const void* host_ptr, size_t byte_size) {
        auto key = reinterpret_cast<uintptr_t>(host_ptr);
        auto it = shadow_.find(key);
        if (it != shadow_.end() && it->second.byte_size == byte_size) {
            // Buffer is current — but only if it wasn't created as an output.
            // Output buffers hold GPU-computed data; if the host wrote to the
            // same pointer, NotifyHostWrite would have invalidated the entry.
            return it->second.buf;
        }
        // Upload.
        id<MTLBuffer> buf = [device
            newBufferWithBytes:host_ptr
                        length:byte_size
                       options:MTLResourceStorageModeShared];
        if (buf) {
            shadow_[key] = {buf, byte_size, false};
        }
        return buf;
    }

    // Create or get a device buffer for output.  If an output buffer already
    // exists for this host pointer (from a previous op), reuse it.
    id<MTLBuffer> get_or_alloc_output(void* host_ptr, size_t byte_size) {
        auto key = reinterpret_cast<uintptr_t>(host_ptr);
        auto it = shadow_.find(key);
        if (it != shadow_.end() && it->second.byte_size == byte_size) {
            return it->second.buf;
        }
        id<MTLBuffer> buf = [device
            newBufferWithLength:byte_size
                        options:MTLResourceStorageModeShared];
        if (buf) {
            shadow_[key] = {buf, byte_size, true};
        }
        return buf;
    }

    // Invalidate shadow entry for host_ptr (NotifyHostWrite).
    void invalidate(const void* host_ptr) {
        shadow_.erase(reinterpret_cast<uintptr_t>(host_ptr));
    }

    // Copy device buffer contents back to host (SyncToHost).
    void download(void* host_ptr, size_t byte_size) {
        auto key = reinterpret_cast<uintptr_t>(host_ptr);
        auto it = shadow_.find(key);
        if (it != shadow_.end() && it->second.buf) {
            std::memcpy(host_ptr, it->second.buf.contents, byte_size);
        }
        // If not in shadow, the data is already in host memory (no-op).
    }
};

// =========================================================================
// Construction / destruction
// =========================================================================

MetalBackend::MetalBackend() : impl_(new Impl()) {}
MetalBackend::~MetalBackend() = default;

namespace {

Status ensure_ready(MetalBackend::Impl& impl) {
    if (!impl.init_error.ok()) {
        return impl.init_error;
    }
    // After a sticky GPU error every op fails fast instead of encoding new
    // work on top of corrupted state.
    return impl.gpu_error_;
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

// Upload a tensor to a device buffer, handling dtype conversion for f16/Q8_0
// inputs that need f32 on the device.  Uses the shadow cache.
id<MTLBuffer> upload_tensor(MetalBackend::Impl& impl, const TensorView& t) {
    // For f32 tensors, we can directly share the host memory.
    if (t.dtype() == DType::kF32) {
        return impl.get_or_upload(t.data(), t.byte_size());
    }
    // For f16/Q8_0, convert to f32 and upload.  The converted data is
    // temporary; we cache by the original host pointer but store the
    // converted f32 buffer.
    // TODO: in the kernels phase, f16 will be kept native.
    auto key = reinterpret_cast<uintptr_t>(t.data());
    auto it = impl.shadow_.find(key);
    if (it != impl.shadow_.end() && it->second.byte_size == t.shape().numel() * sizeof(float)) {
        return it->second.buf;
    }
    const std::vector<float> host = to_f32(t);
    id<MTLBuffer> buf = [impl.device
        newBufferWithBytes:host.data()
                    length:host.size() * sizeof(float)
                   options:MTLResourceStorageModeShared];
    if (buf) {
        impl.shadow_[key] = {buf, host.size() * sizeof(float), false};
    }
    return buf;
}

} // namespace

// =========================================================================
// ImportWeights — f16 kept as-is, Q8_0 kept raw, f32 kept as-is
// =========================================================================

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
        // Keep weights in their native format: f32 as f32, f16 as f16, Q8_0 raw.
        // The GEMV kernels handle each type natively.
        id<MTLBuffer> buf = [impl_->device
            newBufferWithBytes:w.data()
                        length:w.byte_size()
                       options:MTLResourceStorageModeShared];
        if (!buf) {
            return Status::Error(ErrorCode::kBackendFailure,
                                 "MetalBackend: buffer alloc failed");
        }
        wg.buf = buf;
        wg.dtype = w.dtype();
        impl_->weights_[std::string(names[i])] = wg;
    }
    return {};
}

// =========================================================================
// MatMul — GEMV (decode) and MPS (prefill batch > 1)
// =========================================================================

Status MetalBackend::MatMul(TensorView out, TensorView x,
std::string_view weight_name) {
if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "MatMul"); !s.ok()) return s;
    if (auto s = check_contig_valid(x, "MatMul"); !s.ok()) return s;

    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t in_dim = static_cast<int32_t>(x.shape().dim(1));

    auto it = impl_->weights_.find(weight_name);
    if (it == impl_->weights_.end()) {
        return Status::Error(ErrorCode::kNotFound,
                             "MatMul: weight '" + std::string(weight_name) + "' not found");
    }
    Impl::Weight& w = it->second; // mutable: lazily materializes f32 cache
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

    // Get or upload input x (shadow cached).
    id<MTLBuffer> xbuf = upload_tensor(*impl_, x);
    if (!xbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "MatMul: x upload failed");
    }

    // Get or allocate output buffer (shadow cached, persisted).
    const size_t out_bytes = static_cast<size_t>(batch) * static_cast<size_t>(out_dim) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "MatMul: output alloc failed");
    }

    // --- GEMV path (batch == 1, decode) ---
    if (batch == 1) {
        id<MTLComputePipelineState> ps = nil;
        switch (w.dtype) {
        case DType::kQ8_0:
            if (in_dim % kQ8_0BlockSize != 0) {
                return Status::Error(ErrorCode::kInvalidArgument,
                                     "MatMul: in_dim not Q8_0 block-aligned");
            }
            ps = impl_->gemv_q8_0_ps;
            break;
        case DType::kQ4_0:
            if (in_dim % kQ4_0BlockSize != 0) {
                return Status::Error(ErrorCode::kInvalidArgument,
                                     "MatMul: in_dim not Q4_0 block-aligned");
            }
            ps = impl_->gemv_q4_0_ps;
            break;
        case DType::kF16:
            ps = impl_->gemv_f16_ps;
            break;
        case DType::kF32:
            ps = impl_->gemv_f32_ps;
            break;
        default:
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: unsupported weight dtype for GEMV");
        }

        const uint32_t in_d = static_cast<uint32_t>(in_dim);
        const uint32_t out_d = static_cast<uint32_t>(out_dim);

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:xbuf offset:0 atIndex:1];
    [enc setBuffer:w.buf offset:0 atIndex:2];
    [enc setBytes:&in_d length:sizeof(in_d) atIndex:3];
    [enc setBytes:&out_d length:sizeof(out_d) atIndex:4];
    // Quants use one simdgroup (32 lanes) per output row; F16/F32 keep
    // split-K with NT=4 lanes/row and threadgroup-memory reduction.
    const bool simd_per_row = is_quantized(w.dtype);
    [enc setThreadgroupMemoryLength:256 * sizeof(float) atIndex:0];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(out_dim) * (simd_per_row ? 32 : 4),
                                     1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        // Deferred — no commit/wait here.
        return {};
    }

    // --- MPS path (batch > 1, prefill) ---
    // Per-dtype handling:
    //   f16  -> weights bound directly, f16 MPS GEMM (zero copy)
    //   q8_0 -> cached f16 dequant, then f16 MPS GEMM (halves both the
    //           per-weight cache memory and the MPS GEMM read bandwidth vs
    //           the previous dequant-to-f32; prefill is bandwidth bound)
    //   f32  -> weights bound directly, f32 MPS GEMM (unchanged)
    // For the f16 modes the f32 activations are converted in/out by tiny
    // elementwise kernels (batch x dim elements — negligible vs the GEMM).
    const bool f16_mode = (w.dtype != DType::kF32);

    MPSDataType mps_dt = MPSDataTypeFloat32;
    size_t a_row = align16(static_cast<size_t>(in_dim) * sizeof(float));
    size_t b_row = a_row;
    size_t c_row = align16(static_cast<size_t>(out_dim) * sizeof(float));
    id<MTLBuffer> a_mps = xbuf;
    id<MTLBuffer> c_mps = obuf;
    id<MTLBuffer> b_mps = w.buf;

    if (f16_mode) {
        if ((in_dim % 8) != 0 || (out_dim % 8) != 0) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: prefill requires dims to be 8-element aligned");
        }
        if (is_quantized(w.dtype)) {
            const int64_t block_size = dtype_block_size(w.dtype);
            if (in_dim % block_size != 0) {
                return Status::Error(ErrorCode::kInvalidArgument,
                                     "MatMul: in_dim not quant block-aligned");
            }
            if (!w.f16_buf) {
                const size_t numel = static_cast<size_t>(out_dim) * static_cast<size_t>(in_dim);
                w.f16_buf = [impl_->device
                    newBufferWithLength:numel * sizeof(uint16_t)
                                options:MTLResourceStorageModeShared];
                if (!w.f16_buf) {
                    return Status::Error(ErrorCode::kBackendFailure,
                                         "MatMul: f16 weight cache alloc failed");
                }
                id<MTLComputeCommandEncoder> enc = impl_->encoder();
                [enc setComputePipelineState:(w.dtype == DType::kQ8_0)
                                                 ? impl_->dequant_q8_0_f16_ps
                                                 : impl_->dequant_q4_0_f16_ps];
                [enc setBuffer:w.buf offset:0 atIndex:0];
                [enc setBuffer:w.f16_buf offset:0 atIndex:1];
                const NSUInteger num_blocks =
                    static_cast<NSUInteger>(numel / static_cast<size_t>(block_size));
                [enc dispatchThreads:MTLSizeMake(num_blocks, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            }
            b_mps = w.f16_buf;
        } else if (w.dtype != DType::kF16) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: unsupported weight dtype for batched prefill");
        }
        mps_dt = MPSDataTypeFloat16;
        a_row = align16(static_cast<size_t>(in_dim) * sizeof(uint16_t));
        b_row = static_cast<size_t>(in_dim) * sizeof(uint16_t);
        c_row = align16(static_cast<size_t>(out_dim) * sizeof(uint16_t));

        // Stage A (x, f32) into the f16 scratch at MPS row pitch; C (f16)
        // likewise — the result is converted back to f32 after the GEMM.
        impl_->mps_scratch_a_ = impl_->ensure_scratch(static_cast<size_t>(batch) * a_row,
                                                      impl_->mps_scratch_a_,
                                                      impl_->mps_scratch_a_bytes_);
        impl_->mps_scratch_c_ = impl_->ensure_scratch(static_cast<size_t>(batch) * c_row,
                                                      impl_->mps_scratch_c_,
                                                      impl_->mps_scratch_c_bytes_);
        a_mps = impl_->mps_scratch_a_;
        c_mps = impl_->mps_scratch_c_;
        if (a_mps == nil || c_mps == nil) {
            return Status::Error(ErrorCode::kBackendFailure, "MatMul: MPS scratch alloc failed");
        }

        id<MTLComputeCommandEncoder> enc = impl_->encoder();
        [enc setComputePipelineState:impl_->cvt_f32_to_f16_ps];
        [enc setBuffer:xbuf offset:0 atIndex:0];
        [enc setBuffer:a_mps offset:0 atIndex:1];
        uint32_t in_d = static_cast<uint32_t>(in_dim);
        uint32_t pitch = static_cast<uint32_t>(a_row / sizeof(uint16_t));
        [enc setBytes:&in_d length:sizeof(in_d) atIndex:2];
        [enc setBytes:&pitch length:sizeof(pitch) atIndex:3];
        const NSUInteger total = static_cast<NSUInteger>(batch) * static_cast<NSUInteger>(in_dim);
        [enc dispatchThreads:MTLSizeMake(total, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }

    MPSMatrixDescriptor* a_desc = [MPSMatrixDescriptor
        matrixDescriptorWithRows:static_cast<NSUInteger>(batch)
                         columns:static_cast<NSUInteger>(in_dim)
                        rowBytes:a_row
                        dataType:mps_dt];
    MPSMatrixDescriptor* b_desc = [MPSMatrixDescriptor
        matrixDescriptorWithRows:static_cast<NSUInteger>(out_dim)
                         columns:static_cast<NSUInteger>(in_dim)
                        rowBytes:b_row
                        dataType:mps_dt];
    MPSMatrixDescriptor* c_desc = [MPSMatrixDescriptor
        matrixDescriptorWithRows:static_cast<NSUInteger>(batch)
                         columns:static_cast<NSUInteger>(out_dim)
                        rowBytes:c_row
                        dataType:mps_dt];

    MPSMatrix* mA = [[MPSMatrix alloc] initWithBuffer:a_mps descriptor:a_desc];
    MPSMatrix* mB = [[MPSMatrix alloc] initWithBuffer:b_mps descriptor:b_desc];
    MPSMatrix* mC = [[MPSMatrix alloc] initWithBuffer:c_mps descriptor:c_desc];

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

    // Encode the MPS GEMM into the SAME deferred command buffer the compute
    // kernels use (closing the open compute encoder first).  Execution order
    // inside one command buffer is preserved, so the GEMM sees the dequant
    // results and its output is visible to the next kernel — but nothing is
    // committed or waited on here.  The CPU only blocks once per prefill
    // chunk at SyncToHost/Synchronize instead of once per matmul (that was
    // ~0.5-1 ms x 7 GEMMs x num_layers x chunks of pure sync overhead).
    if (impl_->deferred_enc != nil) {
        [impl_->deferred_enc endEncoding];
        impl_->deferred_enc = nil;
    }
    if (impl_->deferred_cb == nil) {
        impl_->deferred_cb = [impl_->queue commandBuffer];
    }
    [mul encodeToCommandBuffer:impl_->deferred_cb leftMatrix:mA rightMatrix:mB resultMatrix:mC];

    if (f16_mode) {
        // Convert the f16 GEMM result back to dense f32 in the shadow
        // output buffer (fresh compute encoder on the same command buffer).
        id<MTLComputeCommandEncoder> enc = impl_->encoder();
        [enc setComputePipelineState:impl_->cvt_f16_to_f32_ps];
        [enc setBuffer:c_mps offset:0 atIndex:0];
        [enc setBuffer:obuf offset:0 atIndex:1];
        uint32_t out_d = static_cast<uint32_t>(out_dim);
        uint32_t pitch = static_cast<uint32_t>(c_row / sizeof(uint16_t));
        [enc setBytes:&out_d length:sizeof(out_d) atIndex:2];
        [enc setBytes:&pitch length:sizeof(pitch) atIndex:3];
        const NSUInteger total = static_cast<NSUInteger>(batch) * static_cast<NSUInteger>(out_dim);
        [enc dispatchThreads:MTLSizeMake(total, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }

    // Mark the output buffer as dirty in shadow so the next op picks it up.
    // The data is on the device; SyncToHost will download it.
    impl_->shadow_[reinterpret_cast<uintptr_t>(out.data())] =
        {obuf, out_bytes, true};

    return {};
}

// =========================================================================
// MatMulFused — multiple GEMVs sharing same input, single dispatch
// =========================================================================

Status MetalBackend::MatMulFused(std::span<TensorView> outs,
TensorView x,
std::span<const std::string_view> weight_names) {
if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (outs.size() != weight_names.size()) {
        return Status::Error(ErrorCode::kInvalidArgument, "MatMulFused: size mismatch");
    }
    if (outs.empty() || outs.size() > 3) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "MatMulFused: supports 1-3 outputs, got " + std::to_string(outs.size()));
    }
    if (auto s = check_contig_valid(x, "MatMulFused"); !s.ok()) return s;

    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t in_dim = static_cast<int32_t>(x.shape().dim(1));
    if (batch != 1) {
        // Fallback for prefill (batch > 1) — use individual MatMul calls.
        for (size_t i = 0; i < outs.size(); ++i) {
            if (auto s = MatMul(outs[i], x, weight_names[i]); !s.ok()) return s;
        }
        return {};
    }

    // Look up weights and validate.
    const size_t n = outs.size();
    std::array<const Impl::Weight*, 3> ws{};
    std::array<int32_t, 3> out_dims{};
    DType wtype = DType::kF32;
    for (size_t i = 0; i < n; ++i) {
        if (auto s = check_contig_valid(outs[i], "MatMulFused"); !s.ok()) return s;
        auto it = impl_->weights_.find(weight_names[i]);
        if (it == impl_->weights_.end()) {
            return Status::Error(ErrorCode::kNotFound,
                                 "MatMulFused: weight '" + std::string(weight_names[i]) + "' not found");
        }
        ws[i] = &it->second;
        out_dims[i] = static_cast<int32_t>(it->second.shape.dim(0));
        if (it->second.shape.dim(1) != in_dim) {
            return Status::Error(ErrorCode::kInvalidArgument, "MatMulFused: in_dim mismatch");
        }
        if (outs[i].shape().dim(0) != 1 || outs[i].shape().dim(1) != out_dims[i]) {
            return Status::Error(ErrorCode::kInvalidArgument, "MatMulFused: output shape mismatch");
        }
        if (outs[i].dtype() != DType::kF32) {
            return Status::Error(ErrorCode::kUnsupported, "MatMulFused: output must be f32");
        }
        if (i == 0) {
            wtype = it->second.dtype;
        } else if (it->second.dtype != wtype) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMulFused: all weights must have same dtype");
        }
    }

    // Select pipeline.
    id<MTLComputePipelineState> ps = nil;
    switch (wtype) {
    case DType::kQ8_0:
    case DType::kQ4_0:
        if (in_dim % dtype_block_size(wtype) != 0) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMulFused: in_dim not quant block-aligned");
        }
        ps = (wtype == DType::kQ8_0) ? impl_->gemv_q8_0_fused_ps : impl_->gemv_q4_0_fused_ps;
        break;
    case DType::kF16:
        ps = impl_->gemv_f16_fused_ps;
        break;
    case DType::kF32:
        ps = impl_->gemv_f32_fused_ps;
        break;
    default:
        return Status::Error(ErrorCode::kUnsupported,
                             "MatMulFused: unsupported weight dtype");
    }

    // Get or upload input x.
    id<MTLBuffer> xbuf = upload_tensor(*impl_, x);
    if (!xbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "MatMulFused: x upload failed");
    }

    // Get or allocate output buffers.
    std::array<id<MTLBuffer>, 3> obufs{};
    for (size_t i = 0; i < n; ++i) {
        const size_t out_bytes = static_cast<size_t>(out_dims[i]) * sizeof(float);
        obufs[i] = impl_->get_or_alloc_output(outs[i].data(), out_bytes);
        if (!obufs[i]) {
            return Status::Error(ErrorCode::kBackendFailure, "MatMulFused: output alloc failed");
        }
    }

    // Build params: [out0, out1, out2, off0, off1, off2, in_dim, n]
    uint32_t params[8] = {0};
    params[0] = static_cast<uint32_t>(out_dims[0]);
    params[1] = (n >= 2) ? static_cast<uint32_t>(out_dims[1]) : 0;
    params[2] = (n >= 3) ? static_cast<uint32_t>(out_dims[2]) : 0;
    params[3] = params[0];                          // off0 = out0
    params[4] = params[3] + params[1];              // off1 = out0 + out1
    params[5] = params[4] + params[2];              // off2 = out0 + out1 + out2
    params[6] = static_cast<uint32_t>(in_dim);
    params[7] = static_cast<uint32_t>(n);

    const uint32_t total_rows = params[3 + (n - 1)];  // last offset = total rows

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:ps];
    [enc setBuffer:obufs[0] offset:0 atIndex:0];
    if (n >= 2) [enc setBuffer:obufs[1] offset:0 atIndex:1];
    if (n >= 3) [enc setBuffer:obufs[2] offset:0 atIndex:2];
    [enc setBuffer:xbuf offset:0 atIndex:3];
    [enc setBuffer:ws[0]->buf offset:0 atIndex:4];
    if (n >= 2) [enc setBuffer:ws[1]->buf offset:0 atIndex:5];
    if (n >= 3) [enc setBuffer:ws[2]->buf offset:0 atIndex:6];
    [enc setBytes:params length:sizeof(params) atIndex:7];
    // Fused quants: one simdgroup per row; F16/F32 keep NT=4 split-K.
    const bool simd_per_row = is_quantized(wtype);
    [enc setThreadgroupMemoryLength:256 * sizeof(float) atIndex:0];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(total_rows) * (simd_per_row ? 32 : 4),
                                     1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    // Deferred.
    return {};
}

// =========================================================================
// RmsNorm
// =========================================================================

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

    id<MTLBuffer> xbuf = upload_tensor(*impl_, x);
    id<MTLBuffer> wbuf = upload_tensor(*impl_, weight);
    const size_t out_bytes = static_cast<size_t>(batch) * static_cast<size_t>(hidden) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!xbuf || !wbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "RmsNorm: buffer alloc failed");
    }

    const uint32_t n = static_cast<uint32_t>(hidden);
    const float e = eps;

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
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
    // Deferred — no commit/wait.
    return {};
}

// =========================================================================
// RmsNormAdd — fused residual-add + RMSNorm
// =========================================================================

Status MetalBackend::RmsNormAdd(TensorView out, TensorView residual, TensorView add,
                                TensorView weight, float eps) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "RmsNormAdd"); !s.ok()) return s;
    if (auto s = check_contig_valid(residual, "RmsNormAdd"); !s.ok()) return s;
    if (auto s = check_contig_valid(add, "RmsNormAdd"); !s.ok()) return s;
    if (auto s = check_contig_valid(weight, "RmsNormAdd"); !s.ok()) return s;

    const int32_t batch = static_cast<int32_t>(residual.shape().dim(0));
    const int32_t hidden = static_cast<int32_t>(residual.shape().dim(1));
    if (out.shape() != residual.shape() || add.shape() != residual.shape() ||
        weight.shape().numel() != hidden) {
        return Status::Error(ErrorCode::kInvalidArgument, "RmsNormAdd: shape mismatch");
    }
    if (!(eps > 0.0f)) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "RmsNormAdd: eps must be positive");
    }
    if (residual.dtype() != DType::kF32 || out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported,
                             "RmsNormAdd: tensors must be f32");
    }

    // residual is in-place AND read-after-write: shadow buffer serves both.
    id<MTLBuffer> rbuf = upload_tensor(*impl_, residual);
    id<MTLBuffer> abuf = upload_tensor(*impl_, add);
    id<MTLBuffer> wbuf = upload_tensor(*impl_, weight);
    const size_t out_bytes = static_cast<size_t>(batch) * static_cast<size_t>(hidden) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!rbuf || !abuf || !wbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure,
                             "RmsNormAdd: buffer alloc failed");
    }
    // Mark residual's device buffer dirty so the next consumer/SyncToHost
    // picks up updated values.
    impl_->shadow_[reinterpret_cast<uintptr_t>(residual.data())] =
        {rbuf, static_cast<size_t>(batch) * static_cast<size_t>(hidden) * sizeof(float),
         true};

    const uint32_t n = static_cast<uint32_t>(hidden);
    const float e = eps;

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->add_rmsnorm_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:rbuf offset:0 atIndex:1];
    [enc setBuffer:abuf offset:0 atIndex:2];
    [enc setBuffer:wbuf offset:0 atIndex:3];
    [enc setBytes:&n length:sizeof(n) atIndex:4];
    [enc setBytes:&e length:sizeof(e) atIndex:5];
    const uint32_t tsize = static_cast<uint32_t>(std::min<int64_t>(
        hidden, static_cast<int64_t>(impl_->device.maxThreadsPerThreadgroup.width)));
    [enc setThreadgroupMemoryLength:tsize * sizeof(float) atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake(static_cast<NSUInteger>(batch), 1, 1)
         threadsPerThreadgroup:MTLSizeMake(tsize, 1, 1)];
    // Deferred — no commit/wait.
    return {};
}

// =========================================================================
// RoPE
// =========================================================================

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

    // RoPE is in-place: we need device buffers that mirror q and k's host
    // memory, and we write the result back into the same device buffer.
    // For in-place ops, the shadow buffer serves double duty: input + output.
    id<MTLBuffer> qbuf = upload_tensor(*impl_, q);
    id<MTLBuffer> kbuf = upload_tensor(*impl_, k);
    if (!qbuf || !kbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "RoPE: buffer alloc failed");
    }

    // After RoPE, the device buffer holds the rotated values.  Update shadow
    // to mark them as outputs so SyncToHost downloads them.
    const size_t q_bytes = q.byte_size();
    const size_t k_bytes = k.byte_size();
    impl_->shadow_[reinterpret_cast<uintptr_t>(q.data())] = {qbuf, q_bytes, true};
    impl_->shadow_[reinterpret_cast<uintptr_t>(k.data())] = {kbuf, k_bytes, true};

    const int64_t q_elems = q.shape().numel();
    const int64_t k_elems = k.shape().numel();
    const uint32_t qe = static_cast<uint32_t>(q_elems);
    const uint32_t qh = static_cast<uint32_t>(num_heads);
    const uint32_t kh = static_cast<uint32_t>(num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(head_dim);
    const float fb = config.freq_base;
    const int32_t pos = static_cast<int32_t>(position);

    id<MTLComputeCommandEncoder> enc = impl_->encoder();

    // Fused path (Qwen3 decode): per-head Q/K RMSNorm + rotation in a
    // single dispatch. Only for batch == 1 (the batch prefill variant
    // keeps the separate norm + rope kernels).
    if (q.shape().dim(0) == 1 && config.q_norm.valid() && config.k_norm.valid()) {
        id<MTLBuffer> qnbuf = upload_tensor(*impl_, config.q_norm);
        id<MTLBuffer> knbuf = upload_tensor(*impl_, config.k_norm);
        if (!qnbuf || !knbuf) {
            return Status::Error(ErrorCode::kBackendFailure,
                                 "RoPE: norm weight upload failed");
        }
        const float qk_eps = config.rms_eps;
        [enc setComputePipelineState:impl_->rope_qknorm_ps];
        [enc setBuffer:qbuf offset:0 atIndex:0];
        [enc setBuffer:kbuf offset:0 atIndex:1];
        [enc setBuffer:qnbuf offset:0 atIndex:2];
        [enc setBuffer:knbuf offset:0 atIndex:3];
        [enc setBytes:&hd length:sizeof(hd) atIndex:4];
        [enc setBytes:&fb length:sizeof(fb) atIndex:5];
        [enc setBytes:&pos length:sizeof(pos) atIndex:6];
        [enc setBytes:&qh length:sizeof(qh) atIndex:7];
        [enc setBytes:&qk_eps length:sizeof(qk_eps) atIndex:8];
        const uint32_t tsize =
            static_cast<uint32_t>(std::min<int64_t>(head_dim, 256));
        [enc setThreadgroupMemoryLength:tsize * sizeof(float) atIndex:0];
        [enc dispatchThreadgroups:MTLSizeMake(
                 static_cast<NSUInteger>(num_heads + num_kv_heads), 1, 1)
             threadsPerThreadgroup:MTLSizeMake(tsize, 1, 1)];
        // Deferred.
        return {};
    }

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
    // Deferred.
    return {};
}

// =========================================================================
// Attention (host KV path — fallback when HasDeviceKV is false)
// =========================================================================

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

    id<MTLBuffer> qbuf = upload_tensor(*impl_, q);

    // Upload KV cache (this is the slow fallback path — device KV is preferred).
    std::vector<float> keys_host(static_cast<size_t>(kv_elems));
    std::vector<float> values_host(static_cast<size_t>(kv_elems));
    for (int64_t i = 0; i < kv_elems; ++i) {
        keys_host[static_cast<size_t>(i)] = elem_to_f32(kv.keys, kv.dtype, i);
        values_host[static_cast<size_t>(i)] = elem_to_f32(kv.values, kv.dtype, i);
    }
    id<MTLBuffer> kbuf = [impl_->device
        newBufferWithBytes:keys_host.data()
                    length:keys_host.size() * sizeof(float)
                   options:MTLResourceStorageModeShared];
    id<MTLBuffer> vbuf = [impl_->device
        newBufferWithBytes:values_host.data()
                    length:values_host.size() * sizeof(float)
                   options:MTLResourceStorageModeShared];

    const size_t out_bytes = static_cast<size_t>(num_heads) * static_cast<size_t>(head_dim) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!qbuf || !kbuf || !vbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "Attention: buffer alloc failed");
    }

    const uint32_t nh = static_cast<uint32_t>(num_heads);
    const uint32_t nkh = static_cast<uint32_t>(num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(head_dim);
    const uint32_t sl = static_cast<uint32_t>(seq_len);
    const float sc = scale;

    // Host KV path uses the unified flash kernel with kv_base = 0.
    const uint64_t kv_base = 0;
    const uint32_t attn_tsize = 128; // matches BLOCK in the flash kernel
    // Threadgroup memory regions: tg_q[head_dim] + tg_sc[128] + tg_acc[head_dim] + tg_red[128].
    const NSUInteger tg_q_bytes = static_cast<NSUInteger>(head_dim) * sizeof(float);
    const NSUInteger tg_acc_bytes = tg_q_bytes;
    const NSUInteger tg_sc_bytes = 128 * sizeof(float);
    const NSUInteger tg_red_bytes = tg_sc_bytes;

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->attention_flash_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:qbuf offset:0 atIndex:1];
    [enc setBuffer:kbuf offset:0 atIndex:2];
    [enc setBuffer:vbuf offset:0 atIndex:3];
    [enc setBytes:&nh length:sizeof(nh) atIndex:4];
    [enc setBytes:&nkh length:sizeof(nkh) atIndex:5];
    [enc setBytes:&hd length:sizeof(hd) atIndex:6];
    [enc setBytes:&sl length:sizeof(sl) atIndex:7];
    [enc setBytes:&sc length:sizeof(sc) atIndex:8];
    [enc setBytes:&kv_base length:sizeof(kv_base) atIndex:9];
    [enc setThreadgroupMemoryLength:tg_q_bytes atIndex:0];
    [enc setThreadgroupMemoryLength:tg_sc_bytes atIndex:1];
    [enc setThreadgroupMemoryLength:tg_acc_bytes atIndex:2];
    [enc setThreadgroupMemoryLength:tg_red_bytes atIndex:3];
    // One threadgroup per query head.
    [enc dispatchThreadgroups:MTLSizeMake(static_cast<NSUInteger>(num_heads), 1, 1)
         threadsPerThreadgroup:MTLSizeMake(attn_tsize, 1, 1)];
    // Deferred.
    return {};
}

// =========================================================================
// SwiGLU
// =========================================================================

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
    id<MTLBuffer> gbuf = upload_tensor(*impl_, gate);
    id<MTLBuffer> ubuf = upload_tensor(*impl_, up);
    const size_t out_bytes = static_cast<size_t>(n) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!gbuf || !ubuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "SwiGLU: buffer alloc failed");
    }

    const uint32_t ne = static_cast<uint32_t>(n);
    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->swiglu_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:gbuf offset:0 atIndex:1];
    [enc setBuffer:ubuf offset:0 atIndex:2];
    [enc setBytes:&ne length:sizeof(ne) atIndex:3];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(n), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    return {};
}

// =========================================================================
// AddInPlace
// =========================================================================

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
    // In-place: x is both input and output.  Upload x (if not cached),
    // and the residual.  The kernel writes into x's buffer.
    id<MTLBuffer> xbuf = upload_tensor(*impl_, x);
    id<MTLBuffer> rbuf = upload_tensor(*impl_, residual);
    if (!xbuf || !rbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "AddInPlace: buffer alloc failed");
    }

    // Mark x's shadow as an output so SyncToHost will download it.
    impl_->shadow_[reinterpret_cast<uintptr_t>(x.data())] =
        {xbuf, x.byte_size(), true};

    const uint32_t ne = static_cast<uint32_t>(n);
    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->add_ps];
    [enc setBuffer:xbuf offset:0 atIndex:0];
    [enc setBuffer:rbuf offset:0 atIndex:1];
    [enc setBytes:&ne length:sizeof(ne) atIndex:2];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(n), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    return {};
}

// =========================================================================
// AddBiasInPlace
// =========================================================================

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
    id<MTLBuffer> xbuf = upload_tensor(*impl_, x);
    id<MTLBuffer> bbuf = upload_tensor(*impl_, bias);
    if (!xbuf || !bbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "AddBiasInPlace: buffer alloc failed");
    }

    // Mark x's shadow as output.
    impl_->shadow_[reinterpret_cast<uintptr_t>(x.data())] =
        {xbuf, x.byte_size(), true};

    const uint32_t n = static_cast<uint32_t>(x.shape().dim(1));
    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->add_bias_ps];
    [enc setBuffer:xbuf offset:0 atIndex:0];
    [enc setBuffer:bbuf offset:0 atIndex:1];
    [enc setBytes:&n length:sizeof(n) atIndex:2];
    [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(total), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    return {};
}

// =========================================================================
// Synchronize
// =========================================================================

Status MetalBackend::Synchronize() {
    return impl_->flush();
}

// =========================================================================
// Device-residency hooks
// =========================================================================

Status MetalBackend::NotifyHostWrite(TensorView t) {
    impl_->invalidate(t.data());
    return {};
}

Status MetalBackend::SyncToHost(TensorView t) {
    // Flush any pending GPU work, then download the buffer to host memory.
    if (auto s = impl_->flush(); !s.ok()) {
        return s;
    }
    impl_->download(t.data(), t.byte_size());
    return {};
}

// Test-only: simulate a GPU-side failure.  Real MTLCommandBuffer errors
// cannot be raised deterministically from a unit test (they require hardware
// faults, timeouts, or debug-layer asserts that abort the process), so the
// error-injection seam exercises the sticky-error propagation path instead.
void MetalBackend::InjectGpuErrorForTest(Status error) {
    impl_->gpu_error_ = std::move(error);
}

// =========================================================================
// Device-resident KV cache
// =========================================================================

bool MetalBackend::HasDeviceKV() const {
    return true;
}

Status MetalBackend::ConfigureDeviceKV(int32_t num_layers,
                                        int32_t num_kv_heads,
                                        int32_t head_dim,
                                        int32_t capacity) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;

    const size_t per_layer = static_cast<size_t>(capacity) *
                             static_cast<size_t>(num_kv_heads) *
                             static_cast<size_t>(head_dim) * sizeof(float);
    const size_t total = per_layer * static_cast<size_t>(num_layers);

    id<MTLBuffer> kbuf = [impl_->device
        newBufferWithLength:total
                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> vbuf = [impl_->device
        newBufferWithLength:total
                    options:MTLResourceStorageModeShared];
    if (!kbuf || !vbuf) {
        return Status::Error(ErrorCode::kBackendFailure,
                             "ConfigureDeviceKV: buffer alloc failed");
    }
    std::memset(kbuf.contents, 0, total);
    std::memset(vbuf.contents, 0, total);

    impl_->device_kv_ = {kbuf, vbuf, num_layers, num_kv_heads, head_dim, capacity};
    return {};
}

Status MetalBackend::AppendKV(int32_t layer, TensorView key, TensorView value,
                              int64_t position) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;

    const auto& kv = impl_->device_kv_;
    if (layer < 0 || layer >= kv.num_layers) {
        return Status::Error(ErrorCode::kInvalidArgument, "AppendKV: layer out of range");
    }
    if (key.shape().rank() != 3 || key.shape().dim(1) != kv.num_kv_heads ||
        key.shape().dim(2) != kv.head_dim || value.shape() != key.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument, "AppendKV: key/value shape mismatch");
    }
    const int64_t rows = key.shape().dim(0);
    if (position < 0 || position + rows > kv.capacity) {
        return Status::Error(ErrorCode::kInvalidArgument, "AppendKV: position out of range");
    }

    // Get device buffers for key and value (they come from MatMul output,
    // already in shadow cache).
    id<MTLBuffer> kbuf = upload_tensor(*impl_, key);
    id<MTLBuffer> vbuf = upload_tensor(*impl_, value);
    if (!kbuf || !vbuf) {
        return Status::Error(ErrorCode::kBackendFailure, "AppendKV: buffer lookup failed");
    }

    const uint32_t nkv = static_cast<uint32_t>(kv.num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(kv.head_dim);
    const uint32_t cap = static_cast<uint32_t>(kv.capacity);
    const uint32_t lay = static_cast<uint32_t>(layer);
    const uint32_t pos = static_cast<uint32_t>(position);

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->append_kv_ps];
    [enc setBuffer:kv.keys offset:0 atIndex:0];
    [enc setBuffer:kv.values offset:0 atIndex:1];
    [enc setBuffer:kbuf offset:0 atIndex:2];
    [enc setBuffer:vbuf offset:0 atIndex:3];
    [enc setBytes:&nkv length:sizeof(nkv) atIndex:4];
    [enc setBytes:&hd length:sizeof(hd) atIndex:5];
    [enc setBytes:&cap length:sizeof(cap) atIndex:6];
    [enc setBytes:&lay length:sizeof(lay) atIndex:7];
    [enc setBytes:&pos length:sizeof(pos) atIndex:8];

    // One thread per (row, element); row b lands at position + b.
    const NSUInteger elems = static_cast<NSUInteger>(rows) * nkv * hd;
    [enc dispatchThreads:MTLSizeMake(elems, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    // Deferred.
    return {};
}

Status MetalBackend::ShiftKV(int64_t drop_tokens) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;

    const auto& kv = impl_->device_kv_;
    if (kv.keys == nil || kv.values == nil) {
        return Status::Error(ErrorCode::kBackendFailure, "ShiftKV: device KV not configured");
    }
    if (drop_tokens < 0 || drop_tokens > kv.capacity) {
        return Status::Error(ErrorCode::kInvalidArgument, "ShiftKV: drop out of range");
    }
    if (drop_tokens == 0 || drop_tokens == kv.capacity) {
        // Full drop needs no data movement (stale slots are never read
        // before being overwritten by the next appends).
        return {};
    }

    // Keep stream ordering with surrounding compute work: close the pending
    // compute encoder and encode the copies on the same command buffer via
    // a blit encoder. The next op lazily opens a fresh compute encoder on
    // this same command buffer.
    if (impl_->deferred_enc != nil) {
        [impl_->deferred_enc endEncoding];
        impl_->deferred_enc = nil;
    }
    if (impl_->deferred_cb == nil) {
        impl_->deferred_cb = [impl_->queue commandBuffer];
    }
    id<MTLBlitCommandEncoder> blit = [impl_->deferred_cb blitCommandEncoder];

    const size_t row_bytes = static_cast<size_t>(kv.num_kv_heads) *
                             static_cast<size_t>(kv.head_dim) * sizeof(float);
    const size_t layer_stride = row_bytes * static_cast<size_t>(kv.capacity);
    const NSUInteger src_delta = static_cast<NSUInteger>(drop_tokens) * row_bytes;
    const NSUInteger shifted_bytes = layer_stride - src_delta;

    // Copy in forward chunks no larger than `src_delta` so every individual
    // blit is strictly non-overlapping within the same buffer (an explicit
    // chunk is only needed when the Engine flushes a partially-filled
    // window and drop_tokens < capacity - drop_tokens; the usual
    // ceil(capacity/2) compaction chunk takes exactly one blit). Forward
    // chunked copies reproduce memmove semantics for a left shift.
    const NSUInteger chunk_bytes = std::min<NSUInteger>(shifted_bytes, src_delta);
    for (int32_t l = 0; l < kv.num_layers; ++l) {
        const NSUInteger layer_base = static_cast<NSUInteger>(l) * layer_stride;
        for (NSUInteger moved = 0; moved < shifted_bytes; moved += chunk_bytes) {
            const NSUInteger size = std::min<NSUInteger>(shifted_bytes - moved, chunk_bytes);
            [blit copyFromBuffer:kv.keys
                    sourceOffset:layer_base + src_delta + moved
                        toBuffer:kv.keys
               destinationOffset:layer_base + moved
                            size:size];
            [blit copyFromBuffer:kv.values
                    sourceOffset:layer_base + src_delta + moved
                        toBuffer:kv.values
               destinationOffset:layer_base + moved
                            size:size];
        }
    }
    [blit endEncoding];
    // Deferred (committed at the next flush like every other op).
    return {};
}

Status MetalBackend::AttentionKV(TensorView out, TensorView q, int32_t layer,
                                   int64_t seq_len, const AttentionConfig& config) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "AttentionKV"); !s.ok()) return s;
    if (auto s = check_contig_valid(q, "AttentionKV"); !s.ok()) return s;

    const auto& kv = impl_->device_kv_;
    const int32_t num_heads = config.num_heads;
    const int32_t num_kv_heads = config.num_kv_heads;
    const int32_t head_dim = config.head_dim;
    const float scale = config.scale > 0.0f
        ? config.scale
        : 1.0f / std::sqrt(static_cast<float>(head_dim));

    if (layer < 0 || layer >= kv.num_layers) {
        return Status::Error(ErrorCode::kInvalidArgument, "AttentionKV: layer out of range");
    }
    if (kv.num_kv_heads != num_kv_heads || kv.head_dim != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "AttentionKV: KV shape mismatch");
    }
    if (q.shape().dim(1) != num_heads || q.shape().dim(2) != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "AttentionKV: q shape mismatch");
    }
    if (out.shape().dim(0) != 1 || out.shape().dim(1) != num_heads * head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "AttentionKV: output shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "AttentionKV: output must be f32");
    }

    id<MTLBuffer> qbuf = upload_tensor(*impl_, q);
    const size_t out_bytes = static_cast<size_t>(num_heads) * static_cast<size_t>(head_dim) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!qbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure, "AttentionKV: buffer alloc failed");
    }

    const uint32_t nh = static_cast<uint32_t>(num_heads);
    const uint32_t nkh = static_cast<uint32_t>(num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(head_dim);
    const uint32_t sl = static_cast<uint32_t>(seq_len);
    // Device KV path uses the unified flash kernel with kv_base = layer * capacity * num_kv_heads * head_dim.
    const uint64_t kv_base = static_cast<uint64_t>(layer)
                           * static_cast<uint64_t>(kv.capacity)
                           * static_cast<uint64_t>(num_kv_heads)
                           * static_cast<uint64_t>(head_dim);
    const float sc = scale;
    const uint32_t attn_tsize = 128; // matches BLOCK in the flash kernel
    const NSUInteger tg_q_bytes = static_cast<NSUInteger>(head_dim) * sizeof(float);
    const NSUInteger tg_acc_bytes = tg_q_bytes;
    const NSUInteger tg_sc_bytes = 128 * sizeof(float);
    const NSUInteger tg_red_bytes = tg_sc_bytes;

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    // Decode flash: 8 simdgroups per 256-thread group, one group per head;
    // barrier-free online softmax in the hot loop. Requires head_dim % 32
    // == 0 and <= 256 (register budget); otherwise fall back to the
    // blockwise flash kernel.
    const bool use_decode = (head_dim % 32 == 0) && (head_dim <= 256);
    if (use_decode) {
        [enc setComputePipelineState:impl_->attention_decode_ps];
    } else {
        [enc setComputePipelineState:impl_->attention_flash_ps];
    }
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:qbuf offset:0 atIndex:1];
    [enc setBuffer:kv.keys offset:0 atIndex:2];
    [enc setBuffer:kv.values offset:0 atIndex:3];
    [enc setBytes:&nh length:sizeof(nh) atIndex:4];
    [enc setBytes:&nkh length:sizeof(nkh) atIndex:5];
    [enc setBytes:&hd length:sizeof(hd) atIndex:6];
    [enc setBytes:&sl length:sizeof(sl) atIndex:7];
    [enc setBytes:&sc length:sizeof(sc) atIndex:8];
    [enc setBytes:&kv_base length:sizeof(kv_base) atIndex:9];
    if (use_decode) {
        // 8 simdgroups x (head_dim + 2) floats merge scratch.
        const NSUInteger tg_bytes =
            static_cast<NSUInteger>(8) * (static_cast<NSUInteger>(head_dim) + 2) *
            sizeof(float);
        [enc setThreadgroupMemoryLength:tg_bytes atIndex:0];
        [enc dispatchThreadgroups:MTLSizeMake(static_cast<NSUInteger>(num_heads), 1, 1)
             threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    } else {
        [enc setThreadgroupMemoryLength:tg_q_bytes atIndex:0];
        [enc setThreadgroupMemoryLength:tg_sc_bytes atIndex:1];
        [enc setThreadgroupMemoryLength:tg_acc_bytes atIndex:2];
        [enc setThreadgroupMemoryLength:tg_red_bytes atIndex:3];
        [enc dispatchThreadgroups:MTLSizeMake(static_cast<NSUInteger>(num_heads), 1, 1)
             threadsPerThreadgroup:MTLSizeMake(attn_tsize, 1, 1)];
    }
    // Deferred.
    return {};
}

Status MetalBackend::AttentionPrefillKV(TensorView out, TensorView q, int32_t layer,
                                        int64_t seq_base, const AttentionConfig& config) {
    if (auto s = ensure_ready(*impl_); !s.ok()) return s;
    if (auto s = check_contig_valid(out, "AttentionPrefillKV"); !s.ok()) return s;
    if (auto s = check_contig_valid(q, "AttentionPrefillKV"); !s.ok()) return s;

    const auto& kv = impl_->device_kv_;
    const int32_t num_heads = config.num_heads;
    const int32_t num_kv_heads = config.num_kv_heads;
    const int32_t head_dim = config.head_dim;
    const int64_t n = q.shape().dim(0);
    const float scale = config.scale > 0.0f
        ? config.scale
        : 1.0f / std::sqrt(static_cast<float>(head_dim));

    if (n <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionPrefillKV: empty batch");
    }
    if (layer < 0 || layer >= kv.num_layers) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionPrefillKV: layer out of range");
    }
    if (kv.num_kv_heads != num_kv_heads || kv.head_dim != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionPrefillKV: KV shape mismatch");
    }
    if (q.shape().rank() != 3 || q.shape().dim(1) != num_heads ||
        q.shape().dim(2) != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionPrefillKV: q shape mismatch");
    }
    if (out.shape().rank() != 2 || out.shape().dim(0) != n ||
        out.shape().dim(1) != num_heads * head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionPrefillKV: output shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported,
                             "AttentionPrefillKV: output must be f32");
    }
    if (seq_base <= 0 || seq_base + n - 1 > kv.capacity) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionPrefillKV: seq_base out of range");
    }

    id<MTLBuffer> qbuf = upload_tensor(*impl_, q);
    const size_t out_bytes = static_cast<size_t>(n) * static_cast<size_t>(num_heads) *
                             static_cast<size_t>(head_dim) * sizeof(float);
    id<MTLBuffer> obuf = impl_->get_or_alloc_output(out.data(), out_bytes);
    if (!qbuf || !obuf) {
        return Status::Error(ErrorCode::kBackendFailure,
                             "AttentionPrefillKV: buffer alloc failed");
    }

    const uint32_t nh = static_cast<uint32_t>(num_heads);
    const uint32_t nkh = static_cast<uint32_t>(num_kv_heads);
    const uint32_t hd = static_cast<uint32_t>(head_dim);
    const uint32_t sb = static_cast<uint32_t>(seq_base);
    const uint64_t kv_base = static_cast<uint64_t>(layer)
                           * static_cast<uint64_t>(kv.capacity)
                           * static_cast<uint64_t>(num_kv_heads)
                           * static_cast<uint64_t>(head_dim);
    const float sc = scale;
    const uint32_t attn_tsize = 128; // matches BLOCK in the flash kernel
    const NSUInteger tg_q_bytes = static_cast<NSUInteger>(head_dim) * sizeof(float);
    const NSUInteger tg_sc_bytes = 128 * sizeof(float);

    id<MTLComputeCommandEncoder> enc = impl_->encoder();
    [enc setComputePipelineState:impl_->attention_flash_batch_ps];
    [enc setBuffer:obuf offset:0 atIndex:0];
    [enc setBuffer:qbuf offset:0 atIndex:1];
    [enc setBuffer:kv.keys offset:0 atIndex:2];
    [enc setBuffer:kv.values offset:0 atIndex:3];
    [enc setBytes:&nh length:sizeof(nh) atIndex:4];
    [enc setBytes:&nkh length:sizeof(nkh) atIndex:5];
    [enc setBytes:&hd length:sizeof(hd) atIndex:6];
    [enc setBytes:&sb length:sizeof(sb) atIndex:7];
    [enc setBytes:&sc length:sizeof(sc) atIndex:8];
    [enc setBytes:&kv_base length:sizeof(kv_base) atIndex:9];
    [enc setThreadgroupMemoryLength:tg_q_bytes atIndex:0];
    [enc setThreadgroupMemoryLength:tg_sc_bytes atIndex:1];
    [enc setThreadgroupMemoryLength:tg_q_bytes atIndex:2];
    [enc setThreadgroupMemoryLength:tg_sc_bytes atIndex:3];
    // One threadgroup per (head, query row).
    [enc dispatchThreadgroups:MTLSizeMake(static_cast<NSUInteger>(num_heads),
                                          static_cast<NSUInteger>(n),
                                          1)
         threadsPerThreadgroup:MTLSizeMake(attn_tsize, 1, 1)];
    // Deferred.
    return {};
}

} // namespace pl::mllm
