// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/variant.h"

namespace pl::sstv2::types {

enum class SortOrder : uint8_t {
    kAscending = 0,
    kDescending = 1,
};

struct KeyColumn {
    std::string name;
    DataType type = DataType::kNone;
    SortOrder order = SortOrder::kAscending;
};

class StructuredRowKey {
public:
    StructuredRowKey() = default;
    explicit StructuredRowKey(std::vector<Variant> values) : values_(std::move(values)) {}

    [[nodiscard]] size_t size() const { return values_.size(); }
    [[nodiscard]] bool empty() const { return values_.empty(); }
    [[nodiscard]] const Variant& operator[](size_t index) const { return values_[index]; }
    [[nodiscard]] const std::vector<Variant>& values() const { return values_; }

private:
    std::vector<Variant> values_;
};

// Strong external schema from the PDF: row-key columns are user-defined, system
// key columns are fixed, and there is exactly one logical Value column.
class TableSchema {
public:
    explicit TableSchema(std::vector<KeyColumn> row_key_columns = {})
        : row_key_columns_(std::move(row_key_columns)) {}

    [[nodiscard]] const std::vector<KeyColumn>& row_key_columns() const { return row_key_columns_; }
    [[nodiscard]] size_t num_row_key_columns() const { return row_key_columns_.size(); }
    [[nodiscard]] const KeyColumn& row_key_column(size_t index) const {
        return row_key_columns_[index];
    }

private:
    std::vector<KeyColumn> row_key_columns_;
};

struct Row {
    StructuredRowKey row_key;
    uint64_t version = 0;
    uint8_t op_type = 0;
    Variant value = Variant::none();
};

} // namespace pl::sstv2::types
