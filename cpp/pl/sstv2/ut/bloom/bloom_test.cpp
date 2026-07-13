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
// Created: 2026/06/05 22:09

#include <gtest/gtest.h>
#include <memory>

#include "cpp/pl/sstv2/bloom/bloom.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/types/row.h"

namespace pl::sstv2::bloom {
namespace {

using types::DataType;
using types::InternalRow;
using types::InternalSchema;
using types::OpType;
using types::Schema;
using types::SchemaBuilder;
using types::Value;
using types::Version;

Schema::ConstRef make_schema() {
    auto schema = SchemaBuilder().add_column("k", DataType::kString).build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

InternalRow make_row(InternalSchema::ConstRef schema, std::string key, uint64_t version) {
    auto row = InternalRow::make(schema);
    row.columns[0] = Value::make<DataType::kString>(std::move(key));
    row.columns[schema->version_index()] =
        Value::make<DataType::kVersion>(Version{.major = version, .minor = 0});
    row.columns[schema->op_type_index()] =
        Value::make<DataType::kUint8>(static_cast<uint8_t>(OpType::kPut));
    row.columns[schema->flag_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->filename_index()] = Value::make<DataType::kString>("@1");
    row.columns[schema->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->length_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    return row;
}

TEST(BloomTest, RoundTripMayContainInsertedRows) {
    auto schema = InternalSchema::make(make_schema());
    Builder builder(10);
    const auto row = make_row(schema, "alpha", 9);
    ASSERT_TRUE(builder.add(row, schema).ok());

    const std::string section = builder.finish();
    auto reader = Reader::open(section);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->header().magic, Header::kMagic);
    EXPECT_EQ(reader->header().row_count, 1u);

    auto may_contain = reader->may_contain(row, schema);
    ASSERT_TRUE(may_contain.ok()) << may_contain.status();
    EXPECT_TRUE(*may_contain);
}

TEST(BloomTest, RejectsInvalidParameters) {
    std::string section;
    codec::append_fixed32(&section, Header::kMagic);
    codec::append_fixed32(&section, Header::kVersion);
    codec::append_fixed32(&section, 1);
    codec::append_fixed64(&section, 0);
    codec::append_fixed64(&section, 0);
    codec::append_fixed64(&section, 0);
    EXPECT_FALSE(Reader::open(section).ok());
}

TEST(BloomTest, RejectsCorruptChecksum) {
    Builder builder(10);
    ASSERT_TRUE(
        builder.add_all_key(types::EncodedAllKey::from_encoded_bytes(std::string("abc"))).ok());
    std::string section = builder.finish();
    section.back() ^= 0x01;

    auto reader = Reader::open(section);
    EXPECT_FALSE(reader.ok());
}

} // namespace
} // namespace pl::sstv2::bloom
