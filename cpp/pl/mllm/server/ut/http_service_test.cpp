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
// Created: 2026/09/13

// End-to-end test of MllmHttpService: a hermetic tiny GGUF model (same
// construction as cpp/pl/mllm/ut/engine/engine_test.cpp), a real brpc::Server
// on an ephemeral port, and a brpc HTTP channel as the client.

#include <brpc/channel.h>
#include <brpc/server.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <simdjson.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "cpp/pl/mllm/engine/engine.h"
#include "cpp/pl/mllm/server/http_service.h"
#include "cpp/pl/mllm/server/http_util.h"
#include "cpp/pl/mllm/ut/testdata/gguf_writer.h"

namespace pl::mllm::server {
namespace {

namespace td = pl::mllm::testdata;

// ---------------------------------------------------------------------------
// Tiny hermetic model (F32 llama, identical weights to engine_test.cpp so its
// greedy determinism results carry over).
// ---------------------------------------------------------------------------

class TempFile {
public:
    explicit TempFile(std::vector<uint8_t> bytes) {
        // mkstemp actually instantiates the XXXXXX template: a plain fixed
        // name means every TempFile in this binary shares ONE path and later
        // TempFiles overwrite earlier ones (bit us the moment a fixture
        // needed model + mmproj files alive at the same time).
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "mllm_http_service_test_XXXXXX").string();
        const int fd = mkstemp(tmpl.data());
        if (fd < 0) {
            ADD_FAILURE() << "TempFile: mkstemp failed, errno=" << errno;
            path_ = tmpl;
            return;
        }
        ::close(fd);
        path_ = std::move(tmpl);
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() { std::filesystem::remove(path_); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

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

constexpr int32_t kHidden = 16;
constexpr int32_t kInter = 32;
constexpr int32_t kVocabSize = 11;
constexpr int32_t kCtx = 64;
constexpr int32_t kHeads = 2;
constexpr int32_t kKVHeads = 1;
constexpr int32_t kHeadDim = 8;
constexpr uint32_t kWeightSeed = 42;

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

const std::vector<std::string> kVocab = {
    "<s>",
    "</s>",
    " ",
    "a",
    "b",
    "c",
    "<0x00>",
    "<0x01>",
    // Special pieces so the image-splice scaffold
    // "<|IMAGE_START|><|IMAGE_PLACEHOLDER|><|IMAGE_END|>" tokenizes on this
    // tiny model (whole-piece lookup; real OCR models carry them likewise).
    "<|IMAGE_START|>",
    "<|IMAGE_PLACEHOLDER|>",
    "<|IMAGE_END|>",
};

td::GgufWriter make_tiny_model_writer() {
    td::GgufWriter w("llama");
    w.meta_u32("llama.context_length", kCtx);
    w.meta_u32("llama.embedding_length", kHidden);
    w.meta_u32("llama.feed_forward_length", kInter);
    w.meta_u32("llama.block_count", 1);
    w.meta_u32("llama.attention.head_count", kHeads);
    w.meta_u32("llama.attention.head_count_kv", kKVHeads);
    w.meta_f32("llama.attention.layer_norm_rms_epsilon", 1e-5f);
    w.meta_f32("llama.rope.freq_base", 10000.0f);
    w.meta_str_array("tokenizer.ggml.tokens", kVocab);
    w.meta_f32_array("tokenizer.ggml.scores",
                     {0.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f, -9.0f});
    // llama.cpp token types: NORMAL=1, CONTROL=3, BYTE=6. Special (non
    // NORMAL/BYTE) pieces match as whole units during encoding, which is
    // what lets the image splice scaffold survive tokenization.
    w.meta_i32_array("tokenizer.ggml.token_type", {3, 3, 1, 1, 1, 1, 6, 6, 3, 3, 3});
    w.meta_bool("tokenizer.ggml.add_bos_token", true);
    w.meta_u32("tokenizer.ggml.bos_token_id", 0);
    w.meta_u32("tokenizer.ggml.eos_token_id", 1);

    w.tensor({"token_embd.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kVocabSize)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed, kHidden * kVocabSize))});
    w.tensor({"output_norm.weight",
              {static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden))});

    const std::string p = "blk.0.";
    w.tensor({p + "attn_norm.weight",
              {static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden))});
    w.tensor({p + "attn_q.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 1, kHidden * kHidden))});
    w.tensor({p + "attn_k.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kKVHeads * kHeadDim)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 2, kKVHeads * kHeadDim * kHidden))});
    w.tensor({p + "attn_v.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kKVHeads * kHeadDim)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 3, kKVHeads * kHeadDim * kHidden))});
    w.tensor({p + "attn_output.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 4, kHidden * kHidden))});
    w.tensor({p + "ffn_norm.weight",
              {static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(ones(kHidden))});
    w.tensor({p + "ffn_gate.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kInter)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 5, kInter * kHidden))});
    w.tensor({p + "ffn_up.weight",
              {static_cast<uint64_t>(kHidden), static_cast<uint64_t>(kInter)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 6, kInter * kHidden))});
    w.tensor({p + "ffn_down.weight",
              {static_cast<uint64_t>(kInter), static_cast<uint64_t>(kHidden)},
              td::GgufType::kF32,
              f32_bytes(make_weights(kWeightSeed + 7, kHidden * kInter))});
    return w;
}

// ---------------------------------------------------------------------------
// Hermetic vision mmproj: minimal PaddleOCR-VL-style clip GGUF. Same geometry
// as cpp/pl/mllm/ut/vision/vision_tower_test.cpp, with the projector output
// resized to the tiny LM's hidden size (kHidden == 16).
// ---------------------------------------------------------------------------

constexpr int32_t kVisHidden = 8;
constexpr int32_t kVisInter = 16;
constexpr int32_t kVisHeads = 2;
constexpr int32_t kVisPatch = 2;
constexpr int32_t kVisMerge = 2;
constexpr int32_t kVisRefGrid = 4;
constexpr int32_t kVisPatchDim = 3 * kVisPatch * kVisPatch;
constexpr int32_t kVisMergedDim = kVisMerge * kVisMerge * kVisHidden;

td::GgufWriter make_tiny_mmproj_writer() {
    td::GgufWriter w("clip");
    w.meta_string("clip.projector_type", "paddleocr");
    w.meta_u32("clip.vision.embedding_length", kVisHidden);
    w.meta_u32("clip.vision.feed_forward_length", kVisInter);
    w.meta_u32("clip.vision.block_count", 1);
    w.meta_u32("clip.vision.attention.head_count", kVisHeads);
    w.meta_u32("clip.vision.patch_size", kVisPatch);
    w.meta_u32("clip.vision.spatial_merge_size", kVisMerge);
    // Engine::Create rejects a mismatch between the projector output and the
    // LM hidden size, so this must be kHidden, not kVisHidden.
    w.meta_u32("clip.vision.projection_dim", kHidden);
    w.meta_f32("clip.vision.attention.layer_norm_epsilon", 1e-6f);

    uint32_t seed = 7;
    auto pick = [&](const std::string& name, std::initializer_list<uint64_t> dims, size_t count) {
        w.tensor({name,
                  std::vector<uint64_t>(dims),
                  td::GgufType::kF32,
                  f32_bytes(make_weights(seed++, count))});
    };
    auto fill = [&](const std::string& name,
                    std::initializer_list<uint64_t> dims,
                    const std::vector<float>& values) {
        w.tensor({name, std::vector<uint64_t>(dims), td::GgufType::kF32, f32_bytes(values)});
    };

    // GGML dims are column-major (reversed on load):
    // loaded [out_dim, in_dim] <- ggml {in_dim, out_dim}.
    pick("v.patch_embd.weight", {kVisPatchDim, kVisHidden}, kVisHidden * kVisPatchDim);
    pick("v.patch_embd.bias", {kVisHidden}, kVisHidden);
    pick("v.position_embd.weight",
         {kVisHidden, kVisRefGrid * kVisRefGrid},
         kVisRefGrid * kVisRefGrid * kVisHidden);
    pick("v.post_ln.weight", {kVisHidden}, kVisHidden);
    fill("v.post_ln.bias", {kVisHidden}, zeros(kVisHidden));

    const std::string p = "v.blk.0.";
    pick(p + "attn_q.weight", {kVisHidden, kVisHidden}, kVisHidden * kVisHidden);
    pick(p + "attn_k.weight", {kVisHidden, kVisHidden}, kVisHidden * kVisHidden);
    pick(p + "attn_v.weight", {kVisHidden, kVisHidden}, kVisHidden * kVisHidden);
    pick(p + "attn_out.weight", {kVisHidden, kVisHidden}, kVisHidden * kVisHidden);
    pick(p + "ffn_up.weight", {kVisHidden, kVisInter}, kVisInter * kVisHidden);
    pick(p + "ffn_down.weight", {kVisInter, kVisHidden}, kVisHidden * kVisInter);
    for (const char* b : {"attn_q.bias", "attn_k.bias", "attn_v.bias", "attn_out.bias"}) {
        fill(p + b, {kVisHidden}, zeros(kVisHidden));
    }
    fill(p + "ffn_up.bias", {kVisInter}, zeros(kVisInter));
    fill(p + "ffn_down.bias", {kVisHidden}, zeros(kVisHidden));
    fill(p + "ln1.weight", {kVisHidden}, ones(kVisHidden));
    fill(p + "ln1.bias", {kVisHidden}, zeros(kVisHidden));
    fill(p + "ln2.weight", {kVisHidden}, ones(kVisHidden));
    fill(p + "ln2.bias", {kVisHidden}, zeros(kVisHidden));

    fill("mm.input_norm.weight", {kVisHidden}, ones(kVisHidden));
    fill("mm.input_norm.bias", {kVisHidden}, zeros(kVisHidden));
    pick("mm.0.weight", {kVisMergedDim, kVisMergedDim}, kVisMergedDim * kVisMergedDim);
    fill("mm.0.bias", {kVisMergedDim}, zeros(kVisMergedDim));
    pick("mm.2.weight", {kVisMergedDim, kHidden}, kHidden * kVisMergedDim);
    fill("mm.2.bias", {kHidden}, zeros(kHidden));
    return w;
}

// ---------------------------------------------------------------------------
// HTTP fixture
// ---------------------------------------------------------------------------

struct HttpResult {
    bool call_ok = false;
    std::string fail_text;
    int status = 0;
    std::string body;
    std::string content_type;
    std::string acao;          // Access-Control-Allow-Origin
    std::string allow_methods; // Access-Control-Allow-Methods
};

class HttpServiceTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto writer = make_tiny_model_writer();
        model_file_ = std::make_unique<TempFile>(writer.build(32));

        Engine::Options opts;
        opts.model_path = model_file_->path();
        opts.backend = BackendKind::kCpu;
        auto engine_result = Engine::Create(opts);
        ASSERT_TRUE(engine_result.ok()) << engine_result.status().message;
        engine_ = std::move(engine_result).value();

        ServerConfig config;
        config.model_name = "test-model";
        config.default_max_tokens = 3;
        config.max_body_bytes = 4096; // small on purpose: cheap 413 test
        config.max_image_bytes = 4096;
        service_ = std::make_unique<MllmHttpService>(engine_.get(), config);

        server_ = std::make_unique<brpc::Server>();
        ASSERT_EQ(server_->AddService(service_.get(),
                                      brpc::SERVER_DOESNT_OWN_SERVICE,
                                      "/v1/* => default_method,"
                                      "/healthz => default_method"),
                  0);
        brpc::ServerOptions options;
        ASSERT_EQ(server_->Start("127.0.0.1:0", &options), 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(server_->listen_address().port);

        // Intentionally leaked: tearing a channel down races bthread globals.
        auto* channel = new brpc::Channel();
        brpc::ChannelOptions channel_options;
        channel_options.protocol = brpc::PROTOCOL_HTTP;
        channel_options.timeout_ms = 60000;
        ASSERT_EQ(channel->Init(base_url_.c_str(), &channel_options), 0);
        channel_ = channel;
    }

    static void TearDownTestSuite() {
        server_->Stop(0);
        server_->Join();
        server_.reset();
        service_.reset();
        engine_.reset();
        model_file_.reset();
    }

    static HttpResult HttpDo(brpc::HttpMethod method,
                             const std::string& path,
                             const std::string& body = {}) {
        brpc::Controller cntl;
        cntl.http_request().uri() = base_url_ + path;
        cntl.http_request().set_method(method);
        if (!body.empty()) {
            cntl.request_attachment().append(body);
        }
        channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
        HttpResult r;
        r.call_ok = !cntl.Failed();
        r.fail_text = cntl.ErrorText();
        r.status = cntl.http_response().status_code();
        r.body = cntl.response_attachment().to_string();
        r.content_type = cntl.http_response().content_type();
        if (const std::string* h = cntl.http_response().GetHeader("Access-Control-Allow-Origin")) {
            r.acao = *h;
        }
        if (const std::string* h = cntl.http_response().GetHeader("Access-Control-Allow-Methods")) {
            r.allow_methods = *h;
        }
        return r;
    }

    static simdjson::dom::element ParseBody(simdjson::dom::parser& parser,
                                            const std::string& body) {
        simdjson::dom::element root;
        if (parser.parse(body).get(root) != simdjson::SUCCESS) {
            ADD_FAILURE() << "response body is not valid JSON: " << body;
            return {};
        }
        return root;
    }

    static std::string ErrorType(const std::string& body) {
        simdjson::dom::parser parser;
        const auto root = ParseBody(parser, body);
        std::string_view type;
        if (root["error"]["type"].get(type) != simdjson::SUCCESS) {
            ADD_FAILURE() << "no error.type in body: " << body;
            return {};
        }
        return std::string(type);
    }

    static std::unique_ptr<TempFile> model_file_;
    static std::unique_ptr<Engine> engine_;
    static std::unique_ptr<MllmHttpService> service_;
    static std::unique_ptr<brpc::Server> server_;
    static brpc::Channel* channel_;
    static std::string base_url_;
};

std::unique_ptr<TempFile> HttpServiceTest::model_file_;
std::unique_ptr<Engine> HttpServiceTest::engine_;
std::unique_ptr<MllmHttpService> HttpServiceTest::service_;
std::unique_ptr<brpc::Server> HttpServiceTest::server_;
brpc::Channel* HttpServiceTest::channel_ = nullptr;
std::string HttpServiceTest::base_url_;

// dom::element's integer indexing is deleted (ambiguity guard); unwrap the
// first array element via the iterator API instead of [].
simdjson::dom::element FirstElement(const simdjson::dom::element& arr_holder,
                                    std::string_view key) {
    simdjson::dom::array arr;
    if (arr_holder.at_key(key).get(arr) != simdjson::SUCCESS) {
        ADD_FAILURE() << "missing array field: " << key;
        return {};
    }
    auto it = arr.begin();
    if (it == arr.end()) {
        ADD_FAILURE() << "empty array field: " << key;
        return {};
    }
    return *it;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(HttpServiceTest, Healthz) {
    const auto r = HttpDo(brpc::HTTP_METHOD_GET, "/healthz");
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, R"({"status":"ok"})");
    EXPECT_NE(r.content_type.find("charset=utf-8"), std::string::npos);
    EXPECT_EQ(r.acao, "*");
}

TEST_F(HttpServiceTest, ListModels) {
    const auto r = HttpDo(brpc::HTTP_METHOD_GET, "/v1/models");
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    ASSERT_EQ(r.status, 200) << r.body;
    simdjson::dom::parser parser;
    const auto root = ParseBody(parser, r.body);
    std::string_view sv;
    ASSERT_EQ(root["object"].get(sv), simdjson::SUCCESS);
    EXPECT_EQ(sv, "list");
    const auto model = FirstElement(root, "data");
    ASSERT_EQ(model["id"].get(sv), simdjson::SUCCESS);
    EXPECT_EQ(sv, "test-model");
}

TEST_F(HttpServiceTest, CorsPreflight) {
    const auto r = HttpDo(brpc::HTTP_METHOD_OPTIONS, "/v1/chat/completions");
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    EXPECT_EQ(r.status, 204);
    EXPECT_EQ(r.acao, "*");
    EXPECT_NE(r.allow_methods.find("POST"), std::string::npos);
}

TEST_F(HttpServiceTest, UnknownRouteUnderV1) {
    // Note: the brpc HTTP client maps non-2xx statuses to a controller
    // failure (EHTTP), so expectations on error responses only check the
    // status code and body, not call_ok.
    const auto r = HttpDo(brpc::HTTP_METHOD_GET, "/v1/unknown");
    EXPECT_EQ(r.status, 404) << r.fail_text;
    EXPECT_EQ(ErrorType(r.body), "not_found");
}

TEST_F(HttpServiceTest, ChatCompletionShapeAndCors) {
    const auto r = HttpDo(brpc::HTTP_METHOD_POST,
                          "/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":"a"}],"max_tokens":4})");
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.acao, "*");
    EXPECT_NE(r.content_type.find("charset=utf-8"), std::string::npos);

    simdjson::dom::parser parser;
    const auto root = ParseBody(parser, r.body);
    std::string_view sv;
    ASSERT_EQ(root["object"].get(sv), simdjson::SUCCESS);
    EXPECT_EQ(sv, "chat.completion");
    ASSERT_EQ(root["id"].get(sv), simdjson::SUCCESS);
    EXPECT_TRUE(sv.starts_with("chatcmpl-"));
    const auto choice = FirstElement(root, "choices");
    ASSERT_EQ(choice["message"]["role"].get(sv), simdjson::SUCCESS);
    EXPECT_EQ(sv, "assistant");
    ASSERT_EQ(choice["message"]["content"].get(sv), simdjson::SUCCESS);
    EXPECT_FALSE(sv.empty());

    int64_t prompt_tokens = -1;
    int64_t completion_tokens = -1;
    int64_t total_tokens = -1;
    ASSERT_EQ(root["usage"]["prompt_tokens"].get(prompt_tokens), simdjson::SUCCESS);
    ASSERT_EQ(root["usage"]["completion_tokens"].get(completion_tokens), simdjson::SUCCESS);
    ASSERT_EQ(root["usage"]["total_tokens"].get(total_tokens), simdjson::SUCCESS);
    EXPECT_GT(prompt_tokens, 0);
    EXPECT_GE(completion_tokens, 1);
    EXPECT_LE(completion_tokens, 4);
    EXPECT_EQ(total_tokens, prompt_tokens + completion_tokens);

    // finish_reason must follow OpenAI semantics: "length" iff the token cap
    // was hit (P0 fix — it used to be hardcoded "stop").
    ASSERT_EQ(choice["finish_reason"].get(sv), simdjson::SUCCESS);
    if (completion_tokens == 4) {
        EXPECT_EQ(sv, "length");
    } else {
        EXPECT_EQ(sv, "stop");
    }
}

TEST_F(HttpServiceTest, FinishReasonLengthWhenCapHit) {
    // max_tokens=1: the tiny model never samples EOS on its first token for
    // this prompt (see engine_test's generated > 0), so the cap IS hit and
    // the server must report "length".
    const auto r = HttpDo(brpc::HTTP_METHOD_POST,
                          "/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":"a"}],"max_tokens":1})");
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    ASSERT_EQ(r.status, 200) << r.body;

    simdjson::dom::parser parser;
    const auto root = ParseBody(parser, r.body);
    std::string_view sv;
    int64_t completion_tokens = -1;
    ASSERT_EQ(root["usage"]["completion_tokens"].get(completion_tokens), simdjson::SUCCESS);
    EXPECT_EQ(completion_tokens, 1);
    const auto choice = FirstElement(root, "choices");
    ASSERT_EQ(choice["finish_reason"].get(sv), simdjson::SUCCESS);
    EXPECT_EQ(sv, "length");
}

TEST_F(HttpServiceTest, UsageBelongsToOwnRequest) {
    // Two sequential requests with different caps: each response's usage must
    // reflect ITS OWN generation (the P0 race fix checkpoints PerfStats under
    // the engine lock; a stale/raced copy would misreport one of them).
    const auto r1 = HttpDo(brpc::HTTP_METHOD_POST,
                           "/v1/chat/completions",
                           R"({"messages":[{"role":"user","content":"a"}],"max_tokens":1})");
    ASSERT_TRUE(r1.call_ok) << r1.fail_text;
    ASSERT_EQ(r1.status, 200) << r1.body;
    const auto r2 = HttpDo(brpc::HTTP_METHOD_POST,
                           "/v1/chat/completions",
                           R"({"messages":[{"role":"user","content":"a"}],"max_tokens":3})");
    ASSERT_TRUE(r2.call_ok) << r2.fail_text;
    ASSERT_EQ(r2.status, 200) << r2.body;

    // Two parses, two parsers: re-parsing one dom::parser replaces its
    // document and invalidates the elements handed out by the first parse.
    simdjson::dom::parser parser1;
    simdjson::dom::parser parser2;
    const auto root1 = ParseBody(parser1, r1.body);
    const auto root2 = ParseBody(parser2, r2.body);

    int64_t completion1 = -1, prompt1 = -1, total1 = -1;
    ASSERT_EQ(root1["usage"]["completion_tokens"].get(completion1), simdjson::SUCCESS);
    ASSERT_EQ(root1["usage"]["prompt_tokens"].get(prompt1), simdjson::SUCCESS);
    ASSERT_EQ(root1["usage"]["total_tokens"].get(total1), simdjson::SUCCESS);
    EXPECT_EQ(completion1, 1);
    EXPECT_EQ(total1, prompt1 + completion1);

    int64_t completion2 = -1, prompt2 = -1, total2 = -1;
    ASSERT_EQ(root2["usage"]["completion_tokens"].get(completion2), simdjson::SUCCESS);
    ASSERT_EQ(root2["usage"]["prompt_tokens"].get(prompt2), simdjson::SUCCESS);
    ASSERT_EQ(root2["usage"]["total_tokens"].get(total2), simdjson::SUCCESS);
    EXPECT_GE(completion2, 1);
    EXPECT_LE(completion2, 3);
    EXPECT_EQ(total2, prompt2 + completion2);

    // Same prompt in both requests -> identical prompt accounting.
    EXPECT_EQ(prompt1, prompt2);
}

TEST_F(HttpServiceTest, ChatDefaultMaxTokens) {
    // ServerConfig.default_max_tokens = 3 applies when the request omits it.
    const auto r = HttpDo(brpc::HTTP_METHOD_POST,
                          "/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":"a"}]})");
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    ASSERT_EQ(r.status, 200) << r.body;
    simdjson::dom::parser parser;
    const auto root = ParseBody(parser, r.body);
    int64_t completion_tokens = -1;
    ASSERT_EQ(root["usage"]["completion_tokens"].get(completion_tokens), simdjson::SUCCESS);
    EXPECT_GE(completion_tokens, 1);
    EXPECT_LE(completion_tokens, 3);
}

TEST_F(HttpServiceTest, ChatRejectsMultiTurnHistory) {
    const auto r = HttpDo(brpc::HTTP_METHOD_POST,
                          "/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":"hi"},
                                          {"role":"assistant","content":"hello"},
                                          {"role":"user","content":"again"}]})");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(ErrorType(r.body), "unsupported_parameter");
}

TEST_F(HttpServiceTest, ChatRejectsStreamTrue) {
    const auto r = HttpDo(brpc::HTTP_METHOD_POST,
                          "/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":"a"}],"stream":true})");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(ErrorType(r.body), "unsupported_parameter");
}

TEST_F(HttpServiceTest, ChatValidatesParams) {
    const char* bad_bodies[] = {
        R"({"messages":[{"role":"user","content":"a"}],"temperature":-0.5})",
        R"({"messages":[{"role":"user","content":"a"}],"temperature":2.5})",
        R"({"messages":[{"role":"user","content":"a"}],"top_p":0})",
        R"({"messages":[{"role":"user","content":"a"}],"top_p":1.5})",
        R"({"messages":[{"role":"user","content":"a"}],"top_k":-1})",
    };
    for (const char* body : bad_bodies) {
        const auto r = HttpDo(brpc::HTTP_METHOD_POST, "/v1/chat/completions", body);
        EXPECT_EQ(r.status, 400) << body;
        EXPECT_EQ(ErrorType(r.body), "invalid_request_error") << body;
    }
}

TEST_F(HttpServiceTest, ChatRejectsBadJsonAndMissingMessages) {
    const auto bad_json = HttpDo(brpc::HTTP_METHOD_POST, "/v1/chat/completions", "{not json");
    EXPECT_EQ(bad_json.status, 400) << bad_json.fail_text;
    EXPECT_EQ(ErrorType(bad_json.body), "invalid_request_error");

    const auto no_messages = HttpDo(brpc::HTTP_METHOD_POST, "/v1/chat/completions", "{}");
    EXPECT_EQ(no_messages.status, 400);
}

TEST_F(HttpServiceTest, BodyTooLarge) {
    // ServerConfig.max_body_bytes is 4096 in this fixture.
    const auto r =
        HttpDo(brpc::HTTP_METHOD_POST, "/v1/chat/completions", std::string(8192, ' ') + "{}");
    EXPECT_EQ(r.status, 413) << r.fail_text;
}

TEST_F(HttpServiceTest, ChatImageWithoutVisionIs503) {
    const auto r = HttpDo(brpc::HTTP_METHOD_POST,
                          "/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":[
            {"type":"text","text":"what is this?"},
            {"type":"image_url","image_url":{"url":"data:image/png;base64,QUJD"}}
        ]}]})");
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(ErrorType(r.body), "server_error");
}

TEST_F(HttpServiceTest, OcrWithoutVisionIs503) {
    const auto r = HttpDo(brpc::HTTP_METHOD_POST, "/v1/ocr", R"({"image":"QUJD"})");
    EXPECT_EQ(r.status, 503) << r.fail_text;
}

// ---------------------------------------------------------------------------
// Vision-enabled suite: tiny LM + hermetic mmproj, exercising the real image
// wire path end to end (JSON -> content-part iteration -> base64 expand ->
// decode -> splice -> multimodal prefill -> generation -> JSON response).
//
// This is the regression net for a production crash found by real-model e2e
// acceptance: the content-parts loop of ParseChatRequest iterated
// `content.get_array().value()`, a dom::array produced by the rvalue
// simdjson_result's `value() &&` — the range-for thereby referenced a handle
// inside a DEAD temporary (ASan: stack-use-after-scope), killing every
// /v1/chat/completions request that carried image parts. The fix binds the
// array to a named variable before iterating it.
// ---------------------------------------------------------------------------

// 8x8 RGBA checkerboard PNG, 46 bytes (generated hermetically).
constexpr const char* kTinyPngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAGElEQ"
                                       "VR42mNgYGD4DwI4abySQJphWJgAAAOGn2FVFEfMAAAAAElFTkSuQmCC";

class HttpVisionServiceTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto model_writer = make_tiny_model_writer();
        model_file_ = std::make_unique<TempFile>(model_writer.build(32));
        auto mmproj_writer = make_tiny_mmproj_writer();
        mmproj_file_ = std::make_unique<TempFile>(mmproj_writer.build(32));

        Engine::Options opts;
        opts.model_path = model_file_->path();
        opts.mmproj_path = mmproj_file_->path();
        opts.backend = BackendKind::kCpu;
        // Multimodal prefill requires MRoPE; the tiny llama GGUF carries no
        // <arch>.rope.mrope_section metadata, so supply it via the escape
        // hatch (all positive; must sum to head_dim/2 == 4).
        opts.mrope_section = {2, 1, 1};
        auto engine_result = Engine::Create(opts);
        ASSERT_TRUE(engine_result.ok()) << engine_result.status().message;
        engine_ = std::move(engine_result).value();
        ASSERT_TRUE(engine_->has_vision());

        ServerConfig config;
        config.model_name = "test-vision-model";
        config.default_max_tokens = 3;
        service_ = std::make_unique<MllmHttpService>(engine_.get(), config);

        server_ = std::make_unique<brpc::Server>();
        ASSERT_EQ(server_->AddService(service_.get(),
                                      brpc::SERVER_DOESNT_OWN_SERVICE,
                                      "/v1/* => default_method,"
                                      "/healthz => default_method"),
                  0);
        brpc::ServerOptions options;
        ASSERT_EQ(server_->Start("127.0.0.1:0", &options), 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(server_->listen_address().port);

        // Intentionally leaked, like HttpServiceTest::channel_.
        auto* channel = new brpc::Channel();
        brpc::ChannelOptions channel_options;
        channel_options.protocol = brpc::PROTOCOL_HTTP;
        channel_options.timeout_ms = 60000;
        ASSERT_EQ(channel->Init(base_url_.c_str(), &channel_options), 0);
        channel_ = channel;
    }

    static void TearDownTestSuite() {
        server_->Stop(0);
        server_->Join();
        server_.reset();
        service_.reset();
        engine_.reset();
        model_file_.reset();
        mmproj_file_.reset();
    }

    static HttpResult HttpDo(brpc::HttpMethod method,
                             const std::string& path,
                             const std::string& body = {}) {
        brpc::Controller cntl;
        cntl.http_request().uri() = base_url_ + path;
        cntl.http_request().set_method(method);
        if (!body.empty()) {
            cntl.request_attachment().append(body);
        }
        channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
        HttpResult r;
        r.call_ok = !cntl.Failed();
        r.fail_text = cntl.ErrorText();
        r.status = cntl.http_response().status_code();
        r.body = cntl.response_attachment().to_string();
        r.content_type = cntl.http_response().content_type();
        return r;
    }

    static std::unique_ptr<TempFile> model_file_;
    static std::unique_ptr<TempFile> mmproj_file_;
    static std::unique_ptr<Engine> engine_;
    static std::unique_ptr<MllmHttpService> service_;
    static std::unique_ptr<brpc::Server> server_;
    static brpc::Channel* channel_;
    static std::string base_url_;
};

std::unique_ptr<TempFile> HttpVisionServiceTest::model_file_;
std::unique_ptr<TempFile> HttpVisionServiceTest::mmproj_file_;
std::unique_ptr<Engine> HttpVisionServiceTest::engine_;
std::unique_ptr<MllmHttpService> HttpVisionServiceTest::service_;
std::unique_ptr<brpc::Server> HttpVisionServiceTest::server_;
brpc::Channel* HttpVisionServiceTest::channel_ = nullptr;
std::string HttpVisionServiceTest::base_url_;

TEST_F(HttpVisionServiceTest, ChatCompletionsWithImageSucceeds) {
    // Longest text part + inline image part: walks the full multimodal
    // request surface. (Pre-fix this request shape killed the server under
    // ASan: stack-use-after-scope in ParseChatRequest's content loop.)
    const std::string body = std::string(R"({"messages":[{"role":"user","content":[
        {"type":"text","text":"a"},
        {"type":"image_url","image_url":{"url":"data:image/png;base64,)") +
                             kTinyPngBase64 + R"("}}
    ]}],"max_tokens":5})";
    const auto r = HttpDo(brpc::HTTP_METHOD_POST, "/v1/chat/completions", body);
    ASSERT_TRUE(r.call_ok) << r.fail_text;
    ASSERT_EQ(r.status, 200) << r.body;

    simdjson::dom::parser parser;
    simdjson::dom::element root;
    ASSERT_EQ(parser.parse(r.body).get(root), simdjson::SUCCESS) << r.body;
    const auto choice = FirstElement(root, "choices");
    std::string_view sv;
    ASSERT_EQ(choice["message"]["role"].get(sv), simdjson::SUCCESS);
    EXPECT_EQ(sv, "assistant");
    ASSERT_EQ(choice["finish_reason"].get(sv), simdjson::SUCCESS);
    EXPECT_TRUE(sv == "stop" || sv == "length") << sv;

    int64_t prompt = -1, completion = -1, total = -1;
    ASSERT_EQ(root["usage"]["prompt_tokens"].get(prompt), simdjson::SUCCESS);
    ASSERT_EQ(root["usage"]["completion_tokens"].get(completion), simdjson::SUCCESS);
    ASSERT_EQ(root["usage"]["total_tokens"].get(total), simdjson::SUCCESS);
    EXPECT_GE(prompt, 1);
    EXPECT_GE(completion, 1);
    EXPECT_EQ(total, prompt + completion);
}

} // namespace
} // namespace pl::mllm::server
