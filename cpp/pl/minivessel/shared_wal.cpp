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

#include "cpp/pl/minivessel/shared_wal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "cpp/pl/sstv2/codec/endian.h"
#include "cpp/pl/sstv2/codec/fixed.h"

namespace pl::minivessel {
namespace {

constexpr uint32_t kFrameMagic = 0x4d56574c; // MVWL
constexpr uint16_t kFrameVersion = 1;
constexpr size_t kHeaderSize = 40;

void Put16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xffU);
    destination[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
}

uint16_t Get16(const uint8_t* source) {
    return static_cast<uint16_t>(static_cast<uint8_t>(source[0])) |
           static_cast<uint16_t>(static_cast<uint8_t>(source[1]) << 8U);
}

absl::Status ValidateType(uint8_t value) {
    switch (static_cast<LogRecordType>(value)) {
        case LogRecordType::kMutation:
        case LogRecordType::kPrimaryBarrier:
        case LogRecordType::kCheckpoint:
            return absl::OkStatus();
    }
    return absl::DataLossError("unknown WAL record type");
}

absl::StatusOr<std::vector<std::byte>> Encode(const LogRecord& record, size_t max_record_bytes) {
    if (!record.lrsn.valid() || !record.writer_epoch.valid()) {
        return absl::InvalidArgumentError("WAL record requires valid LRSN and writer epoch");
    }
    if (record.request_id.size() > std::numeric_limits<uint32_t>::max() ||
        record.payload.size() > std::numeric_limits<uint32_t>::max() ||
        record.request_id.size() + record.payload.size() > max_record_bytes) {
        return absl::ResourceExhaustedError("WAL record exceeds configured limit");
    }
    const size_t frame_size = kHeaderSize + record.request_id.size() + record.payload.size();
    std::vector<std::byte> frame(frame_size);
    auto* bytes = reinterpret_cast<uint8_t*>(frame.data());
    sstv2::codec::encode_fixed32(bytes, kFrameMagic);
    Put16(bytes + 4, kFrameVersion);
    bytes[6] = static_cast<uint8_t>(record.type);
    bytes[7] = 0;
    sstv2::codec::encode_fixed64(bytes + 8, record.lrsn.value());
    sstv2::codec::encode_fixed64(bytes + 16, record.writer_epoch.value());
    sstv2::codec::encode_fixed32(bytes + 24, static_cast<uint32_t>(record.request_id.size()));
    sstv2::codec::encode_fixed32(bytes + 28, static_cast<uint32_t>(record.payload.size()));
    sstv2::codec::encode_fixed32(bytes + 32, 0);
    sstv2::codec::encode_fixed32(bytes + 36, 0);
    std::memcpy(bytes + kHeaderSize, record.request_id.data(), record.request_id.size());
    std::memcpy(bytes + kHeaderSize + record.request_id.size(),
                record.payload.data(),
                record.payload.size());
    const uint32_t checksum = static_cast<uint32_t>(sstv2::codec::crc32c_u64(
        std::string_view(reinterpret_cast<const char*>(bytes), frame.size())));
    sstv2::codec::encode_fixed32(bytes + 32, checksum);
    return frame;
}

absl::StatusOr<LogRecord> Decode(std::span<const std::byte> frame, size_t max_record_bytes) {
    if (frame.size() < kHeaderSize) {
        return absl::DataLossError("truncated WAL frame header");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(frame.data());
    if (sstv2::codec::decode_fixed32(bytes) != kFrameMagic || Get16(bytes + 4) != kFrameVersion) {
        return absl::DataLossError("invalid WAL frame magic or version");
    }
    auto status = ValidateType(static_cast<uint8_t>(bytes[6]));
    if (!status.ok()) {
        return status;
    }
    const uint32_t request_size = sstv2::codec::decode_fixed32(bytes + 24);
    const uint32_t payload_size = sstv2::codec::decode_fixed32(bytes + 28);
    const size_t body_size = static_cast<size_t>(request_size) + payload_size;
    if (body_size > max_record_bytes || frame.size() != kHeaderSize + body_size) {
        return absl::DataLossError("invalid WAL frame length");
    }
    std::vector<std::byte> checksum_input(frame.begin(), frame.end());
    auto* checksum_bytes = reinterpret_cast<uint8_t*>(checksum_input.data());
    const uint32_t expected = sstv2::codec::decode_fixed32(checksum_bytes + 32);
    sstv2::codec::encode_fixed32(checksum_bytes + 32, 0);
    const uint32_t actual = static_cast<uint32_t>(sstv2::codec::crc32c_u64(
        std::string_view(reinterpret_cast<const char*>(checksum_bytes), checksum_input.size())));
    if (actual != expected) {
        return absl::DataLossError("WAL frame checksum mismatch");
    }
    LogRecord record{
        .lrsn = Lrsn(sstv2::codec::decode_fixed64(bytes + 8)),
        .writer_epoch = WriterEpoch(sstv2::codec::decode_fixed64(bytes + 16)),
        .type = static_cast<LogRecordType>(static_cast<uint8_t>(bytes[6])),
        .request_id = std::string(reinterpret_cast<const char*>(bytes + kHeaderSize), request_size),
        .payload = std::vector<std::byte>(payload_size),
    };
    std::memcpy(record.payload.data(), bytes + kHeaderSize + request_size, payload_size);
    if (!record.lrsn.valid() || !record.writer_epoch.valid()) {
        return absl::DataLossError("WAL frame contains invalid identity");
    }
    return record;
}

} // namespace

FramedSharedWal::FramedSharedWal(ActiveLogStorage* storage, FramedSharedWalOptions options)
    : storage_(storage), options_(std::move(options)), next_lrsn_(1) {}

FramedSharedWal::~FramedSharedWal() {
    if (writer_.handle.valid() && storage_ != nullptr) {
        (void)storage_->release_writer(writer_.handle);
    }
}

absl::StatusOr<FramedSharedWal::ScanResult> FramedSharedWal::scan_durable() {
    if (storage_ == nullptr) {
        return absl::FailedPreconditionError("FramedSharedWal is not configured with a backend");
    }
    auto durable_size = storage_->durable_size(options_.path);
    if (!durable_size.ok()) {
        if (durable_size.status().code() == absl::StatusCode::kNotFound) {
            return ScanResult{};
        }
        return durable_size.status();
    }
    if (durable_size->value() > std::numeric_limits<size_t>::max()) {
        return absl::ResourceExhaustedError("durable WAL is too large to scan");
    }
    std::vector<std::byte> bytes(static_cast<size_t>(durable_size->value()));
    auto status = storage_->read_durable(options_.path, ByteOffset(0), bytes);
    if (!status.ok()) {
        return status;
    }
    ScanResult result{.durable_offset = *durable_size};
    size_t offset = 0;
    uint64_t expected_lrsn = 1;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kHeaderSize) {
            return absl::DataLossError("durable WAL ends with a partial frame header");
        }
        const auto* header = reinterpret_cast<const uint8_t*>(bytes.data() + offset);
        const size_t body_size = static_cast<size_t>(sstv2::codec::decode_fixed32(header + 24)) +
                                 sstv2::codec::decode_fixed32(header + 28);
        if (body_size > options_.max_record_bytes ||
            body_size > bytes.size() - offset - kHeaderSize) {
            return absl::DataLossError("durable WAL ends with a partial or oversized frame");
        }
        const size_t frame_size = kHeaderSize + body_size;
        auto record = Decode(std::span<const std::byte>(bytes).subspan(offset, frame_size),
                             options_.max_record_bytes);
        if (!record.ok()) {
            return record.status();
        }
        if (record->lrsn.value() != expected_lrsn) {
            return absl::DataLossError("WAL LRSN sequence has a gap or duplicate");
        }
        result.last_lrsn = record->lrsn;
        result.records.push_back(std::move(*record));
        ++expected_lrsn;
        offset += frame_size;
    }
    return result;
}

absl::StatusOr<WriterLease> FramedSharedWal::acquire_writer(std::string_view owner_instance_id,
                                                            AssignmentEpoch assignment_epoch,
                                                            uint64_t lease_timeout_ms) {
    if (storage_ == nullptr || options_.path.empty() || options_.group.group_id.empty() ||
        !options_.group.incarnation.valid()) {
        return absl::FailedPreconditionError("FramedSharedWal is not configured");
    }
    if (writer_.handle.valid()) {
        return absl::AlreadyExistsError("WAL writer is already acquired");
    }
    auto session = storage_->acquire_writer(AcquireWriterRequest{
        .path = options_.path,
        .owner_instance_id = std::string(owner_instance_id),
        .assignment_epoch = assignment_epoch,
        .lease_timeout_ms = lease_timeout_ms,
    });
    if (!session.ok()) {
        return session.status();
    }
    writer_ = *session;
    auto scan = scan_durable();
    if (!scan.ok()) {
        (void)storage_->release_writer(writer_.handle);
        writer_ = {};
        return scan.status();
    }
    if (scan->last_lrsn.value() == std::numeric_limits<uint64_t>::max()) {
        (void)storage_->release_writer(writer_.handle);
        writer_ = {};
        return absl::OutOfRangeError("WAL exhausted the LRSN space");
    }
    next_lrsn_ = Lrsn(scan->last_lrsn.value() + 1);
    owner_instance_id_ = std::string(owner_instance_id);
    return WriterLease{
        .group = options_.group,
        .owner_instance_id = std::string(owner_instance_id),
        .assignment_epoch = writer_.assignment_epoch,
        .writer_epoch = writer_.writer_epoch,
        .lease_id = writer_.lease_id,
        .expires_at = writer_.expires_at,
    };
}

absl::StatusOr<WriterLease> FramedSharedWal::renew_writer() {
    if (!writer_.handle.valid()) {
        return absl::FailedPreconditionError("WAL writer is not acquired");
    }
    auto renewed = storage_->renew_writer(writer_.handle);
    if (!renewed.ok()) {
        return renewed.status();
    }
    writer_ = *renewed;
    return WriterLease{
        .group = options_.group,
        .owner_instance_id = owner_instance_id_,
        .assignment_epoch = writer_.assignment_epoch,
        .writer_epoch = writer_.writer_epoch,
        .lease_id = writer_.lease_id,
        .expires_at = writer_.expires_at,
    };
}

absl::StatusOr<DurableAppend> FramedSharedWal::append(LogRecordType type,
                                                      std::string request_id,
                                                      std::span<const std::byte> payload) {
    if (!writer_.handle.valid()) {
        return absl::FailedPreconditionError("WAL writer is not acquired");
    }
    LogRecord record{
        .lrsn = next_lrsn_,
        .writer_epoch = writer_.writer_epoch,
        .type = type,
        .request_id = std::move(request_id),
        .payload = std::vector<std::byte>(payload.begin(), payload.end()),
    };
    auto frame = Encode(record, options_.max_record_bytes);
    if (!frame.ok()) {
        return frame.status();
    }
    if (frame->size() > std::numeric_limits<uint64_t>::max() - writer_.next_offset.value()) {
        return absl::OutOfRangeError("WAL byte offset overflow");
    }
    const WriterEpoch expected_epoch = writer_.writer_epoch;
    const ByteOffset expected_end(writer_.next_offset.value() + frame->size());
    auto status = storage_->append(writer_.handle,
                                   AppendOptions{.expected_offset = writer_.next_offset,
                                                 .packet_sequence = writer_.next_packet_sequence},
                                   *frame);
    if (!status.ok()) {
        return status;
    }
    auto synced = storage_->sync(writer_.handle);
    if (!synced.ok()) {
        return synced.status();
    }
    if (synced->writer_epoch != expected_epoch || synced->durable_offset != expected_end) {
        return absl::DataLossError(
            "WAL sync result does not prove the appended frame durable under the current epoch");
    }
    if (writer_.next_packet_sequence.value() == std::numeric_limits<uint64_t>::max() ||
        next_lrsn_.value() == std::numeric_limits<uint64_t>::max()) {
        return absl::OutOfRangeError("WAL sequence space exhausted after durable append");
    }
    writer_.next_offset = synced->durable_offset;
    writer_.next_packet_sequence = PacketSequence(writer_.next_packet_sequence.value() + 1);
    next_lrsn_ = Lrsn(next_lrsn_.value() + 1);
    return DurableAppend{.record = std::move(record), .durable_offset = synced->durable_offset};
}

absl::StatusOr<std::vector<LogRecord>> FramedSharedWal::read(Lrsn first_lrsn, size_t max_records) {
    if (!first_lrsn.valid() || max_records == 0) {
        return absl::InvalidArgumentError("read requires a valid first LRSN and non-zero limit");
    }
    auto scan = scan_durable();
    if (!scan.ok()) {
        return scan.status();
    }
    std::vector<LogRecord> result;
    for (auto& record : scan->records) {
        if (record.lrsn >= first_lrsn && result.size() < max_records) {
            result.push_back(std::move(record));
        }
    }
    return result;
}

absl::StatusOr<Lrsn> FramedSharedWal::durable_lrsn() {
    auto scan = scan_durable();
    return scan.ok() ? absl::StatusOr<Lrsn>(scan->last_lrsn) : scan.status();
}

absl::Status FramedSharedWal::release_writer() {
    if (!writer_.handle.valid()) {
        return absl::OkStatus();
    }
    auto status = storage_->release_writer(writer_.handle);
    writer_ = {};
    owner_instance_id_.clear();
    return status;
}

} // namespace pl::minivessel
