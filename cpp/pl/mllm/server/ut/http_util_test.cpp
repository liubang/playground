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

#include <gtest/gtest.h>
#include <simdjson.h>
#include <string>
#include <vector>

#include "cpp/pl/mllm/server/http_util.h"

namespace pl::mllm::server {
namespace {

// Keep the parser alive as long as the returned element is used.
simdjson::dom::element ParseJson(simdjson::dom::parser& parser, std::string_view json) {
    simdjson::dom::element root;
    if (parser.parse(json).get(root) != simdjson::SUCCESS) {
        ADD_FAILURE() << "fixture JSON did not parse: " << json;
        return {};
    }
    return root;
}

ServerConfig TestConfig() {
    ServerConfig config;
    config.default_max_tokens = 5;
    config.max_images_per_request = 2;
    return config;
}

// ---------------------------------------------------------------------------
// Base64Decode
// ---------------------------------------------------------------------------

TEST(Base64DecodeTest, DecodesPlainAndPadded) {
    std::vector<uint8_t> out;
    ASSERT_TRUE(Base64Decode("QUJD", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B', 'C'}));

    ASSERT_TRUE(Base64Decode("QUI=", &out)); // "AB"
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B'}));

    ASSERT_TRUE(Base64Decode("QQ==", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A'}));

    // 0xFB 0xFF 0xFE exercises '+' and '/' both.
    ASSERT_TRUE(Base64Decode("+//+", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{0xFB, 0xFF, 0xFE}));
}

TEST(Base64DecodeTest, ToleratesWhitespace) {
    std::vector<uint8_t> out;
    ASSERT_TRUE(Base64Decode("Q U\r\nJ\tD ", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B', 'C'}));
}

TEST(Base64DecodeTest, EmptyInputIsEmptyOutput) {
    std::vector<uint8_t> out{'x'};
    ASSERT_TRUE(Base64Decode("", &out));
    EXPECT_TRUE(out.empty());
}

TEST(Base64DecodeTest, RejectsInvalidCharacters) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(Base64Decode("QUJ$", &out));
    EXPECT_FALSE(Base64Decode("QUJ", &out));    // non-canonical zero-padding tail
    EXPECT_FALSE(Base64Decode("Q", &out));      // truncated quantum (6 leftover bits)
    EXPECT_FALSE(Base64Decode("QQ= =A", &out)); // payload after padding
    EXPECT_FALSE(Base64Decode("QUE=QUE=", &out));
}

TEST(Base64DecodeTest, RejectsNonCanonicalPaddingBits) {
    std::vector<uint8_t> out;
    // "/w==" is the canonical encoding of 0xFF; "/x==" keeps the same value
    // but wastes one tail bit set — non-canonical and rejected strictly.
    ASSERT_TRUE(Base64Decode("/w==", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{0xFF}));
    EXPECT_FALSE(Base64Decode("/x==", &out));
}

TEST(Base64DecodeTest, AcceptsPaddingFollowedByWhitespace) {
    std::vector<uint8_t> out;
    ASSERT_TRUE(Base64Decode("QUI= \n", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B'}));
}

// ---------------------------------------------------------------------------
// DecodeImagePayload
// ---------------------------------------------------------------------------

TEST(DecodeImagePayloadTest, RawAndDataUrl) {
    std::vector<uint8_t> out;
    ASSERT_TRUE(DecodeImagePayload("QUJD", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B', 'C'}));

    ASSERT_TRUE(DecodeImagePayload("data:image/png;base64,QUJD", &out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B', 'C'}));
}

TEST(DecodeImagePayloadTest, RejectsMalformedDataUrl) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(DecodeImagePayload("data:image/png;base64", &out)); // no comma
    EXPECT_FALSE(DecodeImagePayload("data:,QUJ$", &out));            // bad payload
}

// ---------------------------------------------------------------------------
// ExpandOcrTemplate
// ---------------------------------------------------------------------------

TEST(ExpandOcrTemplateTest, ExpandsBothMarkersPreservingOrder) {
    const std::string out =
        ExpandOcrTemplate("<|begin_of_sentence|>User: {IMAGE}{TASK}\nAssistant:\n",
                          "<|IMAGE_START|><|IMAGE_PLACEHOLDER|><|IMAGE_END|>",
                          "OCR:");
    EXPECT_EQ(out,
              "<|begin_of_sentence|>User: "
              "<|IMAGE_START|><|IMAGE_PLACEHOLDER|><|IMAGE_END|>OCR:\n"
              "Assistant:\n");
}

TEST(ExpandOcrTemplateTest, ExpandsRepeatedMarkers) {
    EXPECT_EQ(ExpandOcrTemplate("{TASK} and {TASK}", "S", "T"), "T and T");
}

TEST(ExpandOcrTemplateTest, SubstitutedValueIsNeverReexpanded) {
    // A task that literally contains "{TASK}" must come through verbatim and
    // must not trigger further (or endlessly recursive) substitution.
    EXPECT_EQ(ExpandOcrTemplate("User: {TASK}", "S", "{TASK} please"), "User: {TASK} please");
}

TEST(ExpandOcrTemplateTest, NoMarkersIsIdentity) {
    EXPECT_EQ(ExpandOcrTemplate("plain prompt", "S", "T"), "plain prompt");
}

// ---------------------------------------------------------------------------
// ParseOcrRequest
// ---------------------------------------------------------------------------

TEST(ParseOcrRequestTest, RejectsMissingImage) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({"max_tokens": 7})");
    const auto result = ParseOcrRequest(root, TestConfig());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kInvalidArgument);
}

TEST(ParseOcrRequestTest, DefaultsAndOverrides) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({"image": "QUJD"})");
    const auto result = ParseOcrRequest(root, TestConfig());
    ASSERT_TRUE(result.ok()) << result.status().message;
    EXPECT_EQ(result.value().image_payload, "QUJD");
    EXPECT_EQ(result.value().task, "OCR:");
    EXPECT_EQ(result.value().max_tokens, 5); // from config

    const auto root2 =
        ParseJson(parser, R"({"image": "QUE=", "prompt": "Table:", "max_tokens": 9})");
    const auto result2 = ParseOcrRequest(root2, TestConfig());
    ASSERT_TRUE(result2.ok()) << result2.status().message;
    EXPECT_EQ(result2.value().task, "Table:");
    EXPECT_EQ(result2.value().max_tokens, 9);
}

TEST(ParseOcrRequestTest, ClampsMaxTokens) {
    simdjson::dom::parser parser;
    const auto small = ParseJson(parser, R"({"image": "QQ==", "max_tokens": 0})");
    const auto small_result = ParseOcrRequest(small, TestConfig());
    ASSERT_TRUE(small_result.ok());
    EXPECT_EQ(small_result.value().max_tokens, 1);

    const auto huge = ParseJson(parser, R"({"image": "QQ==", "max_tokens": 999999})");
    const auto huge_result = ParseOcrRequest(huge, TestConfig());
    ASSERT_TRUE(huge_result.ok());
    EXPECT_EQ(huge_result.value().max_tokens, kMaxTokensCap);
}

// ---------------------------------------------------------------------------
// ParseChatRequest
// ---------------------------------------------------------------------------

TEST(ParseChatRequestTest, RejectsMissingMessages) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({})");
    const auto result = ParseChatRequest(root, TestConfig());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kInvalidArgument);
}

TEST(ParseChatRequestTest, RejectsEmptyContent) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({"messages": []})");
    const auto result = ParseChatRequest(root, TestConfig());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kInvalidArgument);
}

TEST(ParseChatRequestTest, CollapsesSystemAndUserTurns) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({
        "messages": [
            {"role": "system", "content": "You are terse."},
            {"role": "system", "content": "Answer in Chinese."},
            {"role": "user", "content": "first"},
            {"role": "user", "content": [{"type": "text", "text": "second"}]}
        ]
    })");
    const auto result = ParseChatRequest(root, TestConfig());
    ASSERT_TRUE(result.ok()) << result.status().message;
    EXPECT_EQ(result.value().system, "You are terse.\nAnswer in Chinese.");
    EXPECT_EQ(result.value().user_text, "first\nsecond");
    EXPECT_TRUE(result.value().image_payloads.empty());
}

TEST(ParseChatRequestTest, CollectsImagePayloadsWithoutDecoding) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({
        "messages": [{"role": "user", "content": [
            {"type": "text", "text": "what is this?"},
            {"type": "image_url", "image_url": {"url": "data:image/png;base64,QUJD"}},
            {"type": "image_url", "image_url": "QUE="}
        ]}]
    })");
    const auto result = ParseChatRequest(root, TestConfig());
    ASSERT_TRUE(result.ok()) << result.status().message;
    EXPECT_EQ(result.value().user_text, "what is this?");
    ASSERT_EQ(result.value().image_payloads.size(), 2u);
    EXPECT_EQ(result.value().image_payloads[0], "data:image/png;base64,QUJD");
    EXPECT_EQ(result.value().image_payloads[1], "QUE=");
}

TEST(ParseChatRequestTest, EnforcesImageCountCap) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({
        "messages": [{"role": "user", "content": [
            {"type": "image_url", "image_url": "QUE="},
            {"type": "image_url", "image_url": "QUE="},
            {"type": "image_url", "image_url": "QUE="}
        ]}]
    })");
    const auto result = ParseChatRequest(root, TestConfig()); // cap is 2
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kInvalidArgument);
}

TEST(ParseChatRequestTest, RejectsMultiTurnHistory) {
    simdjson::dom::parser parser;
    const auto root = ParseJson(parser, R"({
        "messages": [
            {"role": "user", "content": "hi"},
            {"role": "assistant", "content": "hello"},
            {"role": "user", "content": "again"}
        ]
    })");
    const auto result = ParseChatRequest(root, TestConfig());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kUnsupported);

    const auto tool_root = ParseJson(parser,
                                     R"({"messages": [
            {"role": "user", "content": "hi"},
            {"role": "tool", "content": "42"}
        ]})");
    const auto tool_result = ParseChatRequest(tool_root, TestConfig());
    ASSERT_FALSE(tool_result.ok());
    EXPECT_EQ(tool_result.status().code, ErrorCode::kUnsupported);
}

TEST(ParseChatRequestTest, RejectsStreamTrue) {
    simdjson::dom::parser parser;
    const auto root =
        ParseJson(parser, R"({"messages": [{"role": "user", "content": "a"}], "stream": true})");
    const auto result = ParseChatRequest(root, TestConfig());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code, ErrorCode::kUnsupported);
}

TEST(ParseChatRequestTest, ValidatesSamplingParams) {
    simdjson::dom::parser parser;
    ServerConfig config = TestConfig();
    const auto parse_ok = [&](std::string_view json) {
        return ParseChatRequest(ParseJson(parser, json), config);
    };

    EXPECT_FALSE(
        parse_ok(R"({"messages": [{"role": "user", "content": "a"}], "temperature": -0.5})").ok());
    EXPECT_FALSE(
        parse_ok(R"({"messages": [{"role": "user", "content": "a"}], "temperature": 2.5})").ok());
    EXPECT_FALSE(parse_ok(R"({"messages": [{"role": "user", "content": "a"}], "top_p": 0})").ok());
    EXPECT_FALSE(
        parse_ok(R"({"messages": [{"role": "user", "content": "a"}], "top_p": 1.01})").ok());
    EXPECT_FALSE(parse_ok(R"({"messages": [{"role": "user", "content": "a"}], "top_k": -1})").ok());

    const auto good = parse_ok(R"({
        "messages": [{"role": "user", "content": "a"}],
        "temperature": 0.7, "top_p": 0.9, "top_k": 40, "seed": 1234
    })");
    ASSERT_TRUE(good.ok()) << good.status().message;
    EXPECT_FLOAT_EQ(good.value().params.temperature, 0.7f);
    EXPECT_FLOAT_EQ(good.value().params.top_p, 0.9f);
    EXPECT_EQ(good.value().params.top_k, 40);
    EXPECT_EQ(good.value().params.seed, 1234u);

    // Integer temperature is accepted (OpenAI clients send "temperature": 1).
    const auto int_temp =
        parse_ok(R"({"messages": [{"role": "user", "content": "a"}], "temperature": 1})");
    ASSERT_TRUE(int_temp.ok()) << int_temp.status().message;
    EXPECT_FLOAT_EQ(int_temp.value().params.temperature, 1.0f);
}

TEST(ParseChatRequestTest, MaxTokensAliasAndClamp) {
    simdjson::dom::parser parser;
    const auto alias =
        ParseChatRequest(ParseJson(parser,
                                   R"({"messages": [{"role": "user", "content": "a"}],
                      "max_completion_tokens": 7})"),
                         TestConfig());
    ASSERT_TRUE(alias.ok()) << alias.status().message;
    EXPECT_EQ(alias.value().params.max_tokens, 7);

    const auto clamped =
        ParseChatRequest(ParseJson(parser,
                                   R"({"messages": [{"role": "user", "content": "a"}],
                      "max_tokens": 10, "max_completion_tokens": 3})"),
                         TestConfig());
    ASSERT_TRUE(clamped.ok());
    EXPECT_EQ(clamped.value().params.max_tokens, 10); // max_tokens wins over the alias

    const auto defaulted = ParseChatRequest(
        ParseJson(parser, R"({"messages": [{"role": "user", "content": "a"}]})"), TestConfig());
    ASSERT_TRUE(defaulted.ok());
    EXPECT_EQ(defaulted.value().params.max_tokens, 5);
}

// ---------------------------------------------------------------------------
// FinishReasonFor
// ---------------------------------------------------------------------------

TEST(FinishReasonForTest, LengthIffCapHit) {
    PerfStats perf;
    perf.generated_tokens = 5;
    EXPECT_STREQ(FinishReasonFor(perf, 10), "stop");
    perf.generated_tokens = 10;
    EXPECT_STREQ(FinishReasonFor(perf, 10), "length");
    perf.generated_tokens = 0;
    EXPECT_STREQ(FinishReasonFor(perf, 0), "stop");
}

} // namespace
} // namespace pl::mllm::server
