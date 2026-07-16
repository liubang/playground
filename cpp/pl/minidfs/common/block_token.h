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
// Created: 2026/07/16 11:30

#pragma once

#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "cpp/pl/minidfs/common/time_util.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {

// Token permissions for DataNode data-plane operations.
enum class BlockTokenPermission : uint32_t {
    kRead = 1u << 0,
    kWrite = 1u << 1,
    kTransfer = 1u << 2,
    kTruncate = 1u << 3,
};

constexpr uint32_t kBlockTokenPermissionRead =
    static_cast<uint32_t>(BlockTokenPermission::kRead);
constexpr uint32_t kBlockTokenPermissionWrite =
    static_cast<uint32_t>(BlockTokenPermission::kWrite);
constexpr uint32_t kBlockTokenPermissionTransfer =
    static_cast<uint32_t>(BlockTokenPermission::kTransfer);
constexpr uint32_t kBlockTokenPermissionTruncate =
    static_cast<uint32_t>(BlockTokenPermission::kTruncate);

inline uint64_t default_block_token_ttl_ms() {
    return 5 * 60 * 1000; // 5 minutes
}

inline std::string configured_block_token_secret(std::string_view flag_value = {}) {
    if (!flag_value.empty()) {
        return std::string(flag_value);
    }
    const char* env = std::getenv("MINIDFS_BLOCK_TOKEN_SECRET");
    return env != nullptr ? std::string(env) : std::string{};
}

inline bool has_token_permission(uint32_t permissions, BlockTokenPermission permission) {
    return (permissions & static_cast<uint32_t>(permission)) != 0;
}

inline uint32_t all_data_plane_permissions() {
    return kBlockTokenPermissionRead | kBlockTokenPermissionWrite | kBlockTokenPermissionTransfer |
           kBlockTokenPermissionTruncate;
}

inline std::string block_token_payload(uint64_t block_id,
                                       uint64_t generation_stamp,
                                       uint64_t inode_id,
                                       uint32_t block_index,
                                       uint32_t permissions,
                                       uint64_t expires_at_ms) {
    return std::to_string(block_id) + "|" + std::to_string(generation_stamp) + "|" +
           std::to_string(inode_id) + "|" + std::to_string(block_index) + "|" +
           std::to_string(permissions) + "|" + std::to_string(expires_at_ms);
}

inline std::string block_token_signature(const std::string& secret,
                                         uint64_t block_id,
                                         uint64_t generation_stamp,
                                         uint64_t inode_id,
                                         uint32_t block_index,
                                         uint32_t permissions,
                                         uint64_t expires_at_ms) {
    const std::string payload = block_token_payload(
        block_id, generation_stamp, inode_id, block_index, permissions, expires_at_ms);

    unsigned int digest_len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    if (HMAC(EVP_sha256(),
             reinterpret_cast<const unsigned char*>(secret.data()),
             static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(payload.data()),
             payload.size(),
             digest,
             &digest_len) == nullptr) {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(digest), digest_len);
}

inline protocol::BlockTokenProto issue_block_token(uint64_t block_id,
                                                   uint64_t generation_stamp,
                                                   uint64_t inode_id,
                                                   uint32_t block_index,
                                                   uint32_t permissions,
                                                   uint64_t ttl_ms,
                                                   const std::string& secret,
                                                   uint64_t now = 0) {
    if (now == 0) {
        now = now_ms();
    }
    protocol::BlockTokenProto token;
    token.set_block_id(block_id);
    token.set_generation_stamp(generation_stamp);
    token.set_inode_id(inode_id);
    token.set_block_index(block_index);
    token.set_permissions(permissions);
    token.set_expires_at_ms(now + ttl_ms);
    token.set_signature(block_token_signature(secret,
                                              block_id,
                                              generation_stamp,
                                              inode_id,
                                              block_index,
                                              permissions,
                                              token.expires_at_ms()));
    return token;
}

inline bool block_token_needs_refresh(const protocol::BlockTokenProto& token,
                                      uint64_t now,
                                      uint64_t refresh_window_ms) {
    return token.expires_at_ms() <= now || token.expires_at_ms() - now <= refresh_window_ms;
}

inline bool constant_time_equals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

inline bool verify_block_token(const protocol::BlockTokenProto& token,
                               const std::string& secret,
                               BlockTokenPermission required_permission,
                               uint64_t expected_block_id,
                               uint64_t expected_generation_stamp,
                               uint64_t expected_inode_id,
                               uint32_t expected_block_index,
                               uint64_t now = 0) {
    if (token.block_id() != expected_block_id ||
        token.generation_stamp() != expected_generation_stamp ||
        token.inode_id() != expected_inode_id || token.block_index() != expected_block_index) {
        return false;
    }

    if (!has_token_permission(token.permissions(), required_permission)) {
        return false;
    }

    if (now == 0) {
        now = now_ms();
    }
    if (token.expires_at_ms() <= now) {
        return false;
    }

    const std::string expected_signature = block_token_signature(secret,
                                                                 token.block_id(),
                                                                 token.generation_stamp(),
                                                                 token.inode_id(),
                                                                 token.block_index(),
                                                                 token.permissions(),
                                                                 token.expires_at_ms());
    if (expected_signature.empty()) {
        return false;
    }
    return constant_time_equals(token.signature(), expected_signature);
}

} // namespace pl::minidfs
