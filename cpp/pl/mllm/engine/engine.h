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

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/media/image.h"
#include "cpp/pl/mllm/sampler/sampler.h"

namespace pl::mllm {

// Backend selection. kCpu works on every platform; kMetal requires macOS
// (Engine::Create returns kUnsupported elsewhere).
enum class BackendKind {
    kCpu,
    kMetal,
};

// Generation parameters.
struct GenerateParams {
    int32_t max_tokens = 128;
    float temperature = 0.0f;    // <= 0 means greedy
    int32_t top_k = 0;           // 0 = disabled
    float top_p = 1.0f;          // 1.0 = disabled
    float repeat_penalty = 1.0f; // 1.0 = disabled
    uint64_t seed = 0;
};

// One multimodal generation input. `prompt` must contain exactly
// images.size() image-placeholder tokens (Options::image_placeholder);
// each placeholder expands to the corresponding image's visual tokens.
// A plain text prompt is a GenerateInput with empty `images`.
struct GenerateInput {
    std::string prompt;
    std::vector<media::Image> images;
};

// Performance statistics for a single Generate call.
struct PerfStats {
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    double prefill_ms = 0.0;
    double decode_ms = 0.0;
    double total_ms = 0.0;
    double tok_per_sec = 0.0;
    double time_to_first_token_ms = 0.0;
};

// Inference engine that owns model, tokenizer, sampler, backend, and KV cache.
// See SPEC §9.1.
class Engine {
public:
    struct Options {
        std::string model_path;
        int32_t max_context = 4096;
        BackendKind backend = BackendKind::kCpu;
        // Sliding-window ("ring") KV cache (SPEC §7.2). When true, the
        // cache capacity = min(max_context, model context length) acts as a
        // window: once the sequence outgrows it, the oldest tokens' K/V are
        // dropped (chunked compaction behind the window origin) instead of
        // failing, so prompts/generations of arbitrary length are allowed.
        // RoPE positions stay absolute; attention over the retained window
        // stays exact-window causal. Quality beyond the window follows
        // sliding-window semantics (no attention sink).
        bool ring = false;
        // Optional mmproj GGUF (vision tower weights) enabling image inputs
        // (e.g. PaddleOCR-VL). Empty = text-only engine.
        std::string mmproj_path;
        // Vocab piece marking an image slot in the prompt. The tokenizer
        // must carry it as a special token (PaddleOCR-VL convention below).
        std::string image_placeholder = "<|IMAGE_PLACEHOLDER|>";
        // Explicit MRoPE (t, h, w) section split overriding the GGUF
        // metadata; {0, 0, 0} = use the model's own
        // `<arch>.rope.mrope_section`.
        std::array<int32_t, 3> mrope_section{0, 0, 0};
    };

    [[nodiscard]] static Result<std::unique_ptr<Engine>> Create(Options options);

    // Destructor: public so unique_ptr/variant can destroy; defined in .cpp.
    ~Engine();

    // Non-streaming generation. Returns generated token ids (excluding prompt).
    [[nodiscard]] Result<std::vector<int32_t>> GenerateTokens(std::string_view prompt,
                                                              GenerateParams params);
    [[nodiscard]] Result<std::vector<int32_t>> GenerateTokens(const GenerateInput& input,
                                                              GenerateParams params);

    // Streaming generation. Calls `on_piece` for each decoded text fragment
    // and its token id. Returning false from `on_piece` cancels generation.
    [[nodiscard]] Status GenerateStream(std::string_view prompt,
                                        GenerateParams params,
                                        std::function<bool(std::string_view, int32_t)> on_piece);
    [[nodiscard]] Status GenerateStream(const GenerateInput& input,
                                        GenerateParams params,
                                        std::function<bool(std::string_view, int32_t)> on_piece);

    // Whether the engine was created with a vision tower (mmproj).
    [[nodiscard]] bool has_vision() const noexcept;

    [[nodiscard]] const PerfStats& last_perf_stats() const noexcept { return perf_; }

    // Format a user (and optional system) message with the model's chat
    // template, taken from the GGUF `tokenizer.chat_template` metadata.
    // Supports the ChatML (Qwen), Llama-2 and Llama-3 template families;
    // falls back to the raw user message when no template is present.
    // The result ends with the assistant-turn opener, so the model
    // continues as the assistant.
    [[nodiscard]] std::string FormatChatPrompt(std::string_view user,
                                               std::string_view system = {}) const;

    // Whether the model ships a recognized chat template.
    [[nodiscard]] bool has_chat_template() const noexcept;

private:
    Engine() = default;

    // Prompt tokens processed per batched-prefill forward pass. Larger
    // chunks amortize weight-dequant and dispatch cost but grow the scratch
    // arena (~per_layer_bytes * chunk * 2).
    static constexpr int32_t kPrefillChunk = 64;

    // Run prefill: embed each token, forward through model in batched
    // chunks. Returns the final hidden state (a view into an engine-owned
    // buffer, valid until the next RunPrefill) so the caller can sample the
    // first generated token without re-forwarding the last prompt token.
    [[nodiscard]] Result<TensorView> RunPrefill(std::span<const int32_t> tokens);

    // Multimodal prefill: Encode()s every image, splices the visual
    // embeddings into the prompt rows at the placeholder positions, builds
    // the MRoPE geometry and prefills the spliced sequence. Returns the
    // final hidden state plus the rope-position delta that maps decode-time
    // sequence indices to rope positions (0 for text-only prompts).
    struct MultimodalPrefill {
        TensorView hidden;
        int64_t mrope_delta = 0;
        int32_t prompt_rows = 0; // KV rows consumed (== tokens + image rows - placeholders)
    };
    [[nodiscard]] Result<MultimodalPrefill> RunPrefillMultimodal(
        std::span<const int32_t> tokens, std::span<const media::Image> images);

    // Shared decode stage: given the hidden state of the last prompt row,
    // sample and forward until EOS / max_tokens / cancellation.
    // `prompt_tokens` feeds the repetition-penalty window; `prompt_rows` is
    // the physical KV length after prefill (== prompt_tokens.size() for
    // text); decode token j is forwarded at rope position
    // `mrope_delta + prompt_rows + j` (mrope_delta == 0 for text).
    [[nodiscard]] Status DecodeStage(
        TensorView hidden_state,
        std::span<const int32_t> prompt_tokens,
        int32_t prompt_rows,
        int64_t mrope_delta,
        GenerateParams params,
        std::chrono::steady_clock::time_point t_start,
        const std::function<bool(std::string_view, int32_t)>& on_piece);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Ring-mode device-KV room keeper (see engine.cpp): before appending K/V
    // for absolute positions [abs_pos, abs_pos + incoming) to a device KV
    // cache, ensure the window can hold them by shifting out the oldest
    // tokens (ceil(capacity/2) per shift). Device buffers and the host shell stay
    // in sync. Only called when ring mode + device KV are active.
    static Status EnsureDeviceKvRoom(Impl& impl, int64_t abs_pos, int32_t incoming);
    PerfStats perf_;
};

} // namespace pl::mllm
