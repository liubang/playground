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
// Created: 2026/05/10 17:45

#include "cpp/pl/minidfs/datanode/local_block_store.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

namespace fs = std::filesystem;

namespace {

inline pl::Status make_io_error(std::string_view msg) {
    return pl::Status(static_cast<pl::status_code_t>(ErrorCode::kIOError), msg);
}

inline pl::Status make_not_found(std::string_view msg) {
    return pl::Status(static_cast<pl::status_code_t>(ErrorCode::kFileNotFound), msg);
}

inline pl::Status make_checksum_error(std::string_view msg) {
    return pl::Status(static_cast<pl::status_code_t>(ErrorCode::kChecksumMismatch), msg);
}

} // namespace

// Construction & Initialization

LocalBlockStore::LocalBlockStore(Config config)
    : config_(std::move(config)),
      root_path_(config_.storage_root),
      tmp_path_(root_path_ / "tmp"),
      current_path_(root_path_ / "current"),
      trash_path_(root_path_ / "trash") {}

pl::Result<pl::Void> LocalBlockStore::init() {
    std::error_code ec;
    fs::create_directories(tmp_path_, ec);
    if (ec) {
        return pl::makeError(
            make_io_error(fmt::format("failed to create tmp directory: {}", ec.message())));
    }
    fs::create_directories(current_path_, ec);
    if (ec) {
        return pl::makeError(
            make_io_error(fmt::format("failed to create current directory: {}", ec.message())));
    }
    fs::create_directories(trash_path_, ec);
    if (ec) {
        return pl::makeError(
            make_io_error(fmt::format("failed to create trash directory: {}", ec.message())));
    }
    RETURN_VOID;
}

// Block Filename Utilities

std::string LocalBlockStore::block_filename(uint64_t block_id, uint64_t generation_stamp) {
    return fmt::format("blk_{}_{}.blk", block_id, generation_stamp);
}

fs::path LocalBlockStore::block_path(std::string_view subdir,
                                     uint64_t block_id,
                                     uint64_t generation_stamp) const {
    return root_path_ / subdir / block_filename(block_id, generation_stamp);
}

// Header I/O

pl::Result<BlockHeader> LocalBlockStore::read_header(const fs::path& path) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return pl::makeError(
            make_not_found(fmt::format("cannot open block file: {}", path.string())));
    }

    BlockHeader header{};
    ifs.read(reinterpret_cast<char*>(&header), sizeof(BlockHeader));
    if (!ifs.good()) {
        return pl::makeError(
            make_io_error(fmt::format("failed to read header from: {}", path.string())));
    }

    if (!validate_block_header(header)) {
        return pl::makeError(
            make_io_error(fmt::format("invalid block header in: {}", path.string())));
    }

    return header;
}

pl::Result<pl::Void> LocalBlockStore::write_header(const fs::path& path,
                                                   const BlockHeader& header) const {
    std::ofstream ofs(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!ofs.is_open()) {
        // File may not exist yet, try creating
        ofs.open(path, std::ios::binary | std::ios::out);
        if (!ofs.is_open()) {
            return pl::makeError(
                make_io_error(fmt::format("cannot open file for writing: {}", path.string())));
        }
    }
    ofs.seekp(0);
    ofs.write(reinterpret_cast<const char*>(&header), sizeof(BlockHeader));
    if (!ofs.good()) {
        return pl::makeError(
            make_io_error(fmt::format("failed to write header to: {}", path.string())));
    }
    ofs.flush();
    RETURN_VOID;
}

// Block Lifecycle

pl::Result<pl::Void> LocalBlockStore::create_block(uint64_t block_id,
                                                   uint64_t inode_id,
                                                   uint32_t block_index,
                                                   uint64_t generation_stamp) {
    std::lock_guard lock(mu_);

    auto path = block_path("tmp", block_id, generation_stamp);
    if (fs::exists(path)) {
        auto header_result = read_header(path);
        if (header_result.hasError()) {
            return pl::makeError(std::move(header_result.error()));
        }
        const auto& header = header_result.value();
        if (header.block_id == block_id && header.inode_id == inode_id &&
            header.block_index == block_index && header.generation_stamp == generation_stamp) {
            RETURN_VOID;
        }
        return pl::makeError(
            pl::Status(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                       fmt::format("tmp block exists with different identity: {}", path.string())));
    }

    auto current_path = block_path("current", block_id, generation_stamp);
    if (fs::exists(current_path)) {
        auto header_result = read_header(current_path);
        if (header_result.hasError()) {
            return pl::makeError(std::move(header_result.error()));
        }
        const auto& header = header_result.value();
        if (header.block_id == block_id && header.inode_id == inode_id &&
            header.block_index == block_index && header.generation_stamp == generation_stamp) {
            RETURN_VOID;
        }
        return pl::makeError(
            pl::Status(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                       fmt::format("current block exists with different identity: {}",
                                   current_path.string())));
    }

    // Initialize header
    BlockHeader header{};
    init_block_header(header, block_id, inode_id, block_index, generation_stamp);

    // Create file with header
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        return pl::makeError(
            make_io_error(fmt::format("cannot create block file: {}", path.string())));
    }
    ofs.write(reinterpret_cast<const char*>(&header), sizeof(BlockHeader));
    if (!ofs.good()) {
        return pl::makeError(
            make_io_error(fmt::format("failed to write initial header: {}", path.string())));
    }
    ofs.flush();
    RETURN_VOID;
}

pl::Result<uint64_t> LocalBlockStore::append_chunk(uint64_t block_id,
                                                   uint64_t generation_stamp,
                                                   const void* data,
                                                   uint32_t size,
                                                   uint32_t chunk_index) {
    std::lock_guard lock(mu_);

    auto path = block_path("tmp", block_id, generation_stamp);
    auto header_result = read_header(path);
    if (header_result.hasError()) {
        auto current = block_path("current", block_id, generation_stamp);
        if (fs::exists(current)) {
            auto current_header = read_header(current);
            if (current_header.hasError()) {
                return pl::makeError(std::move(current_header.error()));
            }
            uint32_t crc = compute_crc32c(data, size);
            const auto& header = current_header.value();
            if (chunk_index < header.chunk_count && header.chunk_checksums[chunk_index] == crc) {
                return header.data_length;
            }
            return pl::makeError(pl::Status(
                static_cast<pl::status_code_t>(ErrorCode::kBlockAlreadyCommitted),
                fmt::format("block {}:{} is already finalized", block_id, generation_stamp)));
        }
        return pl::makeError(std::move(header_result.error()));
    }

    auto header = header_result.value();

    // Compute CRC32C for this chunk (needed for both ordering check and idempotency)
    uint32_t crc = compute_crc32c(data, size);

    // Idempotency: if chunk_index == last written chunk and CRC matches, treat as retry.
    if (chunk_index < header.chunk_count) {
        if (chunk_index == header.chunk_count - 1 && header.chunk_checksums[chunk_index] == crc) {
            // Duplicate/retried write — return current data length (success, no-op).
            return header.data_length;
        }
        // chunk_index < chunk_count but CRC doesn't match or not the last chunk
        return pl::makeError(pl::Status(
            static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
            fmt::format("chunk_index {} already written with different data", chunk_index)));
    }

    // Ordering: chunk_index must be exactly the next expected chunk.
    if (chunk_index != header.chunk_count) {
        return pl::makeError(pl::Status(
            static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
            fmt::format("expected chunk_index {} but got {}", header.chunk_count, chunk_index)));
    }

    if (header.chunk_count >= kMaxChunkCount) {
        return pl::makeError(pl::Status(static_cast<pl::status_code_t>(ErrorCode::kIOError),
                                        "max chunk count reached"));
    }

    // Open file to get actual data region size
    std::fstream fs_file(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
    if (!fs_file.is_open()) {
        return pl::makeError(
            make_io_error(fmt::format("cannot open tmp block for append: {}", path.string())));
    }

    auto file_size = static_cast<uint64_t>(fs_file.tellp());
    uint32_t data_offset = static_cast<uint32_t>(file_size - kBlockHeaderSize);

    // Write chunk data at end of file
    fs_file.write(reinterpret_cast<const char*>(data), size);
    if (!fs_file.good()) {
        return pl::makeError(make_io_error("failed to append chunk data"));
    }

    // Update header
    header.chunk_offsets[header.chunk_count] = data_offset;
    header.chunk_checksums[header.chunk_count] = crc;
    header.chunk_count++;
    header.data_length += size;

    // Recompute whole-block checksum (extend incrementally)
    if (header.chunk_count == 1) {
        header.block_checksum = crc;
    } else {
        // Re-extend from existing block checksum
        header.block_checksum = extend_crc32c(header.block_checksum, data, size);
    }

    // Write updated header back
    fs_file.seekp(0);
    fs_file.write(reinterpret_cast<const char*>(&header), sizeof(BlockHeader));
    if (!fs_file.good()) {
        return pl::makeError(make_io_error("failed to update header after append"));
    }
    fs_file.flush();

    return header.data_length;
}

pl::Result<pl::Void> LocalBlockStore::finalize_block(uint64_t block_id, uint64_t generation_stamp) {
    std::lock_guard lock(mu_);

    auto src = block_path("tmp", block_id, generation_stamp);
    if (!fs::exists(src)) {
        auto current = block_path("current", block_id, generation_stamp);
        if (fs::exists(current)) {
            auto verify = verify_block_file(current);
            if (verify.hasError()) {
                return folly::makeUnexpected(verify.error());
            }
            if (!verify.value()) {
                return pl::makeError(make_checksum_error(
                    fmt::format("finalized block checksum mismatch: {}", current.string())));
            }
            RETURN_VOID;
        }
        return pl::makeError(make_not_found(fmt::format("tmp block not found: {}", src.string())));
    }

    auto verify = verify_block_file(src);
    if (verify.hasError()) {
        return folly::makeUnexpected(verify.error());
    }
    if (!verify.value()) {
        return pl::makeError(
            make_checksum_error(fmt::format("tmp block checksum mismatch: {}", src.string())));
    }

    auto dst = block_path("current", block_id, generation_stamp);
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (ec) {
        return pl::makeError(
            make_io_error(fmt::format("failed to finalize block: {}", ec.message())));
    }
    RETURN_VOID;
}

pl::Result<pl::Void> LocalBlockStore::delete_block(uint64_t block_id, uint64_t generation_stamp) {
    std::lock_guard lock(mu_);

    auto src = block_path("current", block_id, generation_stamp);
    if (!fs::exists(src)) {
        return pl::makeError(
            make_not_found(fmt::format("block not found in current: {}", src.string())));
    }

    auto dst = block_path("trash", block_id, generation_stamp);
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (ec) {
        return pl::makeError(
            make_io_error(fmt::format("failed to move block to trash: {}", ec.message())));
    }
    RETURN_VOID;
}

pl::Result<pl::Void> LocalBlockStore::truncate_block(uint64_t block_id,
                                                    uint64_t generation_stamp,
                                                    uint64_t length) {
    std::lock_guard lock(mu_);

    auto path = block_path("current", block_id, generation_stamp);
    auto header_result = read_header(path);
    if (header_result.hasError()) {
        return folly::makeUnexpected(header_result.error());
    }
    const auto original_header = header_result.value();
    auto header = original_header;
    if (length > header.data_length) {
        return pl::makeError(pl::Status(
            static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
            fmt::format("truncate length {} exceeds block length {}", length, header.data_length)));
    }
    if (length == header.data_length) {
        RETURN_VOID;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return pl::makeError(make_not_found(path.string()));
    }
    ifs.seekg(kBlockHeaderSize);
    std::string data(length, '\0');
    if (length > 0) {
        ifs.read(data.data(), static_cast<std::streamsize>(length));
        if (!ifs.good()) {
            return pl::makeError(make_io_error("failed to read block prefix for truncate"));
        }
    }

    std::memset(header.chunk_offsets, 0, sizeof(header.chunk_offsets));
    std::memset(header.chunk_checksums, 0, sizeof(header.chunk_checksums));
    uint32_t retained_chunks = 0;
    for (uint32_t i = 0;
         i < original_header.chunk_count && original_header.chunk_offsets[i] < length;
         ++i) {
        uint32_t offset = original_header.chunk_offsets[i];
        uint64_t original_end =
            i + 1 < original_header.chunk_count ? original_header.chunk_offsets[i + 1]
                                                : original_header.data_length;
        uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(original_end, length));
        header.chunk_offsets[retained_chunks] = offset;
        header.chunk_checksums[retained_chunks] = compute_crc32c(data.data() + offset, end - offset);
        ++retained_chunks;
    }
    header.chunk_count = retained_chunks;
    header.data_length = length;
    header.block_checksum = compute_crc32c(data.data(), data.size());

    auto replacement = tmp_path_ / path.filename();
    std::ofstream ofs(replacement, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return pl::makeError(
            make_io_error(fmt::format("cannot create truncated block: {}", replacement.string())));
    }
    ofs.write(reinterpret_cast<const char*>(&header), sizeof(BlockHeader));
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    ofs.flush();
    if (!ofs.good()) {
        std::error_code ec;
        fs::remove(replacement, ec);
        return pl::makeError(
            make_io_error(fmt::format("failed to write truncated block: {}", replacement.string())));
    }
    ofs.close();

    std::error_code ec;
    fs::rename(replacement, path, ec);
    if (ec) {
        fs::remove(replacement, ec);
        return pl::makeError(
            make_io_error(fmt::format("failed to install truncated block: {}", path.string())));
    }
    RETURN_VOID;
}

pl::Result<uint32_t> LocalBlockStore::purge_trash() {
    std::lock_guard lock(mu_);

    uint32_t count = 0;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(trash_path_, ec)) {
        if (entry.is_regular_file()) {
            fs::remove(entry.path(), ec);
            if (!ec) {
                ++count;
            }
        }
    }
    return count;
}

// Block Reading

pl::Result<std::string> LocalBlockStore::read_block_data(uint64_t block_id,
                                                         uint64_t generation_stamp) {
    auto path = block_path("current", block_id, generation_stamp);
    auto header_result = read_header(path);
    if (header_result.hasError()) {
        return pl::makeError(std::move(header_result.error()));
    }

    const auto& header = header_result.value();
    if (header.chunk_count == 0) {
        return std::string{};
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return pl::makeError(make_not_found(path.string()));
    }

    // Seek past header to data region
    ifs.seekg(kBlockHeaderSize);

    // Calculate total data region size on disk
    // = file_size - header_size
    ifs.seekg(0, std::ios::end);
    auto file_size = static_cast<uint64_t>(ifs.tellg());
    if (file_size < kBlockHeaderSize) {
        return pl::makeError(make_io_error("block file is smaller than header"));
    }
    uint64_t data_region_size = file_size - kBlockHeaderSize;

    ifs.seekg(kBlockHeaderSize);
    std::string data(data_region_size, '\0');
    ifs.read(data.data(), static_cast<std::streamsize>(data_region_size));
    if (!ifs.good()) {
        return pl::makeError(make_io_error("failed to read block data"));
    }

    if (data_region_size != header.data_length) {
        return pl::makeError(
            make_io_error(fmt::format("block data length mismatch: header={}, file={}",
                                      header.data_length,
                                      data_region_size)));
    }

    for (uint32_t i = 0; i < header.chunk_count; ++i) {
        uint32_t offset = header.chunk_offsets[i];
        uint32_t end = (i + 1 < header.chunk_count) ? header.chunk_offsets[i + 1]
                                                    : static_cast<uint32_t>(data.size());
        if (end < offset || end > data.size()) {
            return pl::makeError(
                make_io_error(fmt::format("invalid chunk offsets for chunk {}", i)));
        }
        uint32_t crc = compute_crc32c(data.data() + offset, end - offset);
        if (crc != header.chunk_checksums[i]) {
            return pl::makeError(
                make_checksum_error(fmt::format("chunk {} CRC mismatch: expected={:#x}, got={:#x}",
                                                i,
                                                header.chunk_checksums[i],
                                                crc)));
        }
    }

    uint32_t block_crc = compute_crc32c(data.data(), data.size());
    if (block_crc != header.block_checksum) {
        return pl::makeError(make_checksum_error(fmt::format(
            "block CRC mismatch: expected={:#x}, got={:#x}", header.block_checksum, block_crc)));
    }

    return data;
}

pl::Result<std::string> LocalBlockStore::read_chunk(uint64_t block_id,
                                                    uint64_t generation_stamp,
                                                    uint32_t chunk_index) {
    auto path = block_path("current", block_id, generation_stamp);
    auto header_result = read_header(path);
    if (header_result.hasError()) {
        return pl::makeError(std::move(header_result.error()));
    }

    const auto& header = header_result.value();
    if (chunk_index >= header.chunk_count) {
        return pl::makeError(pl::Status(
            static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
            fmt::format("chunk_index {} >= chunk_count {}", chunk_index, header.chunk_count)));
    }

    // Determine chunk offset and size
    uint32_t offset = header.chunk_offsets[chunk_index];
    uint32_t chunk_size = 0;
    if (chunk_index + 1 < header.chunk_count) {
        chunk_size = header.chunk_offsets[chunk_index + 1] - offset;
    } else {
        // Last chunk: size = file_size - header - offset
        std::ifstream ifs_size(path, std::ios::binary | std::ios::ate);
        auto file_size = static_cast<uint64_t>(ifs_size.tellg());
        chunk_size = static_cast<uint32_t>(file_size - kBlockHeaderSize - offset);
    }

    std::ifstream ifs(path, std::ios::binary);
    ifs.seekg(static_cast<std::streamoff>(kBlockHeaderSize + offset));

    std::string chunk_data(chunk_size, '\0');
    ifs.read(chunk_data.data(), chunk_size);
    if (!ifs.good()) {
        return pl::makeError(make_io_error("failed to read chunk"));
    }

    // Verify checksum
    uint32_t crc = compute_crc32c(chunk_data.data(), chunk_data.size());
    if (crc != header.chunk_checksums[chunk_index]) {
        return pl::makeError(
            make_checksum_error(fmt::format("chunk {} CRC mismatch: expected={:#x}, got={:#x}",
                                            chunk_index,
                                            header.chunk_checksums[chunk_index],
                                            crc)));
    }

    return chunk_data;
}

pl::Result<bool> LocalBlockStore::verify_block(uint64_t block_id, uint64_t generation_stamp) {
    auto path = block_path("current", block_id, generation_stamp);
    return verify_block_file(path);
}

pl::Result<bool> LocalBlockStore::verify_block_file(const fs::path& path) const {
    auto header_result = read_header(path);
    if (header_result.hasError()) {
        return pl::makeError(std::move(header_result.error()));
    }

    const auto& header = header_result.value();
    if (header.chunk_count == 0) {
        return true;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return pl::makeError(make_not_found(path.string()));
    }
    ifs.seekg(0, std::ios::end);
    auto file_size = static_cast<uint64_t>(ifs.tellg());
    if (file_size < kBlockHeaderSize || file_size - kBlockHeaderSize != header.data_length) {
        return false;
    }

    // Verify each chunk's CRC individually.
    ifs.seekg(kBlockHeaderSize);

    for (uint32_t i = 0; i < header.chunk_count; ++i) {
        uint32_t chunk_size = 0;
        if (i + 1 < header.chunk_count) {
            if (header.chunk_offsets[i + 1] < header.chunk_offsets[i]) {
                return false;
            }
            chunk_size = header.chunk_offsets[i + 1] - header.chunk_offsets[i];
        } else {
            if (header.chunk_offsets[i] > header.data_length) {
                return false;
            }
            chunk_size = static_cast<uint32_t>(header.data_length - header.chunk_offsets[i]);
        }
        ifs.seekg(static_cast<std::streamoff>(kBlockHeaderSize + header.chunk_offsets[i]));

        std::string buf(chunk_size, '\0');
        ifs.read(buf.data(), chunk_size);
        if (!ifs.good()) {
            return false;
        }

        uint32_t crc = compute_crc32c(buf.data(), buf.size());
        if (crc != header.chunk_checksums[i]) {
            return false;
        }
    }

    ifs.seekg(kBlockHeaderSize);
    std::string all_data(header.data_length, '\0');
    ifs.read(all_data.data(), static_cast<std::streamsize>(all_data.size()));
    if (!ifs.good()) {
        return false;
    }
    return compute_crc32c(all_data.data(), all_data.size()) == header.block_checksum;
}

// Block Reporting

pl::Result<std::vector<BlockInfo>> LocalBlockStore::report_blocks() {
    std::lock_guard lock(mu_);

    std::vector<BlockInfo> blocks;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(current_path_, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        auto header_result = read_header(entry.path());
        if (header_result.hasError()) {
            continue; // Skip corrupt/unreadable files
        }

        const auto& header = header_result.value();
        BlockInfo info{};
        info.block_id = header.block_id;
        info.generation_stamp = header.generation_stamp;
        info.length = header.data_length;
        blocks.push_back(info);
    }

    return blocks;
}

bool LocalBlockStore::has_block(uint64_t block_id, uint64_t generation_stamp) const {
    auto path = block_path("current", block_id, generation_stamp);
    return fs::exists(path);
}

pl::Result<uint64_t> LocalBlockStore::available_bytes() const {
    std::error_code ec;
    auto space = fs::space(root_path_, ec);
    if (ec) {
        return pl::makeError(
            make_io_error(fmt::format("failed to query disk space: {}", ec.message())));
    }
    uint64_t available = space.available;
    if (available <= config_.reserved_bytes) {
        return uint64_t{0};
    }
    return available - config_.reserved_bytes;
}

} // namespace pl::minidfs
