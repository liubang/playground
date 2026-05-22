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
// Created: 2026/05/17 22:42

#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/sst/encoding.h"
#include "cpp/pl/sst/sstable_format.h"

namespace pl {

class SstableFormatTest : public ::testing::Test {};

// ==================== BlockHandle Tests ====================

TEST_F(SstableFormatTest, block_handle_encode_decode_roundtrip) {
    BlockHandle handle;
    handle.setOffset(1024);
    handle.setSize(4096);

    std::string encoded;
    handle.encodeTo(&encoded);
    EXPECT_EQ(encoded.size(), 16u);

    BlockHandle decoded;
    auto result = decoded.decodeFrom(encoded);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.offset(), 1024u);
    EXPECT_EQ(decoded.size(), 4096u);
}

TEST_F(SstableFormatTest, block_handle_decode_too_short) {
    // Less than 16 bytes should fail
    std::string short_data(15, '\0');
    BlockHandle handle;
    auto result = handle.decodeFrom(short_data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, block_handle_decode_empty) {
    std::string empty;
    BlockHandle handle;
    auto result = handle.decodeFrom(empty);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, block_handle_zero_values) {
    BlockHandle handle;
    handle.setOffset(0);
    handle.setSize(0);

    std::string encoded;
    handle.encodeTo(&encoded);

    BlockHandle decoded;
    auto result = decoded.decodeFrom(encoded);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.offset(), 0u);
    EXPECT_EQ(decoded.size(), 0u);
}

TEST_F(SstableFormatTest, block_handle_max_values) {
    BlockHandle handle;
    // Use large values just below INVALID_VALUE
    handle.setOffset(UINT64_MAX - 1);
    handle.setSize(UINT64_MAX - 1);

    std::string encoded;
    handle.encodeTo(&encoded);

    BlockHandle decoded;
    auto result = decoded.decodeFrom(encoded);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.offset(), UINT64_MAX - 1);
    EXPECT_EQ(decoded.size(), UINT64_MAX - 1);
}

// ==================== FileMeta Tests ====================

TEST_F(SstableFormatTest, file_meta_encode_decode_roundtrip) {
    FileMeta meta;
    meta.setSSTType(SSTType::MINOR);
    meta.setSSTVersion(SSTVersion::V1);
    meta.setPatchId(100);
    meta.setSSTId(200);
    meta.setFilterPolicyType(FilterPolicyType::STANDARD_BLOOM_FILTER);
    meta.setBitsPerKey(10);
    meta.setCellNum(1000);
    meta.setRowNum(500);
    meta.setMinTimestamp(111111);
    meta.setMaxTimestamp(222222);
    meta.setMinKey("aaaa");
    meta.setMaxKey("zzzz");

    std::string encoded;
    meta.encodeTo(&encoded);

    FileMeta decoded;
    auto result = decoded.decodeFrom(encoded);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.sstType(), SSTType::MINOR);
    EXPECT_EQ(decoded.sstVersion(), SSTVersion::V1);
    EXPECT_EQ(decoded.patchId(), 100u);
    EXPECT_EQ(decoded.sstId(), 200u);
    EXPECT_EQ(decoded.filterPolicyType(), FilterPolicyType::STANDARD_BLOOM_FILTER);
    EXPECT_EQ(decoded.bitsPerKey(), 10u);
    EXPECT_EQ(decoded.cellNum(), 1000u);
    EXPECT_EQ(decoded.rowNum(), 500u);
    EXPECT_EQ(decoded.minTimestamp(), 111111u);
    EXPECT_EQ(decoded.maxTimestamp(), 222222u);
    EXPECT_EQ(decoded.minKey(), "aaaa");
    EXPECT_EQ(decoded.maxKey(), "zzzz");
}

TEST_F(SstableFormatTest, file_meta_decode_too_short) {
    // FILE_META_MIN_LEN is 67
    std::string short_data(66, '\0');
    FileMeta meta;
    auto result = meta.decodeFrom(short_data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_decode_invalid_magic) {
    // Build a buffer with wrong magic number
    std::string data;
    encodeInt<uint32_t>(&data, 0xDEADBEEF); // wrong magic
    // Pad the rest to meet minimum length
    data.resize(FILE_META_MIN_LEN, '\0');

    FileMeta meta;
    auto result = meta.decodeFrom(data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_decode_invalid_sst_type) {
    std::string data;
    encodeInt<uint32_t>(&data, FILE_META_MAGIC_NUMBER); // correct magic
    encodeInt<uint8_t>(&data, 0);                       // invalid sst type (0 = NONE)
    data.resize(FILE_META_MIN_LEN, '\0');

    FileMeta meta;
    auto result = meta.decodeFrom(data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_decode_sst_type_out_of_range) {
    std::string data;
    encodeInt<uint32_t>(&data, FILE_META_MAGIC_NUMBER);
    encodeInt<uint8_t>(&data, 99); // out of range
    data.resize(FILE_META_MIN_LEN, '\0');

    FileMeta meta;
    auto result = meta.decodeFrom(data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_decode_invalid_sst_version) {
    std::string data;
    encodeInt<uint32_t>(&data, FILE_META_MAGIC_NUMBER);
    encodeInt<uint8_t>(&data, static_cast<uint8_t>(SSTType::MINOR)); // valid type
    encodeInt<uint8_t>(&data, 0);                                    // invalid version (0 = NONE)
    data.resize(FILE_META_MIN_LEN, '\0');

    FileMeta meta;
    auto result = meta.decodeFrom(data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_decode_invalid_filter_type) {
    std::string data;
    encodeInt<uint32_t>(&data, FILE_META_MAGIC_NUMBER);
    encodeInt<uint8_t>(&data, static_cast<uint8_t>(SSTType::MINOR));
    encodeInt<uint8_t>(&data, static_cast<uint8_t>(SSTVersion::V1));
    encodeInt<uint64_t>(&data, 1);   // patch_id
    encodeInt<uint64_t>(&data, 2);   // sst_id
    encodeInt<uint8_t>(&data, 0xFF); // invalid filter type
    data.resize(FILE_META_MIN_LEN, '\0');

    FileMeta meta;
    auto result = meta.decodeFrom(data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_encode_decode_empty_keys) {
    FileMeta meta;
    meta.setSSTType(SSTType::MEMORY);
    meta.setSSTVersion(SSTVersion::V1);
    meta.setPatchId(1);
    meta.setSSTId(2);
    meta.setFilterPolicyType(FilterPolicyType::NONE);
    meta.setBitsPerKey(0);
    meta.setCellNum(0);
    meta.setRowNum(0);
    meta.setMinTimestamp(0);
    meta.setMaxTimestamp(0);
    meta.setMinKey("");
    meta.setMaxKey("");

    std::string encoded;
    meta.encodeTo(&encoded);

    FileMeta decoded;
    auto result = decoded.decodeFrom(encoded);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.minKey(), "");
    EXPECT_EQ(decoded.maxKey(), "");
}

TEST_F(SstableFormatTest, file_meta_decode_truncated_min_key) {
    // Build a valid header but truncate at min_key
    FileMeta meta;
    meta.setSSTType(SSTType::MINOR);
    meta.setSSTVersion(SSTVersion::V1);
    meta.setPatchId(1);
    meta.setSSTId(2);
    meta.setFilterPolicyType(FilterPolicyType::STANDARD_BLOOM_FILTER);
    meta.setBitsPerKey(10);
    meta.setCellNum(100);
    meta.setRowNum(50);
    meta.setMinTimestamp(1000);
    meta.setMaxTimestamp(2000);
    meta.setMinKey("long_min_key_value");
    meta.setMaxKey("long_max_key_value");

    std::string encoded;
    meta.encodeTo(&encoded);

    // Truncate in the middle of min_key data
    // The header fields + min_key_size are at offset 59 (4+1+1+8+8+1+4+8+8+8+8+4 = 63)
    // min key starts at offset 63, so cut a few bytes before the end
    std::string truncated = encoded.substr(0, 65); // cuts min_key short

    FileMeta decoded;
    auto result = decoded.decodeFrom(truncated);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, file_meta_decode_max_key_size_mismatch) {
    // Build valid encoded data, then append extra bytes to cause size mismatch
    FileMeta meta;
    meta.setSSTType(SSTType::MINOR);
    meta.setSSTVersion(SSTVersion::V1);
    meta.setPatchId(1);
    meta.setSSTId(2);
    meta.setFilterPolicyType(FilterPolicyType::STANDARD_BLOOM_FILTER);
    meta.setBitsPerKey(10);
    meta.setCellNum(100);
    meta.setRowNum(50);
    meta.setMinTimestamp(1000);
    meta.setMaxTimestamp(2000);
    meta.setMinKey("aaa");
    meta.setMaxKey("bbb");

    std::string encoded;
    meta.encodeTo(&encoded);

    // Append extra garbage bytes to cause cursor + max_key_size != total_size
    encoded.append("extra_garbage");

    FileMeta decoded;
    auto result = decoded.decodeFrom(encoded);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

// ==================== Footer Tests ====================

TEST_F(SstableFormatTest, footer_encode_decode_roundtrip) {
    Footer footer;
    BlockHandle filter_handle, index_handle, meta_handle;
    filter_handle.setOffset(0);
    filter_handle.setSize(100);
    index_handle.setOffset(100);
    index_handle.setSize(200);
    meta_handle.setOffset(300);
    meta_handle.setSize(67);

    footer.setFilterHandle(filter_handle);
    footer.setIndexHandle(index_handle);
    footer.setFileMetaHandle(meta_handle);

    std::string encoded;
    footer.encodeTo(&encoded);
    EXPECT_EQ(encoded.size(), FOOTER_LEN);

    Footer decoded;
    auto result = decoded.decodeFrom(encoded);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.filterHandle().offset(), 0u);
    EXPECT_EQ(decoded.filterHandle().size(), 100u);
    EXPECT_EQ(decoded.indexHandle().offset(), 100u);
    EXPECT_EQ(decoded.indexHandle().size(), 200u);
    EXPECT_EQ(decoded.fileMetaHandle().offset(), 300u);
    EXPECT_EQ(decoded.fileMetaHandle().size(), 67u);
}

TEST_F(SstableFormatTest, footer_decode_too_short) {
    std::string short_data(FOOTER_LEN - 1, '\0');
    Footer footer;
    auto result = footer.decodeFrom(short_data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, footer_decode_invalid_magic) {
    // Build a 60-byte buffer with wrong magic at the end
    std::string data(FOOTER_LEN, '\0');
    // Set last 4 bytes to wrong magic
    uint32_t bad_magic = 0xDEADBEEF;
    std::memcpy(data.data() + FOOTER_LEN - 4, &bad_magic, 4);

    Footer footer;
    auto result = footer.decodeFrom(data);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

TEST_F(SstableFormatTest, footer_decode_empty) {
    std::string empty;
    Footer footer;
    auto result = footer.decodeFrom(empty);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kDataCorruption);
}

} // namespace pl
