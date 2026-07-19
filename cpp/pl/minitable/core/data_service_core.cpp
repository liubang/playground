// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/data_service_core.h"

#include <array>
#include <bit>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "cpp/pl/minitable/core/embedded_slice.h"
#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable {
namespace {

namespace pb = ::pl::minitable::proto::v2;
using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Value;

absl::Status ValidateHeader(const pb::DataRequestHeader& header, bool mutation) {
    if (header.table_id() == 0 || header.slice_id() == 0 || header.schema_version() == 0 ||
        header.route_epoch() == 0) {
        return absl::InvalidArgumentError("data request requires complete routing metadata");
    }
    if (mutation &&
        (header.context().client_id().empty() || header.context().request_id().empty())) {
        return absl::InvalidArgumentError("mutation requires client_id and request_id");
    }
    return absl::OkStatus();
}

absl::Status ValidatePhaseOne(const codec::CellKeyCodec& codec, uint32_t locality_group_id) {
    if (locality_group_id == 0) {
        return absl::InvalidArgumentError("locality group ID zero is reserved");
    }
    if (codec.format().partition_mode != PartitionMode::kGlobalOrder) {
        return absl::UnimplementedError("Phase 1 Data RPC supports GLOBAL_ORDER only");
    }
    return absl::OkStatus();
}

absl::Status ValidateAllocation(CommitAllocation allocation) {
    if (allocation.commit_ts.domain_epoch == 0 || allocation.commit_ts.counter == 0 ||
        allocation.commit_physical_ms == 0) {
        return absl::InvalidArgumentError("commit allocation is incomplete");
    }
    return absl::OkStatus();
}

absl::StatusOr<Value> FromProtoValue(const pb::Value& source, bool allow_null) {
    switch (source.kind_case()) {
        case pb::Value::kNullValue:
            if (!allow_null) {
                return absl::InvalidArgumentError("row key values must be non-null scalars");
            }
            return Value{};
        case pb::Value::kBoolValue:
            return Value::make<DataType::kBool>(source.bool_value());
        case pb::Value::kInt32Value:
            return Value::make<DataType::kInt32>(source.int32_value());
        case pb::Value::kUint32Value:
            return Value::make<DataType::kUint32>(source.uint32_value());
        case pb::Value::kInt64Value:
            return Value::make<DataType::kInt64>(source.int64_value());
        case pb::Value::kUint64Value:
            return Value::make<DataType::kUint64>(source.uint64_value());
        case pb::Value::kFloatValue:
            return Value::make<DataType::kFloat>(source.float_value());
        case pb::Value::kDoubleValue:
            return Value::make<DataType::kDouble>(source.double_value());
        case pb::Value::kStringValue:
            return Value::make<DataType::kString>(source.string_value());
        case pb::Value::kBytesValue:
            return Value::make<DataType::kBinary>(source.bytes_value());
        case pb::Value::kArrayValue: {
            std::vector<Value> values;
            values.reserve(static_cast<size_t>(source.array_value().elements_size()));
            for (const auto& element : source.array_value().elements()) {
                auto value = FromProtoValue(element, true);
                if (!value.ok()) {
                    return value.status();
                }
                values.push_back(std::move(*value));
            }
            return Value::make_array(std::move(values));
        }
        case pb::Value::kMapValue: {
            std::vector<std::pair<Value, Value>> entries;
            entries.reserve(static_cast<size_t>(source.map_value().entries_size()));
            for (const auto& entry : source.map_value().entries()) {
                auto key = FromProtoValue(entry.key(), false);
                auto value = FromProtoValue(entry.value(), true);
                if (!key.ok()) {
                    return key.status();
                }
                if (!value.ok()) {
                    return value.status();
                }
                entries.emplace_back(std::move(*key), std::move(*value));
            }
            return Value::make_map(std::move(entries));
        }
        default:
            return absl::InvalidArgumentError("protobuf Value kind is missing");
    }
}

void ToProtoValue(const Value& source, pb::Value* target) {
    switch (source.type()) {
        case DataType::kNone:
            target->mutable_null_value();
            break;
        case DataType::kBool:
            target->set_bool_value(source.as_bool());
            break;
        case DataType::kInt32:
            target->set_int32_value(source.as_int32());
            break;
        case DataType::kUint32:
            target->set_uint32_value(source.as_uint32());
            break;
        case DataType::kInt64:
            target->set_int64_value(source.as_int64());
            break;
        case DataType::kUint64:
            target->set_uint64_value(source.as_uint64());
            break;
        case DataType::kFloat:
            target->set_float_value(source.as_float());
            break;
        case DataType::kDouble:
            target->set_double_value(source.as_double());
            break;
        case DataType::kString:
            target->set_string_value(std::string(source.as_string()));
            break;
        case DataType::kBinary:
            target->set_bytes_value(std::string(source.as_binary()));
            break;
        case DataType::kArray:
            for (const auto& element : source.as_array()) {
                ToProtoValue(element, target->mutable_array_value()->add_elements());
            }
            break;
        case DataType::kMap:
            for (const auto& [key, value] : source.as_map()) {
                auto* entry = target->mutable_map_value()->add_entries();
                ToProtoValue(key, entry->mutable_key());
                ToProtoValue(value, entry->mutable_value());
            }
            break;
        default:
            // Public v2 deliberately exposes only this subset of sstv2 types.
            target->Clear();
            break;
    }
}

absl::StatusOr<std::vector<Value>> DecodeRowKey(const pb::RowKey& source) {
    if (source.values().empty()) {
        return absl::InvalidArgumentError("row key is empty");
    }
    std::vector<Value> result;
    result.reserve(static_cast<size_t>(source.values_size()));
    for (const auto& value : source.values()) {
        auto decoded = FromProtoValue(value, false);
        if (!decoded.ok()) {
            return decoded.status();
        }
        if (decoded->type() == DataType::kArray || decoded->type() == DataType::kMap) {
            return absl::InvalidArgumentError("row key values must be scalar");
        }
        result.push_back(std::move(*decoded));
    }
    return result;
}

absl::StatusOr<CellRef> DecodeCellRef(const pb::CellRef& source) {
    if (source.column_family_id() == 0) {
        return absl::InvalidArgumentError("cell requires a column family ID");
    }
    CellRef result{.column_family_id = source.column_family_id()};
    switch (source.qualifier_case()) {
        case pb::CellRef::kStaticQualifier:
            if (source.static_qualifier().column_id() == 0) {
                return absl::InvalidArgumentError("static qualifier requires a column ID");
            }
            result.qualifier = StaticQualifier{.column_id = source.static_qualifier().column_id()};
            break;
        case pb::CellRef::kDynamicQualifier:
            result.qualifier = DynamicQualifier{.value = source.dynamic_qualifier().qualifier()};
            break;
        default:
            return absl::InvalidArgumentError("cell qualifier is missing");
    }
    return result;
}

void EncodeCellRef(const CellRef& source, pb::CellRef* target) {
    target->set_column_family_id(source.column_family_id);
    if (const auto* qualifier = std::get_if<StaticQualifier>(&source.qualifier)) {
        target->mutable_static_qualifier()->set_column_id(qualifier->column_id);
    } else {
        target->mutable_dynamic_qualifier()->set_qualifier(
            std::get<DynamicQualifier>(source.qualifier).value);
    }
}

absl::StatusOr<std::string> EncodeCellValue(const Value& value) {
    std::string encoded(1, static_cast<char>(value.type()));
    auto status = sstv2::types::encode_value(value, &encoded);
    return status.ok() ? absl::StatusOr<std::string>(std::move(encoded))
                       : absl::StatusOr<std::string>(status);
}

void HashBytes(uint64_t* hash, std::string_view bytes) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (char byte : bytes) {
        *hash ^= static_cast<unsigned char>(byte);
        *hash *= kPrime;
    }
    *hash ^= 0xff;
    *hash *= kPrime;
}

template <typename T> void HashScalar(uint64_t* hash, T value) {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    HashBytes(hash, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

uint64_t PayloadHash(std::string_view canonical_request) {
    uint64_t hash = 14695981039346656037ULL;
    HashBytes(&hash, canonical_request);
    return hash == 0 ? 1 : hash;
}

template <typename Request> std::string CanonicalRequestBytes(const Request& request) {
    Request canonical = request;
    auto* context = canonical.mutable_header()->mutable_context();
    context->clear_deadline_unix_ms();
    context->clear_trace_id();
    return canonical.SerializeAsString();
}

template <typename Response> absl::StatusOr<std::string> SerializeResponse(Timestamp commit_ts) {
    Response response;
    response.mutable_commit_ts()->set_domain_epoch(commit_ts.domain_epoch);
    response.mutable_commit_ts()->set_counter(commit_ts.counter);
    std::string encoded;
    if (!response.SerializeToString(&encoded)) {
        return absl::InternalError("failed to serialize mutation response");
    }
    return encoded;
}

absl::StatusOr<PreparedDataMutation> FinishMutation(
    const pb::DataRequestHeader& header,
    std::vector<Value> row_key,
    std::vector<CommittedMemTableMutation> mutations,
    CommitAllocation allocation,
    uint32_t locality_group_id,
    uint64_t payload_hash,
    std::string serialized_response,
    const codec::CellKeyCodec& codec) {
    auto encoded_row = codec.EncodeLogicalRowKey(row_key);
    if (!encoded_row.ok()) {
        return encoded_row.status();
    }
    CommittedSliceMutation committed{
        .identity = {.client_id = header.context().client_id(),
                     .request_id = header.context().request_id(),
                     .payload_hash = payload_hash},
        .commit_ts = allocation.commit_ts,
        .commit_physical_ms = allocation.commit_physical_ms,
        .locality_group_mutations = {std::move(mutations)},
        .locality_group_ids = {locality_group_id},
        .serialized_response = std::move(serialized_response),
    };
    auto encoded = EncodeSliceMutationV2(committed, *encoded_row, codec);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return PreparedDataMutation{.encoded_entry = std::move(*encoded),
                                .serialized_response = committed.serialized_response,
                                .commit_ts = allocation.commit_ts};
}

absl::StatusOr<CommittedMemTableMutation> EncodeMutation(const codec::CellKeyCodec& codec,
                                                         const std::vector<Value>& row_key,
                                                         const RecordTarget& target,
                                                         Timestamp commit_ts,
                                                         uint32_t sequence,
                                                         OpType op_type,
                                                         std::optional<Value> value) {
    VersionedStorageKey key{
        .storage_key = {.partition = GlobalOrderPrefix{}, .row_key = row_key, .target = target},
        .commit_ts = commit_ts,
        .mutation_seq = sequence,
        .op_type = op_type};
    auto encoded_key = codec.EncodeVersionedStorageKey(key);
    if (!encoded_key.ok()) {
        return encoded_key.status();
    }
    std::string encoded_value(1, static_cast<char>(DataType::kNone));
    if (value.has_value()) {
        auto encoded = EncodeCellValue(*value);
        if (!encoded.ok()) {
            return encoded.status();
        }
        encoded_value = std::move(*encoded);
    }
    return CommittedMemTableMutation{.encoded_key = std::move(*encoded_key),
                                     .encoded_value = std::move(encoded_value)};
}

} // namespace

absl::StatusOr<MutationIdentity> PutIdentityV2(const pb::PutRequest& request) {
    auto status = ValidateHeader(request.header(), true);
    if (!status.ok()) {
        return status;
    }
    return MutationIdentity{.client_id = request.header().context().client_id(),
                            .request_id = request.header().context().request_id(),
                            .payload_hash = PayloadHash(CanonicalRequestBytes(request))};
}

absl::StatusOr<MutationIdentity> DeleteIdentityV2(const pb::DeleteRequest& request) {
    auto status = ValidateHeader(request.header(), true);
    if (!status.ok()) {
        return status;
    }
    return MutationIdentity{.client_id = request.header().context().client_id(),
                            .request_id = request.header().context().request_id(),
                            .payload_hash = PayloadHash(CanonicalRequestBytes(request))};
}

absl::StatusOr<PreparedDataMutation> PreparePutV2(const pb::PutRequest& request,
                                                  CommitAllocation allocation,
                                                  uint32_t locality_group_id,
                                                  const codec::CellKeyCodec& codec) {
    auto status = ValidateHeader(request.header(), true);
    if (!status.ok()) {
        return status;
    }
    status = ValidatePhaseOne(codec, locality_group_id);
    if (!status.ok()) {
        return status;
    }
    status = ValidateAllocation(allocation);
    if (!status.ok()) {
        return status;
    }
    if (request.mutations().empty()) {
        return absl::InvalidArgumentError("Put mutation list is empty");
    }
    auto row_key = DecodeRowKey(request.row_key());
    if (!row_key.ok()) {
        return row_key.status();
    }
    try {
        std::vector<CommittedMemTableMutation> mutations;
        mutations.reserve(static_cast<size_t>(request.mutations_size()));
        for (int index = 0; index < request.mutations_size(); ++index) {
            const auto& source = request.mutations(index);
            auto ref = DecodeCellRef(source.cell());
            if (!ref.ok()) {
                return ref.status();
            }
            OpType op_type;
            std::optional<Value> value;
            switch (source.type()) {
                case pb::SET:
                    if (!source.has_value()) {
                        return absl::InvalidArgumentError("SET requires a value");
                    }
                    {
                        auto decoded = FromProtoValue(source.value(), false);
                        if (!decoded.ok()) {
                            return decoded.status();
                        }
                        value = std::move(*decoded);
                    }
                    op_type = OpType::kPut;
                    break;
                case pb::SET_NULL:
                    return absl::UnimplementedError("SET_NULL is not enabled in Phase 1 Data RPC");
                case pb::DELETE_COLUMN:
                    if (source.has_value()) {
                        return absl::InvalidArgumentError("DELETE_COLUMN must not carry a value");
                    }
                    op_type = OpType::kDelete;
                    break;
                default:
                    return absl::InvalidArgumentError("unknown CellMutation type");
            }
            auto encoded = EncodeMutation(codec,
                                          *row_key,
                                          *ref,
                                          allocation.commit_ts,
                                          static_cast<uint32_t>(index),
                                          op_type,
                                          std::move(value));
            if (!encoded.ok()) {
                return encoded.status();
            }
            mutations.push_back(std::move(*encoded));
        }
        auto response = SerializeResponse<pb::PutResponse>(allocation.commit_ts);
        if (!response.ok()) {
            return response.status();
        }
        return FinishMutation(request.header(),
                              std::move(*row_key),
                              std::move(mutations),
                              allocation,
                              locality_group_id,
                              PayloadHash(CanonicalRequestBytes(request)),
                              std::move(*response),
                              codec);
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Put request conversion allocation failed");
    }
}

absl::StatusOr<PreparedDataMutation> PrepareDeleteV2(const pb::DeleteRequest& request,
                                                     CommitAllocation allocation,
                                                     uint32_t locality_group_id,
                                                     const codec::CellKeyCodec& codec) {
    auto status = ValidateHeader(request.header(), true);
    if (!status.ok()) {
        return status;
    }
    status = ValidatePhaseOne(codec, locality_group_id);
    if (!status.ok()) {
        return status;
    }
    status = ValidateAllocation(allocation);
    if (!status.ok()) {
        return status;
    }
    if (request.targets().empty()) {
        return absl::InvalidArgumentError("Delete target list is empty");
    }
    auto row_key = DecodeRowKey(request.row_key());
    if (!row_key.ok()) {
        return row_key.status();
    }
    try {
        std::vector<CommittedMemTableMutation> mutations;
        mutations.reserve(static_cast<size_t>(request.targets_size()));
        for (int index = 0; index < request.targets_size(); ++index) {
            const auto& source = request.targets(index);
            RecordTarget target;
            switch (source.target_case()) {
                case pb::DeleteTarget::kRow:
                    if (!source.row() || request.targets_size() != 1) {
                        return absl::InvalidArgumentError(
                            "row delete must be true and must be the only target");
                    }
                    target = RowTombstone{};
                    break;
                case pb::DeleteTarget::kColumnFamilyId:
                    if (source.column_family_id() == 0) {
                        return absl::InvalidArgumentError("column family ID zero is reserved");
                    }
                    target = ColumnFamilyTombstone{.column_family_id = source.column_family_id()};
                    break;
                case pb::DeleteTarget::kCell: {
                    auto ref = DecodeCellRef(source.cell());
                    if (!ref.ok()) {
                        return ref.status();
                    }
                    target = std::move(*ref);
                    break;
                }
                default:
                    return absl::InvalidArgumentError("Delete target is missing");
            }
            auto encoded = EncodeMutation(codec,
                                          *row_key,
                                          target,
                                          allocation.commit_ts,
                                          static_cast<uint32_t>(index),
                                          OpType::kDelete,
                                          std::nullopt);
            if (!encoded.ok()) {
                return encoded.status();
            }
            mutations.push_back(std::move(*encoded));
        }
        auto response = SerializeResponse<pb::DeleteResponse>(allocation.commit_ts);
        if (!response.ok()) {
            return response.status();
        }
        return FinishMutation(request.header(),
                              std::move(*row_key),
                              std::move(mutations),
                              allocation,
                              locality_group_id,
                              PayloadHash(CanonicalRequestBytes(request)),
                              std::move(*response),
                              codec);
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Delete request conversion allocation failed");
    }
}

absl::StatusOr<std::optional<pb::RowResult>> ExecuteGetV2(const pb::GetRequest& request,
                                                          Timestamp read_ts,
                                                          uint32_t locality_group_id,
                                                          const codec::CellKeyCodec& codec,
                                                          const SliceApplyMachine& machine,
                                                          size_t max_row_aggregate_bytes) {
    auto status = ValidateHeader(request.header(), false);
    if (!status.ok()) {
        return status;
    }
    status = ValidatePhaseOne(codec, locality_group_id);
    if (!status.ok()) {
        return status;
    }
    if (request.max_versions() > 1) {
        return absl::UnimplementedError("Phase 1 Get supports at most one visible version");
    }
    if (request.read_policy().consistency() == pb::BOUNDED_STALENESS) {
        return absl::UnimplementedError("bounded staleness requires closed_ts and safe_ts");
    }
    if (request.read_policy().consistency() == pb::READ_CONSISTENCY_UNSPECIFIED) {
        return absl::InvalidArgumentError("read consistency is required");
    }
    if (!request.projection().columns().empty()) {
        return absl::UnimplementedError("Phase 1 Get projection is not implemented");
    }
    auto row_key = DecodeRowKey(request.row_key());
    if (!row_key.ok()) {
        return row_key.status();
    }
    auto row = ReadVisibleRow(
        codec, machine.store(), locality_group_id, *row_key, read_ts, max_row_aggregate_bytes);
    if (!row.ok()) {
        return row.status();
    }
    if (!row->has_value()) {
        return std::optional<pb::RowResult>{};
    }
    pb::RowResult result;
    for (const auto& value : (*row)->row_key) {
        ToProtoValue(value, result.mutable_row_key()->add_values());
    }
    for (const auto& cell : (*row)->cells) {
        auto* target = result.add_cells();
        EncodeCellRef(cell.ref, target->mutable_cell());
        ToProtoValue(cell.value, target->mutable_value());
        target->mutable_commit_ts()->set_domain_epoch(cell.commit_ts.domain_epoch);
        target->mutable_commit_ts()->set_counter(cell.commit_ts.counter);
        target->set_mutation_seq(cell.mutation_seq);
    }
    return std::optional<pb::RowResult>(std::move(result));
}

} // namespace pl::minitable
