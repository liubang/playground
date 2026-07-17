// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#include "cpp/pl/minitable/core/embedded_slice.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::minitable {
namespace {

using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Value;

struct CellRefLess {
    bool operator()(const CellRef& lhs, const CellRef& rhs) const {
        if (lhs.column_family_id != rhs.column_family_id) {
            return lhs.column_family_id < rhs.column_family_id;
        }
        if (lhs.qualifier.index() != rhs.qualifier.index()) {
            return lhs.qualifier.index() < rhs.qualifier.index();
        }
        if (const auto* left = std::get_if<StaticQualifier>(&lhs.qualifier)) {
            return left->column_id < std::get<StaticQualifier>(rhs.qualifier).column_id;
        }
        return std::get<DynamicQualifier>(lhs.qualifier).value <
               std::get<DynamicQualifier>(rhs.qualifier).value;
    }
};

struct VersionStamp {
    Timestamp commit_ts;
    uint32_t mutation_seq = 0;

    auto operator<=>(const VersionStamp&) const = default;
};

struct RowAccumulator {
    std::vector<Value> row_key;
    std::optional<VersionStamp> row_delete;
    std::map<uint32_t, VersionStamp> family_deletes;
    std::map<CellRef, std::optional<VisibleCell>, CellRefLess> cells;
};

bool IsVisible(Timestamp commit_ts, Timestamp read_ts) {
    return commit_ts <= read_ts;
}

void RaiseDelete(std::optional<VersionStamp>* current, VersionStamp value) {
    if (!current->has_value() || **current < value) {
        *current = value;
    }
}

absl::StatusOr<std::string> EncodeCellValue(const Value& value) {
    std::string encoded;
    encoded.push_back(static_cast<char>(value.type()));
    auto status = sstv2::types::encode_value(value, &encoded);
    if (!status.ok()) {
        return status;
    }
    return encoded;
}

absl::StatusOr<Value> DecodeCellValue(std::string_view encoded) {
    if (encoded.empty()) {
        return absl::DataLossError("Put entry has an empty value envelope");
    }
    const auto type = static_cast<DataType>(static_cast<uint8_t>(encoded.front()));
    if (static_cast<uint8_t>(type) > static_cast<uint8_t>(DataType::kMap)) {
        return absl::DataLossError("Put entry has an invalid value type");
    }
    auto value = sstv2::types::decode_value(type, encoded.substr(1));
    if (!value.ok()) {
        return absl::DataLossError(absl::StrCat("invalid persisted Cell value: ", value.status()));
    }
    return value;
}

absl::Status ValidateOptions(const codec::CellKeyCodec& codec,
                             const EmbeddedSliceOptions& options) {
    if (options.locality_group_id == 0) {
        return absl::InvalidArgumentError("locality group ID zero is reserved");
    }
    if (options.timestamp_domain_epoch == 0) {
        return absl::InvalidArgumentError("timestamp domain epoch zero is reserved");
    }
    if (codec.format().partition_mode != PartitionMode::kGlobalOrder) {
        return absl::UnimplementedError("Phase 1A EmbeddedSlice supports GLOBAL_ORDER only");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> EncodeRowStart(const codec::CellKeyCodec& codec,
                                           const std::vector<Value>& row_key) {
    return codec.EncodeStorageKey(
        StorageKey{.partition = GlobalOrderPrefix{}, .row_key = row_key, .target = RowTombstone{}});
}

} // namespace

absl::StatusOr<std::unique_ptr<EmbeddedSlice>> EmbeddedSlice::Create(codec::CellKeyCodec codec,
                                                                     EmbeddedSliceOptions options) {
    auto status = ValidateOptions(codec, options);
    if (!status.ok()) {
        return status;
    }
    auto store =
        SliceStore::Create({{options.locality_group_id, options.memtable}}, options.persistence);
    if (!store.ok()) {
        return store.status();
    }
    return std::unique_ptr<EmbeddedSlice>(
        new EmbeddedSlice(std::move(codec), std::move(options), std::move(*store), 0));
}

absl::StatusOr<std::unique_ptr<EmbeddedSlice>> EmbeddedSlice::Reopen(
    codec::CellKeyCodec codec, EmbeddedSliceRecovery recovery) {
    auto status = ValidateOptions(codec, recovery.options);
    if (!status.ok()) {
        return status;
    }
    auto store = SliceStore::Reopen(
        {.locality_groups = {{recovery.options.locality_group_id, recovery.options.memtable}},
         .persistence = recovery.options.persistence,
         .manifest = recovery.manifest});
    if (!store.ok()) {
        return store.status();
    }

    uint64_t high_watermark = 0;
    auto cursor = (*store)->read_view().new_cursor(recovery.options.locality_group_id);
    if (!cursor.ok()) {
        return cursor.status();
    }
    status = (*cursor)->seek_to_first();
    if (!status.ok()) {
        return status;
    }
    while ((*cursor)->valid()) {
        auto key = codec.DecodeVersionedStorageKey((*cursor)->key());
        if (!key.ok()) {
            return key.status();
        }
        if (key->commit_ts.domain_epoch != recovery.options.timestamp_domain_epoch) {
            return absl::FailedPreconditionError(
                "persisted key belongs to another timestamp domain");
        }
        high_watermark = std::max(high_watermark, key->commit_ts.counter);
        status = (*cursor)->next();
        if (!status.ok()) {
            return status;
        }
    }
    return std::unique_ptr<EmbeddedSlice>(new EmbeddedSlice(
        std::move(codec), std::move(recovery.options), std::move(*store), high_watermark));
}

absl::StatusOr<Timestamp> EmbeddedSlice::mutate(const std::vector<Value>& row_key,
                                                std::span<const CellMutation> mutations) {
    const std::lock_guard lock(writer_mutex_);
    if (mutations.empty()) {
        return absl::InvalidArgumentError("mutation batch is empty");
    }
    if (last_timestamp_counter_ == std::numeric_limits<uint64_t>::max()) {
        return absl::OutOfRangeError("Slice-local timestamp counter is exhausted");
    }
    const Timestamp commit_ts{.domain_epoch = options_.timestamp_domain_epoch,
                              .counter = last_timestamp_counter_ + 1};

    try {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        keys.reserve(mutations.size());
        values.reserve(mutations.size());
        for (size_t i = 0; i < mutations.size(); ++i) {
            if (i > std::numeric_limits<uint32_t>::max()) {
                return absl::ResourceExhaustedError("too many mutations in one row transaction");
            }
            const auto& mutation = mutations[i];
            if (mutation.op_type == OpType::kMerge) {
                return absl::UnimplementedError("Merge belongs to Phase 2B");
            }
            if (mutation.op_type != OpType::kPut && mutation.op_type != OpType::kDelete) {
                return absl::InvalidArgumentError("unknown mutation operation");
            }
            if (mutation.op_type == OpType::kPut) {
                if (!std::holds_alternative<CellRef>(mutation.target) ||
                    !mutation.value.has_value()) {
                    return absl::InvalidArgumentError("Put requires a CellRef and a value");
                }
                auto value = EncodeCellValue(*mutation.value);
                if (!value.ok()) {
                    return value.status();
                }
                values.push_back(std::move(*value));
            } else {
                if (mutation.value.has_value()) {
                    return absl::InvalidArgumentError("Delete must not carry a value");
                }
                // Keep the generic SST payload non-empty. The operation type is authoritative;
                // this sentinel is never decoded as a user value.
                values.emplace_back(1, static_cast<char>(DataType::kNone));
            }
            VersionedStorageKey key{.storage_key = {.partition = GlobalOrderPrefix{},
                                                    .row_key = row_key,
                                                    .target = mutation.target},
                                    .commit_ts = commit_ts,
                                    .mutation_seq = static_cast<uint32_t>(i),
                                    .op_type = mutation.op_type};
            auto encoded_key = codec_.EncodeVersionedStorageKey(key);
            if (!encoded_key.ok()) {
                return encoded_key.status();
            }
            keys.push_back(std::move(*encoded_key));
        }

        std::vector<MemTableMutation> batch;
        batch.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            batch.push_back({.encoded_key = keys[i], .encoded_value = values[i]});
        }
        const LocalityGroupPatch patch{.locality_group_id = options_.locality_group_id,
                                       .mutations = batch};
        auto status =
            store_->apply(std::span<const LocalityGroupPatch>(&patch, 1), commit_ts.counter);
        if (!status.ok()) {
            return status;
        }
        last_timestamp_counter_ = commit_ts.counter;
        return commit_ts;
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("mutation allocation failed");
    }
}

absl::StatusOr<std::optional<VisibleRow>> EmbeddedSlice::get(const std::vector<Value>& row_key,
                                                             Timestamp read_ts) const {
    auto start = EncodeRowStart(codec_, row_key);
    if (!start.ok()) {
        return start.status();
    }
    auto rows = scan_encoded(*start, std::nullopt, read_ts, *start);
    if (!rows.ok()) {
        return rows.status();
    }
    if (rows->empty() || rows->front().row_key != row_key) {
        return std::nullopt;
    }
    return std::move(rows->front());
}

absl::StatusOr<std::vector<VisibleRow>> EmbeddedSlice::scan(
    const std::optional<std::vector<Value>>& start_row_key,
    const std::optional<std::vector<Value>>& end_row_key,
    Timestamp read_ts) const {
    std::optional<std::string> start;
    std::optional<std::string> end;
    if (start_row_key.has_value()) {
        auto encoded = EncodeRowStart(codec_, *start_row_key);
        if (!encoded.ok()) {
            return encoded.status();
        }
        start = std::move(*encoded);
    }
    if (end_row_key.has_value()) {
        auto encoded = EncodeRowStart(codec_, *end_row_key);
        if (!encoded.ok()) {
            return encoded.status();
        }
        end = std::move(*encoded);
    }
    if (start.has_value() && end.has_value() && *start > *end) {
        return absl::InvalidArgumentError("scan start row is after end row");
    }
    return scan_encoded(std::move(start), std::move(end), read_ts);
}

absl::StatusOr<std::vector<VisibleRow>> EmbeddedSlice::scan_encoded(
    std::optional<std::string> start,
    std::optional<std::string> end,
    Timestamp read_ts,
    std::optional<std::string_view> exact_row) const {
    if (read_ts.domain_epoch != options_.timestamp_domain_epoch) {
        return absl::InvalidArgumentError("read timestamp belongs to another Slice domain");
    }
    try {
        auto cursor = store_->read_view().new_cursor(options_.locality_group_id);
        if (!cursor.ok()) {
            return cursor.status();
        }
        auto status = start.has_value() ? (*cursor)->seek(*start) : (*cursor)->seek_to_first();
        if (!status.ok()) {
            return status;
        }

        std::map<std::string, RowAccumulator> rows;
        while ((*cursor)->valid()) {
            auto decoded = codec_.DecodeVersionedStorageKey((*cursor)->key());
            if (!decoded.ok()) {
                return decoded.status();
            }
            auto row_start = EncodeRowStart(codec_, decoded->storage_key.row_key);
            if (!row_start.ok()) {
                return row_start.status();
            }
            if ((exact_row.has_value() && *row_start != *exact_row) ||
                (end.has_value() && *row_start >= *end)) {
                break;
            }
            if (decoded->commit_ts.domain_epoch != options_.timestamp_domain_epoch) {
                return absl::DataLossError("stored key belongs to another timestamp domain");
            }
            if (IsVisible(decoded->commit_ts, read_ts)) {
                auto [it, inserted] = rows.try_emplace(*row_start);
                if (inserted) {
                    it->second.row_key = decoded->storage_key.row_key;
                }
                auto& row = it->second;
                if (std::holds_alternative<RowTombstone>(decoded->storage_key.target)) {
                    RaiseDelete(
                        &row.row_delete,
                        {.commit_ts = decoded->commit_ts, .mutation_seq = decoded->mutation_seq});
                } else if (const auto* family =
                               std::get_if<ColumnFamilyTombstone>(&decoded->storage_key.target)) {
                    const VersionStamp stamp{.commit_ts = decoded->commit_ts,
                                             .mutation_seq = decoded->mutation_seq};
                    auto family_it = row.family_deletes.find(family->column_family_id);
                    if (family_it == row.family_deletes.end() || family_it->second < stamp) {
                        row.family_deletes[family->column_family_id] = stamp;
                    }
                } else {
                    const auto& ref = std::get<CellRef>(decoded->storage_key.target);
                    if (!row.cells.contains(ref)) {
                        if (decoded->op_type == OpType::kPut) {
                            auto value = DecodeCellValue((*cursor)->value());
                            if (!value.ok()) {
                                return value.status();
                            }
                            row.cells.emplace(ref,
                                              VisibleCell{.ref = ref,
                                                          .commit_ts = decoded->commit_ts,
                                                          .mutation_seq = decoded->mutation_seq,
                                                          .value = std::move(*value)});
                        } else if (decoded->op_type != OpType::kDelete) {
                            return absl::DataLossError("unsupported operation in Phase 1A data");
                        } else {
                            row.cells.emplace(ref, std::nullopt);
                        }
                    }
                }
            }
            status = (*cursor)->next();
            if (!status.ok()) {
                return status;
            }
        }

        std::vector<VisibleRow> result;
        result.reserve(rows.size());
        for (auto& [encoded_row, accumulator] : rows) {
            static_cast<void>(encoded_row);
            VisibleRow row{.row_key = std::move(accumulator.row_key)};
            for (auto& [ref, cell] : accumulator.cells) {
                static_cast<void>(ref);
                if (!cell.has_value()) {
                    continue;
                }
                const VersionStamp cell_stamp{.commit_ts = cell->commit_ts,
                                              .mutation_seq = cell->mutation_seq};
                if (accumulator.row_delete.has_value() && cell_stamp <= *accumulator.row_delete) {
                    continue;
                }
                const auto family = accumulator.family_deletes.find(cell->ref.column_family_id);
                if (family != accumulator.family_deletes.end() && cell_stamp <= family->second) {
                    continue;
                }
                row.cells.push_back(std::move(*cell));
            }
            if (!row.cells.empty()) {
                result.push_back(std::move(row));
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("MVCC scan allocation failed");
    }
}

absl::Status EmbeddedSlice::freeze() {
    const std::lock_guard lock(writer_mutex_);
    return store_->freeze_locality_group(options_.locality_group_id);
}

absl::Status EmbeddedSlice::flush(FlushSstOptions options) {
    const std::lock_guard lock(writer_mutex_);
    auto token = store_->begin_flush(options_.locality_group_id);
    if (!token.ok()) {
        return token.status();
    }
    auto finalized = SliceStore::build_flush_sst(*token, std::move(options));
    if (!finalized.ok()) {
        return finalized.status();
    }
    return store_->install_flush(*token, *finalized);
}

uint64_t EmbeddedSlice::last_timestamp_counter() const {
    const std::lock_guard lock(writer_mutex_);
    return last_timestamp_counter_;
}

} // namespace pl::minitable
