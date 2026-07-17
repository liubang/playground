// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {

struct Timestamp {
    uint64_t domain_epoch = 0;
    uint64_t counter = 0;

    auto operator<=>(const Timestamp&) const = default;
};

enum class PartitionMode : uint8_t {
    kHash = 1,
    kGlobalOrder = 2,
};

enum class HashAlgorithm : uint8_t {
    kNone = 0,
    kXxh3_64V1 = 1,
};

struct StaticQualifier {
    uint32_t column_id = 0;
    bool operator==(const StaticQualifier&) const = default;
};

struct DynamicQualifier {
    std::string value;
    bool operator==(const DynamicQualifier&) const = default;
};

using Qualifier = std::variant<StaticQualifier, DynamicQualifier>;

struct RowTombstone {
    bool operator==(const RowTombstone&) const = default;
};

struct ColumnFamilyTombstone {
    uint32_t column_family_id = 0;
    bool operator==(const ColumnFamilyTombstone&) const = default;
};

struct CellRef {
    uint32_t column_family_id = 0;
    Qualifier qualifier;
    bool operator==(const CellRef&) const = default;
};

using RecordTarget = std::variant<RowTombstone, ColumnFamilyTombstone, CellRef>;

struct GlobalOrderPrefix {
    bool operator==(const GlobalOrderPrefix&) const = default;
};

struct HashPrefix {
    uint32_t virtual_bucket_id;
    bool operator==(const HashPrefix&) const = default;
};

using PartitionPrefix = std::variant<GlobalOrderPrefix, HashPrefix>;

struct StorageKey {
    PartitionPrefix partition;
    std::vector<sstv2::types::Value> row_key;
    RecordTarget target;
};

struct VersionedStorageKey {
    StorageKey storage_key;
    Timestamp commit_ts;
    uint32_t mutation_seq = 0;
    sstv2::types::OpType op_type = sstv2::types::OpType::kPut;
};

} // namespace pl::minitable
