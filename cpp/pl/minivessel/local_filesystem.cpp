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

#include "cpp/pl/minivessel/local_filesystem.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/codec/endian.h"
#include "cpp/pl/sstv2/codec/fixed.h"

namespace pl::minivessel {
namespace {

constexpr uint64_t kMetadataMagic = 0x4D56455353454C31ULL;
constexpr uint32_t kMetadataVersion = 1;
constexpr size_t kMetadataSize = 88;
constexpr size_t kMetadataChecksumOffset = 84;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

std::string MetadataPath(std::string_view path) {
    return absl::StrCat(path, ".minivessel.meta");
}

std::string ParentPath(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return ".";
    }
    return slash == 0 ? "/" : std::string(path.substr(0, slash));
}

absl::Status ErrnoStatus(std::string_view operation, std::string_view path, int error) {
    const std::string message = absl::StrCat(operation, " ", path, ": ", std::strerror(error));
    switch (error) {
        case EEXIST:
            return absl::AlreadyExistsError(message);
        case ENOENT:
            return absl::NotFoundError(message);
        case EACCES:
        case EPERM:
            return absl::PermissionDeniedError(message);
        case ENOSPC:
            return absl::ResourceExhaustedError(message);
        case EWOULDBLOCK:
            return absl::FailedPreconditionError(message);
        case EINVAL:
        case EISDIR:
        case ENOTDIR:
            return absl::InvalidArgumentError(message);
        default:
            return absl::InternalError(message);
    }
}

absl::Status FsyncParent(std::string_view path) {
    const std::string parent = ParentPath(path);
    const int descriptor = ::open(parent.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return ErrnoStatus("open parent directory", parent, errno);
    }
    const int sync_result = ::fsync(descriptor);
    const int sync_error = sync_result == 0 ? 0 : errno;
    const int close_result = ::close(descriptor);
    if (sync_result != 0) {
        return ErrnoStatus("fsync parent directory", parent, sync_error);
    }
    return close_result == 0 ? absl::OkStatus()
                             : ErrnoStatus("close parent directory", parent, errno);
}

absl::Status WriteExactly(int descriptor, std::span<const std::byte> data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(descriptor, data.data() + written, data.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("write metadata", "sidecar", errno);
        }
        if (count == 0) {
            return absl::InternalError("metadata write made no progress");
        }
        written += static_cast<size_t>(count);
    }
    return absl::OkStatus();
}

absl::Status ReadExactly(int descriptor, std::span<std::byte> data) {
    size_t read = 0;
    while (read < data.size()) {
        const ssize_t count = ::read(descriptor, data.data() + read, data.size() - read);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("read metadata", "sidecar", errno);
        }
        if (count == 0) {
            return absl::DataLossError("truncated MiniVessel local metadata");
        }
        read += static_cast<size_t>(count);
    }
    return absl::OkStatus();
}

} // namespace

struct LocalFileSystem::PersistedMetadata final {
    uint64_t writer_epoch = 0;
    uint64_t assignment_epoch = 0;
    uint64_t durable_offset = 0;
    uint64_t lease_id = 0;
    uint64_t expires_at_ms = 0;
    uint64_t next_packet_sequence = 0;
    uint64_t last_packet_offset = 0;
    uint64_t last_packet_size = 0;
    uint32_t last_packet_checksum = 0;
    bool sealed = false;
};

struct LocalFileSystem::OpenWriter final {
    int descriptor = -1;
    std::string path;
    std::string owner_instance_id;
    uint64_t lease_timeout_ms = 0;
    PersistedMetadata metadata;
    uint64_t next_offset = 0;
    uint64_t next_packet_sequence = 0;
    std::mutex mutex;
};

namespace {

std::array<std::byte, kMetadataSize> EncodeMetadata(
    const LocalFileSystem::PersistedMetadata& metadata) {
    std::array<std::byte, kMetadataSize> encoded{};
    auto* bytes = reinterpret_cast<uint8_t*>(encoded.data());
    sstv2::codec::encode_fixed64(bytes, kMetadataMagic);
    sstv2::codec::encode_fixed32(bytes + 8, kMetadataVersion);
    sstv2::codec::encode_fixed32(bytes + 12, metadata.sealed ? 1U : 0U);
    sstv2::codec::encode_fixed64(bytes + 16, metadata.writer_epoch);
    sstv2::codec::encode_fixed64(bytes + 24, metadata.assignment_epoch);
    sstv2::codec::encode_fixed64(bytes + 32, metadata.durable_offset);
    sstv2::codec::encode_fixed64(bytes + 40, metadata.lease_id);
    sstv2::codec::encode_fixed64(bytes + 48, metadata.expires_at_ms);
    sstv2::codec::encode_fixed64(bytes + 56, metadata.next_packet_sequence);
    sstv2::codec::encode_fixed64(bytes + 64, metadata.last_packet_offset);
    sstv2::codec::encode_fixed64(bytes + 72, metadata.last_packet_size);
    sstv2::codec::encode_fixed32(bytes + 80, metadata.last_packet_checksum);
    const auto checksum_input =
        std::string_view(reinterpret_cast<const char*>(encoded.data()), kMetadataChecksumOffset);
    sstv2::codec::encode_fixed32(bytes + kMetadataChecksumOffset,
                                 static_cast<uint32_t>(sstv2::codec::crc32c_u64(checksum_input)));
    return encoded;
}

absl::StatusOr<LocalFileSystem::PersistedMetadata> DecodeMetadata(
    std::span<const std::byte> encoded) {
    if (encoded.size() != kMetadataSize) {
        return absl::DataLossError("invalid MiniVessel metadata length");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.data());
    if (sstv2::codec::decode_fixed64(bytes) != kMetadataMagic ||
        sstv2::codec::decode_fixed32(bytes + 8) != kMetadataVersion) {
        return absl::DataLossError("invalid MiniVessel metadata header");
    }
    const auto checksum_input =
        std::string_view(reinterpret_cast<const char*>(encoded.data()), kMetadataChecksumOffset);
    if (sstv2::codec::decode_fixed32(bytes + kMetadataChecksumOffset) !=
        static_cast<uint32_t>(sstv2::codec::crc32c_u64(checksum_input))) {
        return absl::DataLossError("MiniVessel metadata checksum mismatch");
    }
    const uint32_t flags = sstv2::codec::decode_fixed32(bytes + 12);
    if ((flags & ~uint32_t{1}) != 0) {
        return absl::DataLossError("invalid MiniVessel metadata flags");
    }
    return LocalFileSystem::PersistedMetadata{
        .writer_epoch = sstv2::codec::decode_fixed64(bytes + 16),
        .assignment_epoch = sstv2::codec::decode_fixed64(bytes + 24),
        .durable_offset = sstv2::codec::decode_fixed64(bytes + 32),
        .lease_id = sstv2::codec::decode_fixed64(bytes + 40),
        .expires_at_ms = sstv2::codec::decode_fixed64(bytes + 48),
        .next_packet_sequence = sstv2::codec::decode_fixed64(bytes + 56),
        .last_packet_offset = sstv2::codec::decode_fixed64(bytes + 64),
        .last_packet_size = sstv2::codec::decode_fixed64(bytes + 72),
        .last_packet_checksum = sstv2::codec::decode_fixed32(bytes + 80),
        .sealed = flags != 0,
    };
}

} // namespace

LocalFileSystem::LocalFileSystem()
    : object_filesystem_(std::make_shared<sstv2::io::LocalFileSystem>()) {}

LocalFileSystem::~LocalFileSystem() {
    std::lock_guard lock(mutex_);
    for (const auto& [handle, writer] : writers_) {
        static_cast<void>(handle);
        std::lock_guard writer_lock(writer->mutex);
        if (writer->descriptor >= 0) {
            (void)::flock(writer->descriptor, LOCK_UN);
            (void)::close(writer->descriptor);
            writer->descriptor = -1;
        }
    }
}

std::shared_ptr<sstv2::io::FileSystem> LocalFileSystem::object_filesystem() const {
    return object_filesystem_;
}

WriterHandle LocalFileSystem::allocate_writer_handle() noexcept {
    return WriterHandle(next_handle_.fetch_add(1, std::memory_order_relaxed));
}

absl::StatusOr<LocalFileSystem::PersistedMetadata> LocalFileSystem::read_metadata(
    std::string_view path, bool wal_created) {
    const std::string metadata_path = MetadataPath(path);
    const int descriptor = ::open(metadata_path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        if (errno == ENOENT && wal_created) {
            return PersistedMetadata{};
        }
        return errno == ENOENT ? absl::DataLossError("existing WAL is missing MiniVessel metadata")
                               : ErrnoStatus("open metadata", metadata_path, errno);
    }
    struct stat metadata_stat{};
    if (::fstat(descriptor, &metadata_stat) != 0) {
        const int error = errno;
        (void)::close(descriptor);
        return ErrnoStatus("stat metadata", metadata_path, error);
    }
    if (metadata_stat.st_size != static_cast<off_t>(kMetadataSize)) {
        (void)::close(descriptor);
        return absl::DataLossError("invalid MiniVessel metadata file size");
    }
    std::array<std::byte, kMetadataSize> encoded{};
    auto status = ReadExactly(descriptor, encoded);
    const int close_result = ::close(descriptor);
    if (!status.ok()) {
        return status;
    }
    if (close_result != 0) {
        return ErrnoStatus("close metadata", metadata_path, errno);
    }
    return DecodeMetadata(encoded);
}

absl::Status LocalFileSystem::persist_metadata(std::string_view path,
                                               const PersistedMetadata& metadata) {
    const std::string metadata_path = MetadataPath(path);
    const std::string temporary_path = absl::StrCat(metadata_path, ".tmp");
    const int descriptor = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        return ErrnoStatus("create metadata", temporary_path, errno);
    }
    const auto encoded = EncodeMetadata(metadata);
    auto status = WriteExactly(descriptor, encoded);
    if (status.ok() && ::fsync(descriptor) != 0) {
        status = ErrnoStatus("fsync metadata", temporary_path, errno);
    }
    const int close_result = ::close(descriptor);
    if (status.ok() && close_result != 0) {
        status = ErrnoStatus("close metadata", temporary_path, errno);
    }
    if (!status.ok()) {
        (void)::unlink(temporary_path.c_str());
        return status;
    }
    if (::rename(temporary_path.c_str(), metadata_path.c_str()) != 0) {
        const int error = errno;
        (void)::unlink(temporary_path.c_str());
        return ErrnoStatus("publish metadata", metadata_path, error);
    }
    return FsyncParent(metadata_path);
}

absl::StatusOr<std::shared_ptr<LocalFileSystem::OpenWriter>> LocalFileSystem::get_writer(
    WriterHandle handle) const {
    std::lock_guard lock(mutex_);
    const auto it = writers_.find(handle);
    if (it == writers_.end()) {
        return absl::FailedPreconditionError("writer handle is closed or invalid");
    }
    return it->second;
}

absl::StatusOr<WriterSession> LocalFileSystem::acquire_writer(const AcquireWriterRequest& request) {
    if (request.path.empty() || request.owner_instance_id.empty() ||
        !request.assignment_epoch.valid() || request.lease_timeout_ms == 0) {
        return absl::InvalidArgumentError("incomplete local writer acquisition request");
    }
    const std::string path(request.path);
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    const bool wal_created = descriptor >= 0;
    int wal_descriptor = descriptor;
    if (!wal_created && errno == EEXIST) {
        wal_descriptor = ::open(path.c_str(), O_RDWR);
    }
    if (wal_descriptor < 0) {
        return ErrnoStatus("open WAL", path, errno);
    }
    if (::flock(wal_descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        (void)::close(wal_descriptor);
        return ErrnoStatus("acquire WAL writer lock", path, error);
    }
    auto metadata = read_metadata(path, wal_created);
    if (!metadata.ok()) {
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return metadata.status();
    }
    if (metadata->sealed) {
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return absl::FailedPreconditionError("sealed WAL cannot acquire a writer");
    }
    if (request.assignment_epoch.value() < metadata->assignment_epoch) {
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return absl::FailedPreconditionError("assignment epoch would move backwards");
    }
    if (metadata->writer_epoch == std::numeric_limits<uint64_t>::max()) {
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return absl::OutOfRangeError("local writer epoch exhausted");
    }
    if (::ftruncate(wal_descriptor, static_cast<off_t>(metadata->durable_offset)) != 0) {
        const auto status = ErrnoStatus("recover WAL durable prefix", path, errno);
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return status;
    }
    if (::fsync(wal_descriptor) != 0) {
        const auto status = ErrnoStatus("fsync recovered WAL", path, errno);
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return status;
    }
    ++metadata->writer_epoch;
    metadata->assignment_epoch = request.assignment_epoch.value();
    metadata->lease_id = metadata->writer_epoch;
    const uint64_t now_ms = NowMs();
    if (request.lease_timeout_ms > std::numeric_limits<uint64_t>::max() - now_ms) {
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return absl::InvalidArgumentError("WAL writer lease expiry overflows");
    }
    metadata->expires_at_ms = now_ms + request.lease_timeout_ms;
    metadata->next_packet_sequence = 0;
    metadata->last_packet_offset = metadata->durable_offset;
    metadata->last_packet_size = 0;
    metadata->last_packet_checksum = 0;
    auto status = persist_metadata(path, *metadata);
    if (status.ok() && wal_created) {
        status = FsyncParent(path);
    }
    if (!status.ok()) {
        (void)::flock(wal_descriptor, LOCK_UN);
        (void)::close(wal_descriptor);
        return status;
    }

    const auto handle = allocate_writer_handle();
    auto writer = std::make_shared<OpenWriter>();
    writer->descriptor = wal_descriptor;
    writer->path = path;
    writer->owner_instance_id = request.owner_instance_id;
    writer->lease_timeout_ms = request.lease_timeout_ms;
    writer->metadata = *metadata;
    writer->next_offset = metadata->durable_offset;
    writer->next_packet_sequence = metadata->next_packet_sequence;
    {
        std::lock_guard lock(mutex_);
        writers_.emplace(handle, writer);
    }
    return WriterSession{
        .handle = handle,
        .assignment_epoch = AssignmentEpoch(metadata->assignment_epoch),
        .writer_epoch = WriterEpoch(metadata->writer_epoch),
        .lease_id = LeaseId(metadata->lease_id),
        .expires_at = UnixTimeMillis(metadata->expires_at_ms),
        .next_offset = ByteOffset(metadata->durable_offset),
        .next_packet_sequence = PacketSequence(metadata->next_packet_sequence),
    };
}

absl::StatusOr<WriterSession> LocalFileSystem::renew_writer(WriterHandle handle) {
    auto writer = get_writer(handle);
    if (!writer.ok()) {
        return writer.status();
    }
    std::lock_guard lock((*writer)->mutex);
    const uint64_t now_ms = NowMs();
    if (now_ms >= (*writer)->metadata.expires_at_ms) {
        return absl::AbortedError("WAL writer lease expired and cannot be renewed");
    }
    if ((*writer)->lease_timeout_ms > std::numeric_limits<uint64_t>::max() - now_ms) {
        return absl::InvalidArgumentError("WAL writer lease expiry overflows");
    }
    (*writer)->metadata.expires_at_ms = now_ms + (*writer)->lease_timeout_ms;
    auto status = persist_metadata((*writer)->path, (*writer)->metadata);
    if (!status.ok()) {
        return status;
    }
    return WriterSession{
        .handle = handle,
        .assignment_epoch = AssignmentEpoch((*writer)->metadata.assignment_epoch),
        .writer_epoch = WriterEpoch((*writer)->metadata.writer_epoch),
        .lease_id = LeaseId((*writer)->metadata.lease_id),
        .expires_at = UnixTimeMillis((*writer)->metadata.expires_at_ms),
        .next_offset = ByteOffset((*writer)->next_offset),
        .next_packet_sequence = PacketSequence((*writer)->next_packet_sequence),
    };
}

absl::Status LocalFileSystem::append(WriterHandle handle,
                                     const AppendOptions& options,
                                     std::span<const std::byte> data) {
    auto writer = get_writer(handle);
    if (!writer.ok()) {
        return writer.status();
    }
    std::lock_guard lock((*writer)->mutex);
    if ((*writer)->descriptor < 0 || (*writer)->metadata.sealed) {
        return absl::FailedPreconditionError("writer is closed or WAL is sealed");
    }
    if (NowMs() >= (*writer)->metadata.expires_at_ms) {
        return absl::FailedPreconditionError("local writer lease expired");
    }
    if (options.expected_offset.value() != (*writer)->next_offset ||
        options.packet_sequence.value() != (*writer)->next_packet_sequence) {
        return absl::FailedPreconditionError("append offset or packet sequence mismatch");
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<ssize_t>::max()) ||
        (*writer)->next_offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return absl::OutOfRangeError("append exceeds platform limits");
    }
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::pwrite((*writer)->descriptor,
                                       data.data() + written,
                                       data.size() - written,
                                       static_cast<off_t>((*writer)->next_offset + written));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("append WAL", (*writer)->path, errno);
        }
        if (count == 0) {
            return absl::InternalError("WAL append made no progress");
        }
        written += static_cast<size_t>(count);
    }
    (*writer)->metadata.last_packet_offset = (*writer)->next_offset;
    (*writer)->metadata.last_packet_size = data.size();
    (*writer)->metadata.last_packet_checksum = static_cast<uint32_t>(sstv2::codec::crc32c_u64(
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size())));
    (*writer)->next_offset += data.size();
    ++(*writer)->next_packet_sequence;
    return absl::OkStatus();
}

absl::StatusOr<SyncResult> LocalFileSystem::sync(WriterHandle handle) {
    auto writer = get_writer(handle);
    if (!writer.ok()) {
        return writer.status();
    }
    std::lock_guard lock((*writer)->mutex);
    if (NowMs() >= (*writer)->metadata.expires_at_ms) {
        return absl::FailedPreconditionError("local writer lease expired");
    }
    if (::fsync((*writer)->descriptor) != 0) {
        return ErrnoStatus("fsync WAL", (*writer)->path, errno);
    }
    (*writer)->metadata.durable_offset = (*writer)->next_offset;
    (*writer)->metadata.next_packet_sequence = (*writer)->next_packet_sequence;
    auto status = persist_metadata((*writer)->path, (*writer)->metadata);
    if (!status.ok()) {
        return status;
    }
    return SyncResult{
        .writer_epoch = WriterEpoch((*writer)->metadata.writer_epoch),
        .durable_offset = ByteOffset((*writer)->metadata.durable_offset),
    };
}

absl::StatusOr<ByteOffset> LocalFileSystem::durable_size(std::string_view path) {
    auto metadata = read_metadata(path, false);
    return metadata.ok() ? absl::StatusOr<ByteOffset>(ByteOffset(metadata->durable_offset))
                         : metadata.status();
}

absl::Status LocalFileSystem::read_durable(std::string_view path,
                                           ByteOffset offset,
                                           std::span<std::byte> destination) {
    auto metadata = read_metadata(path, false);
    if (!metadata.ok()) {
        return metadata.status();
    }
    if (offset.value() > metadata->durable_offset ||
        destination.size() > metadata->durable_offset - offset.value()) {
        return absl::OutOfRangeError("read exceeds the durable WAL boundary");
    }
    const std::string owned_path(path);
    const int descriptor = ::open(owned_path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return ErrnoStatus("open WAL for durable read", path, errno);
    }
    size_t read = 0;
    while (read < destination.size()) {
        const ssize_t count = ::pread(descriptor,
                                      destination.data() + read,
                                      destination.size() - read,
                                      static_cast<off_t>(offset.value() + read));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto status = ErrnoStatus("read durable WAL", path, errno);
            (void)::close(descriptor);
            return status;
        }
        if (count == 0) {
            (void)::close(descriptor);
            return absl::DataLossError("durable WAL prefix is shorter than published metadata");
        }
        read += static_cast<size_t>(count);
    }
    if (::close(descriptor) != 0) {
        return ErrnoStatus("close durable WAL", path, errno);
    }
    return absl::OkStatus();
}

absl::Status LocalFileSystem::close_writer(WriterHandle handle, bool seal_writer) {
    std::shared_ptr<OpenWriter> writer;
    {
        std::lock_guard lock(mutex_);
        const auto it = writers_.find(handle);
        if (it == writers_.end()) {
            return absl::FailedPreconditionError("writer handle is closed or invalid");
        }
        writer = std::move(it->second);
        writers_.erase(it);
    }
    std::lock_guard lock(writer->mutex);
    absl::Status status;
    if (seal_writer && NowMs() >= writer->metadata.expires_at_ms) {
        status = absl::FailedPreconditionError("local writer lease expired");
    } else if (seal_writer) {
        if (::fsync(writer->descriptor) != 0) {
            status = ErrnoStatus("fsync WAL before seal", writer->path, errno);
        } else {
            writer->metadata.durable_offset = writer->next_offset;
            writer->metadata.next_packet_sequence = writer->next_packet_sequence;
            writer->metadata.sealed = true;
            status = persist_metadata(writer->path, writer->metadata);
        }
    }
    const int descriptor = std::exchange(writer->descriptor, -1);
    (void)::flock(descriptor, LOCK_UN);
    if (::close(descriptor) != 0 && status.ok()) {
        status = ErrnoStatus("close WAL writer", writer->path, errno);
    }
    return status;
}

absl::Status LocalFileSystem::seal(WriterHandle handle) {
    return close_writer(handle, true);
}

absl::Status LocalFileSystem::release_writer(WriterHandle handle) {
    return close_writer(handle, false);
}

} // namespace pl::minivessel
