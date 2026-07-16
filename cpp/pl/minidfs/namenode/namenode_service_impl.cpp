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
// Created: 2026/05/10 18:30

#include "cpp/pl/minidfs/namenode/namenode_service_impl.h"

#include <algorithm>
#include <brpc/channel.h>
#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

#include "cpp/pl/minidfs/common/block_token.h"
#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

namespace {

void set_status(protocol::StatusProto* proto, uint32_t code, std::string_view msg = {}) {
    proto->set_code(code);
    if (!msg.empty()) {
        proto->set_message(std::string(msg));
    }
}

pl::Result<pl::Void> truncate_replica(MetadataStore* store,
                                      const protocol::BlockTokenProto& block_token,
                                      const BlockReplica& replica,
                                      uint64_t length) {
    auto datanode = store->get_datanode(replica.datanode_id);
    if (datanode.hasError()) {
        return folly::makeUnexpected(datanode.error());
    }
    if (datanode.value().state != DataNodeState::kLive) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNoAvailableDataNode),
                             "cannot truncate replica on non-live datanode");
    }

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 10000;
    options.max_retry = 1;
    std::string host =
        datanode.value().ip.empty() ? datanode.value().hostname : datanode.value().ip;
    std::string address = host + ":" + std::to_string(datanode.value().data_port);
    if (channel.Init(address.c_str(), &options) != 0) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCConnectFailed),
                             "failed to connect to datanode for truncate");
    }

    protocol::DataTransferService_Stub stub(&channel);
    brpc::Controller controller;
    protocol::TruncateBlockRequest request;
    request.set_block_id(replica.block_id);
    request.set_generation_stamp(replica.generation_stamp);
    request.set_length(length);
    *request.mutable_block_token() = block_token;
    protocol::TruncateBlockResponse response;
    stub.TruncateBlock(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kRPCError),
                             controller.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<pl::status_code_t>(response.status().code()),
                             response.status().message());
    }
    return pl::Void{};
}

std::string escape_json(std::string_view input) {
    std::string out;
    out.reserve(input.size() + input.size() / 8);
    for (char c : input) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::string make_oplog_payload(const protocol::RequestHeader& header,
                               std::string_view op_type,
                               uint64_t target_inode_id,
                               uint32_t status_code,
                               std::string_view status_message,
                               std::string_view response_payload_json) {
    return fmt::format(
        "{{\"request_id\":\"{}\",\"client_id\":\"{}\",\"user\":\"{}\","
        "\"op_type\":\"{}\",\"target_inode_id\":{},\"status_code\":{},"
        "\"status_message\":\"{}\",\"response\":{}}}",
        escape_json(header.request_id()),
        escape_json(header.client_id()),
        escape_json(header.user()),
        escape_json(op_type),
        target_inode_id,
        status_code,
        escape_json(status_message),
        response_payload_json);
}

bool parse_json_u64(std::string_view json, std::string_view key, uint64_t* value) {
    const std::string pattern = std::string("\"") + std::string(key) + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    size_t end = pos;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') {
        ++end;
    }
    if (end == pos) {
        return false;
    }
    *value = std::strtoull(std::string(json.substr(pos, end - pos)).c_str(), nullptr, 10);
    return true;
}

bool parse_json_string(std::string_view json, std::string_view key, std::string* value) {
    const std::string pattern = std::string("\"") + std::string(key) + "\":\"";
    size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos += pattern.size();
    std::string out;
    out.reserve(32);
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\') {
            if (pos >= json.size()) {
                return false;
            }
            char esc = json[pos++];
            switch (esc) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                default:
                    out.push_back(esc);
                    break;
            }
            continue;
        }
        if (c == '"') {
            *value = std::move(out);
            return true;
        }
        out.push_back(c);
    }
    return false;
}

bool extract_json_object(std::string_view json,
                         std::string_view key,
                         std::string* object_json,
                         char open = '{',
                         char close = '}') {
    const std::string pattern = std::string("\"") + std::string(key) + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != open) {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t start = pos;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == open) {
            ++depth;
            continue;
        }
        if (c == close) {
            --depth;
            if (depth == 0) {
                *object_json = std::string(json.substr(start, pos - start + 1));
                return true;
            }
        }
    }
    return false;
}

bool parse_response_status(const OplogEntry& entry,
                           std::string_view expected_op,
                           uint32_t* status_code,
                           std::string* status_message,
                           uint64_t* target_inode_id,
                           std::string* response_json) {
    if (entry.op_type != expected_op) {
        return false;
    }
    uint64_t status_code_u64 = 0;
    if (!parse_json_u64(entry.payload_json, "status_code", &status_code_u64)) {
        return false;
    }
    if (!parse_json_u64(entry.payload_json, "target_inode_id", target_inode_id)) {
        return false;
    }
    if (!parse_json_string(entry.payload_json, "status_message", status_message)) {
        status_message->clear();
    }
    if (!extract_json_object(entry.payload_json, "response", response_json)) {
        return false;
    }
    *status_code = static_cast<uint32_t>(status_code_u64);
    return true;
}

bool parse_located_block(std::string_view response_json, protocol::LocatedBlockProto* block) {
    std::string block_json;
    if (!extract_json_object(response_json, "block", &block_json)) {
        return false;
    }

    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!parse_json_u64(block_json, "block_id", &block_id) ||
        !parse_json_u64(block_json, "generation_stamp", &generation_stamp) ||
        !parse_json_u64(block_json, "offset", &offset) ||
        !parse_json_u64(block_json, "length", &length)) {
        return false;
    }

    block->set_block_id(block_id);
    block->set_generation_stamp(generation_stamp);
    block->set_offset(offset);
    block->set_length(length);

    std::string locations_json;
    if (!extract_json_object(block_json, "locations", &locations_json, '[', ']')) {
        return true;
    }

    size_t pos = 1;
    while (pos < locations_json.size()) {
        while (pos < locations_json.size() &&
               (locations_json[pos] == ' ' || locations_json[pos] == ',')) {
            ++pos;
        }
        if (pos >= locations_json.size() || locations_json[pos] == ']') {
            break;
        }
        if (locations_json[pos] != '{') {
            return false;
        }

        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        size_t start = pos;
        for (; pos < locations_json.size(); ++pos) {
            char c = locations_json[pos];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '{') {
                ++depth;
                continue;
            }
            if (c == '}') {
                --depth;
                if (depth == 0) {
                    std::string_view loc_json(locations_json.data() + start, pos - start + 1);
                    uint64_t datanode_id = 0;
                    uint64_t data_port = 0;
                    std::string host;
                    if (!parse_json_u64(loc_json, "datanode_id", &datanode_id) ||
                        !parse_json_u64(loc_json, "data_port", &data_port) ||
                        !parse_json_string(loc_json, "host", &host)) {
                        return false;
                    }
                    auto* location = block->add_locations();
                    location->set_datanode_id(datanode_id);
                    location->set_data_port(static_cast<uint32_t>(data_port));
                    location->set_host(host);
                    ++pos;
                    break;
                }
            }
        }
    }

    return true;
}

std::string located_block_to_json(const LocatedBlock& block) {
    std::string locations = "[";
    for (size_t i = 0; i < block.locations.size(); ++i) {
        if (i > 0) {
            locations += ",";
        }
        locations += fmt::format("{{\"datanode_id\":{},\"host\":\"{}\",\"data_port\":{}}}",
                                 block.locations[i].datanode_id,
                                 escape_json(block.locations[i].host),
                                 block.locations[i].data_port);
    }
    locations += "]";
    return fmt::format(
        "{{\"block\":{{\"block_id\":{},\"generation_stamp\":{},\"offset\":{},"
        "\"length\":{},\"locations\":{}}}}}",
        block.block_id,
        block.generation_stamp,
        block.offset,
        block.length,
        locations);
}

bool rebuild_allocate_block_from_metadata(MetadataStore* store,
                                          uint64_t inode_id,
                                          uint32_t block_index,
                                          LocatedBlock* out) {
    auto blocks = store->get_blocks_by_inode(inode_id);
    if (blocks.hasError()) {
        return false;
    }

    for (const auto& block : blocks.value()) {
        if (block.block_index != block_index || block.state == BlockState::kDeleted) {
            continue;
        }

        out->block_id = block.block_id;
        out->generation_stamp = block.generation_stamp;
        out->offset = 0;
        out->length = block.length;
        out->locations.clear();

        auto replicas = store->get_replicas(block.block_id);
        if (replicas.hasError()) {
            return false;
        }
        for (const auto& replica : replicas.value()) {
            if (replica.state == ReplicaState::kDeleted || replica.state == ReplicaState::kCorrupt) {
                continue;
            }
            auto datanode = store->get_datanode(replica.datanode_id);
            if (datanode.hasError()) {
                continue;
            }
            out->locations.push_back(DataNodeEndpoint{
                .datanode_id = datanode.value().datanode_id,
                .host = datanode.value().ip.empty() ? datanode.value().hostname : datanode.value().ip,
                .data_port = datanode.value().data_port,
            });
        }
        return !out->locations.empty();
    }

    return false;
}

} // namespace

// Helper utilities
void NameNodeServiceImpl::fill_status(protocol::StatusProto* proto,
                                      uint32_t code,
                                      std::string_view msg) {
    set_status(proto, code, msg);
}

void NameNodeServiceImpl::fill_file_status(protocol::FileStatusProto* proto, const FileStatus& fs) {
    proto->set_inode_id(fs.inode_id);
    proto->set_path(fs.path);
    proto->set_is_dir(fs.is_dir);
    proto->set_length(fs.length);
    proto->set_replication(fs.replication);
    proto->set_block_size(fs.block_size);
    proto->set_mtime_ms(fs.mtime_ms);
    proto->set_owner(fs.owner);
    proto->set_group(fs.group);
    proto->set_permission(fs.permission);
}

void NameNodeServiceImpl::fill_located_block(protocol::LocatedBlockProto* proto,
                                             const LocatedBlock& lb) {
    proto->set_block_id(lb.block_id);
    proto->set_generation_stamp(lb.generation_stamp);
    proto->set_offset(lb.offset);
    proto->set_length(lb.length);
    for (const auto& loc : lb.locations) {
        auto* ep = proto->add_locations();
        ep->set_datanode_id(loc.datanode_id);
        ep->set_host(loc.host);
        ep->set_data_port(loc.data_port);
    }
    auto* token = proto->mutable_block_token();
    token->set_block_id(lb.block_token.block_id);
    token->set_generation_stamp(lb.block_token.generation_stamp);
    token->set_inode_id(lb.block_token.inode_id);
    token->set_block_index(lb.block_token.block_index);
    token->set_permissions(lb.block_token.permissions);
    token->set_expires_at_ms(lb.block_token.expires_at_ms);
    token->set_signature(lb.block_token.signature);
}

NameNodeServiceImpl::NameNodeServiceImpl(NamespaceManager* ns_mgr,
                                         BlockManager* block_mgr,
                                         LeaseManager* lease_mgr,
                                         MetadataStore* metadata_store,
                                         std::string block_token_secret,
                                         uint64_t block_token_ttl_ms)
    : ns_mgr_(ns_mgr),
      block_mgr_(block_mgr),
      lease_mgr_(lease_mgr),
      metadata_store_(metadata_store),
      block_token_secret_(std::move(block_token_secret)),
      block_token_ttl_ms_(block_token_ttl_ms) {}

protocol::BlockTokenProto NameNodeServiceImpl::issue_block_token(uint64_t block_id,
                                                                  uint64_t generation_stamp,
                                                                  uint64_t inode_id,
                                                                  uint32_t block_index,
                                                                  uint32_t permissions) const {
    return pl::minidfs::issue_block_token(block_id,
                                          generation_stamp,
                                          inode_id,
                                          block_index,
                                          permissions,
                                          block_token_ttl_ms_,
                                          block_token_secret_);
}

bool NameNodeServiceImpl::check_idempotent(const protocol::RequestHeader& header,
                                           protocol::StatusProto* status) {
    if (header.request_id().empty()) {
        return false;
    }
    auto result = metadata_store_->check_request_id(header.request_id());
    if (result.hasError()) {
        fill_status(status, result.error().code(), result.error().message());
        return true;
    }
    if (result.value()) {
        fill_status(status, 0);
        return true;
    }
    return false;
}

pl::Result<pl::Void> NameNodeServiceImpl::write_oplog(std::string_view op_type,
                                                    uint64_t target_inode_id,
                                                    const protocol::RequestHeader& header,
                                                    std::string_view payload_json) {
    if (header.request_id().empty()) {
        return pl::Void{};
    }
    return metadata_store_->write_oplog(op_type, target_inode_id, header.request_id(), payload_json);
}

// Namespace Operations
void NameNodeServiceImpl::Mkdir(google::protobuf::RpcController* /*controller*/,
                                const protocol::MkdirRequest* request,
                                protocol::MkdirResponse* response,
                                google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && !request->header().request_id().empty()) {
        auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
        if (existing.hasError()) {
            fill_status(response->mutable_status(), existing.error().code(), existing.error().message());
            return;
        }
        if (existing.value().has_value()) {
            uint32_t status_code = 0;
            uint64_t target_inode_id = 0;
            std::string status_message;
            std::string response_json;
            if (!parse_response_status(existing.value().value(),
                                       "mkdir",
                                       &status_code,
                                       &status_message,
                                       &target_inode_id,
                                       &response_json)) {
                fill_status(response->mutable_status(),
                            static_cast<uint32_t>(ErrorCode::kInternalError),
                            "invalid mkdir idempotency payload");
                return;
            }
            fill_status(response->mutable_status(), status_code, status_message);
            if (status_code == 0) {
                uint64_t inode_id = target_inode_id;
                (void)parse_json_u64(response_json, "inode_id", &inode_id);
                response->set_inode_id(inode_id);
            }
            return;
        }
    }

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }

    auto result = ns_mgr_->mkdir(request->path(),
                                 request->owner(),
                                 request->group(),
                                 request->permission(),
                                 /*create_parent=*/true);
    if (result.hasError()) {
        if (request->has_header() && !request->header().request_id().empty()) {
            auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
            if (existing.hasValue() && existing.value().has_value()) {
                uint32_t status_code = 0;
                uint64_t target_inode_id = 0;
                std::string status_message;
                std::string response_json;
                if (parse_response_status(existing.value().value(),
                                          "mkdir",
                                          &status_code,
                                          &status_message,
                                          &target_inode_id,
                                          &response_json)) {
                    fill_status(response->mutable_status(), status_code, status_message);
                    if (status_code == 0) {
                        uint64_t inode_id = target_inode_id;
                        (void)parse_json_u64(response_json, "inode_id", &inode_id);
                        response->set_inode_id(inode_id);
                    }
                    return;
                }
            }
        }
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header() && !request->header().request_id().empty()) {
        auto payload = make_oplog_payload(request->header(),
                                          "mkdir",
                                          result.value().inode_id,
                                          0,
                                          "",
                                          fmt::format("{{\"inode_id\":{}}}", result.value().inode_id));
        auto log = write_oplog("mkdir", result.value().inode_id, request->header(), payload);
        if (log.hasError()) {
            fill_status(response->mutable_status(), log.error().code(), log.error().message());
            return;
        }
    }

    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        fill_status(response->mutable_status(), commit.error().code(), commit.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    response->set_inode_id(result.value().inode_id);
}

void NameNodeServiceImpl::CreateFile(google::protobuf::RpcController* /*controller*/,
                                     const protocol::CreateFileRequest* request,
                                     protocol::CreateFileResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && !request->header().request_id().empty()) {
        auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
        if (existing.hasError()) {
            fill_status(response->mutable_status(), existing.error().code(), existing.error().message());
            return;
        }
        if (existing.value().has_value()) {
            uint32_t status_code = 0;
            uint64_t target_inode_id = 0;
            std::string status_message;
            std::string response_json;
            if (!parse_response_status(existing.value().value(),
                                       "create_file",
                                       &status_code,
                                       &status_message,
                                       &target_inode_id,
                                       &response_json)) {
                fill_status(response->mutable_status(),
                            static_cast<uint32_t>(ErrorCode::kInternalError),
                            "invalid create_file idempotency payload");
                return;
            }
            fill_status(response->mutable_status(), status_code, status_message);
            if (status_code == 0) {
                uint64_t inode_id = target_inode_id;
                uint64_t lease_id = 0;
                (void)parse_json_u64(response_json, "inode_id", &inode_id);
                (void)parse_json_u64(response_json, "lease_id", &lease_id);
                response->set_inode_id(inode_id);
                response->set_lease_id(lease_id);
            }
            return;
        }
    }

    uint32_t replication =
        request->replication() > 0 ? request->replication() : kDefaultReplication;
    uint64_t block_size = request->block_size() > 0 ? request->block_size() : kDefaultBlockSize;

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }

    if (request->overwrite()) {
        auto existing = ns_mgr_->resolve_path(request->path());
        if (existing.hasValue()) {
            if (existing.value().type != InodeType::kFile) {
                fill_status(response->mutable_status(),
                            static_cast<uint32_t>(ErrorCode::kIsDirectory),
                            "cannot overwrite a directory");
                return;
            }
            if (existing.value().state != FileState::kNormal) {
                fill_status(response->mutable_status(),
                            static_cast<uint32_t>(ErrorCode::kFileUnderConstruction),
                            "cannot overwrite a file under construction");
                return;
            }
            auto invalidate = block_mgr_->invalidate_blocks(existing.value().inode_id);
            if (invalidate.hasError()) {
                fill_status(response->mutable_status(),
                            invalidate.error().code(),
                            invalidate.error().message());
                return;
            }
            auto remove = ns_mgr_->remove(request->path());
            if (remove.hasError()) {
                fill_status(
                    response->mutable_status(), remove.error().code(), remove.error().message());
                return;
            }
        } else if (existing.error().code() !=
                   static_cast<pl::status_code_t>(ErrorCode::kNotFound)) {
            fill_status(
                response->mutable_status(), existing.error().code(), existing.error().message());
            return;
        }
    }

    auto result = ns_mgr_->create_file(request->path(),
                                       request->owner(),
                                       request->group(),
                                       request->permission(),
                                       replication,
                                       block_size);
    if (result.hasError()) {
        if (request->has_header() && !request->header().request_id().empty()) {
            auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
            if (existing.hasValue() && existing.value().has_value()) {
                uint32_t status_code = 0;
                uint64_t target_inode_id = 0;
                std::string status_message;
                std::string response_json;
                if (parse_response_status(existing.value().value(),
                                          "create_file",
                                          &status_code,
                                          &status_message,
                                          &target_inode_id,
                                          &response_json)) {
                    fill_status(response->mutable_status(), status_code, status_message);
                    if (status_code == 0) {
                        uint64_t inode_id = target_inode_id;
                        uint64_t lease_id = 0;
                        (void)parse_json_u64(response_json, "inode_id", &inode_id);
                        (void)parse_json_u64(response_json, "lease_id", &lease_id);
                        response->set_inode_id(inode_id);
                        response->set_lease_id(lease_id);
                    }
                    return;
                }
            }
        }
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    auto lease_result = lease_mgr_->acquire_lease(result.value().inode_id, request->client_id());
    if (lease_result.hasError()) {
        (void)ns_mgr_->remove(request->path());
        fill_status(response->mutable_status(),
                    lease_result.error().code(),
                    lease_result.error().message());
        return;
    }

    if (request->has_header() && !request->header().request_id().empty()) {
        auto payload = make_oplog_payload(
            request->header(),
            "create_file",
            result.value().inode_id,
            0,
            "",
            fmt::format("{{\"inode_id\":{},\"lease_id\":{}}}",
                        result.value().inode_id,
                        lease_result.value().lease_id));
        auto log = write_oplog("create_file", result.value().inode_id, request->header(), payload);
        if (log.hasError()) {
            auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
            if (existing.hasValue() && existing.value().has_value()) {
                uint32_t status_code = 0;
                uint64_t target_inode_id = 0;
                std::string status_message;
                std::string response_json;
                if (parse_response_status(existing.value().value(),
                                          "create_file",
                                          &status_code,
                                          &status_message,
                                          &target_inode_id,
                                          &response_json)) {
                    fill_status(response->mutable_status(), status_code, status_message);
                    if (status_code == 0) {
                        uint64_t inode_id = target_inode_id;
                        uint64_t lease_id = 0;
                        (void)parse_json_u64(response_json, "inode_id", &inode_id);
                        (void)parse_json_u64(response_json, "lease_id", &lease_id);
                        response->set_inode_id(inode_id);
                        response->set_lease_id(lease_id);
                    }
                    return;
                }
            }
            fill_status(response->mutable_status(), log.error().code(), log.error().message());
            return;
        }
    }

    auto commit_result = txn.value()->commit();
    if (commit_result.hasError()) {
        fill_status(response->mutable_status(),
                    commit_result.error().code(),
                    commit_result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    response->set_inode_id(result.value().inode_id);
    response->set_lease_id(lease_result.value().lease_id);
}

void NameNodeServiceImpl::CompleteFile(google::protobuf::RpcController* /*controller*/,
                                       const protocol::CompleteFileRequest* request,
                                       protocol::CompleteFileResponse* response,
                                       google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    // Get located blocks to compute final file length
    auto blocks_result = block_mgr_->get_located_blocks(request->inode_id());
    if (blocks_result.hasError()) {
        fill_status(response->mutable_status(),
                    blocks_result.error().code(),
                    blocks_result.error().message());
        return;
    }

    uint64_t total_length = 0;
    for (const auto& b : blocks_result.value()) {
        total_length += b.length;
    }

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }

    // Complete the file (transition state)
    auto complete_result = ns_mgr_->complete_file(request->inode_id(), total_length);
    if (complete_result.hasError()) {
        fill_status(response->mutable_status(),
                    complete_result.error().code(),
                    complete_result.error().message());
        return;
    }

    // Release lease. A lease mismatch must not be hidden behind a successful complete.
    auto release_result = lease_mgr_->release_lease(request->inode_id(), request->client_id());
    if (release_result.hasError()) {
        fill_status(response->mutable_status(),
                    release_result.error().code(),
                    release_result.error().message());
        return;
    }

    auto commit_result = txn.value()->commit();
    if (commit_result.hasError()) {
        fill_status(response->mutable_status(),
                    commit_result.error().code(),
                    commit_result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
}

void NameNodeServiceImpl::AppendFile(google::protobuf::RpcController* /*controller*/,
                                     const protocol::AppendFileRequest* request,
                                     protocol::AppendFileResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }
    auto inode = ns_mgr_->begin_append(request->path());
    if (inode.hasError()) {
        fill_status(response->mutable_status(), inode.error().code(), inode.error().message());
        return;
    }
    auto lease = lease_mgr_->acquire_lease(inode.value().inode_id, request->client_id());
    if (lease.hasError()) {
        fill_status(response->mutable_status(), lease.error().code(), lease.error().message());
        return;
    }
    auto blocks = metadata_store_->get_blocks_by_inode(inode.value().inode_id);
    if (blocks.hasError()) {
        fill_status(response->mutable_status(), blocks.error().code(), blocks.error().message());
        return;
    }

    uint32_t next_block_index = 0;
    for (const auto& block : blocks.value()) {
        next_block_index = std::max(next_block_index, block.block_index + 1);
    }
    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        fill_status(response->mutable_status(), commit.error().code(), commit.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    response->set_inode_id(inode.value().inode_id);
    response->set_lease_id(lease.value().lease_id);
    response->set_next_block_index(next_block_index);
    response->set_block_size(inode.value().block_size);
    response->set_replication(inode.value().replication);
}

void NameNodeServiceImpl::GetFileStatus(google::protobuf::RpcController* /*controller*/,
                                        const protocol::GetFileStatusRequest* request,
                                        protocol::GetFileStatusResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = ns_mgr_->get_file_status(request->path());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    fill_file_status(response->mutable_file_status(), result.value());
}

void NameNodeServiceImpl::ListStatus(google::protobuf::RpcController* /*controller*/,
                                     const protocol::ListStatusRequest* request,
                                     protocol::ListStatusResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = ns_mgr_->list_status(request->path());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    for (const auto& entry : result.value()) {
        fill_file_status(response->add_entries(), entry);
    }
}

void NameNodeServiceImpl::Delete(google::protobuf::RpcController* /*controller*/,
                                 const protocol::DeleteRequest* request,
                                 protocol::DeleteResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }

    auto target = ns_mgr_->resolve_path(request->path());
    if (target.hasError()) {
        fill_status(response->mutable_status(), target.error().code(), target.error().message());
        return;
    }

    if (target.value().type == InodeType::kFile) {
        auto invalidate = block_mgr_->invalidate_blocks(target.value().inode_id);
        if (invalidate.hasError()) {
            fill_status(response->mutable_status(),
                        invalidate.error().code(),
                        invalidate.error().message());
            return;
        }
    } else if (request->recursive()) {
        std::vector<uint64_t> dir_stack = {target.value().inode_id};
        while (!dir_stack.empty()) {
            uint64_t dir_id = dir_stack.back();
            dir_stack.pop_back();
            auto children = metadata_store_->list_children(dir_id);
            if (children.hasError()) {
                fill_status(response->mutable_status(),
                            children.error().code(),
                            children.error().message());
                return;
            }
            for (const auto& child : children.value()) {
                if (child.type == InodeType::kFile) {
                    auto invalidate = block_mgr_->invalidate_blocks(child.inode_id);
                    if (invalidate.hasError()) {
                        fill_status(response->mutable_status(),
                                    invalidate.error().code(),
                                    invalidate.error().message());
                        return;
                    }
                } else if (child.type == InodeType::kDirectory) {
                    dir_stack.push_back(child.inode_id);
                }
            }
        }
    }

    auto result = ns_mgr_->remove(request->path(), request->recursive());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header() && !request->header().request_id().empty()) {
        auto payload = make_oplog_payload(request->header(),
                                          "delete",
                                          result.value().inode_id,
                                          0,
                                          "",
                                          "{}");
        auto log = write_oplog("delete", result.value().inode_id, request->header(), payload);
        if (log.hasError()) {
            fill_status(response->mutable_status(), log.error().code(), log.error().message());
            return;
        }
    }

    auto commit_result = txn.value()->commit();
    if (commit_result.hasError()) {
        fill_status(response->mutable_status(),
                    commit_result.error().code(),
                    commit_result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
}

void NameNodeServiceImpl::Rename(google::protobuf::RpcController* /*controller*/,
                                 const protocol::RenameRequest* request,
                                 protocol::RenameResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    std::lock_guard rename_lock(rename_mu_);

    if (request->has_header() && check_idempotent(request->header(), response->mutable_status())) {
        return;
    }

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }

    auto result = ns_mgr_->rename(request->src(), request->dst());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header() && !request->header().request_id().empty()) {
        auto payload = make_oplog_payload(request->header(), "rename", 0, 0, "", "{}");
        auto log = write_oplog("rename", 0, request->header(), payload);
        if (log.hasError()) {
            fill_status(response->mutable_status(), log.error().code(), log.error().message());
            return;
        }
    }

    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        fill_status(response->mutable_status(), commit.error().code(), commit.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
}

void NameNodeServiceImpl::TruncateFile(google::protobuf::RpcController* /*controller*/,
                                       const protocol::TruncateFileRequest* request,
                                       protocol::TruncateFileResponse* response,
                                       google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    auto inode = ns_mgr_->resolve_path(request->path());
    if (inode.hasError()) {
        fill_status(response->mutable_status(), inode.error().code(), inode.error().message());
        return;
    }

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }
    auto truncate =
        block_mgr_->truncate_file(inode.value().inode_id,
                                  request->length(),
                                  [this](const BlockReplica& replica, uint64_t length) {
                                      auto token = issue_block_token(replica.block_id,
                                                                     replica.generation_stamp,
                                                                     0,
                                                                     0,
                                                                     kBlockTokenPermissionTruncate);
                                      return truncate_replica(metadata_store_, token, replica, length);
                                  });
    if (truncate.hasError()) {
        fill_status(
            response->mutable_status(), truncate.error().code(), truncate.error().message());
        return;
    }
    auto update = ns_mgr_->set_file_length(inode.value().inode_id, request->length());
    if (update.hasError()) {
        fill_status(response->mutable_status(), update.error().code(), update.error().message());
        return;
    }
    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        fill_status(response->mutable_status(), commit.error().code(), commit.error().message());
        return;
    }
    fill_status(response->mutable_status(), 0);
}

void NameNodeServiceImpl::SetReplication(google::protobuf::RpcController* /*controller*/,
                                         const protocol::SetReplicationRequest* request,
                                         protocol::SetReplicationResponse* response,
                                         google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    if (request->replication() == 0) {
        fill_status(response->mutable_status(),
                    static_cast<uint32_t>(ErrorCode::kInvalidArgument),
                    "replication must be positive");
        return;
    }
    auto inode = ns_mgr_->resolve_path(request->path());
    if (inode.hasError()) {
        fill_status(response->mutable_status(), inode.error().code(), inode.error().message());
        return;
    }

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }
    auto update = ns_mgr_->set_replication(inode.value().inode_id, request->replication());
    if (update.hasError()) {
        fill_status(response->mutable_status(), update.error().code(), update.error().message());
        return;
    }
    auto blocks = block_mgr_->set_replication(inode.value().inode_id, request->replication());
    if (blocks.hasError()) {
        fill_status(response->mutable_status(), blocks.error().code(), blocks.error().message());
        return;
    }
    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        fill_status(response->mutable_status(), commit.error().code(), commit.error().message());
        return;
    }
    fill_status(response->mutable_status(), 0);
}

// Block Operations
void NameNodeServiceImpl::AllocateBlock(google::protobuf::RpcController* /*controller*/,
                                        const protocol::AllocateBlockRequest* request,
                                        protocol::AllocateBlockResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    // Replays may mint a fresh short-lived write token, so they must still prove
    // ownership of the active file lease before consulting the idempotency log.
    auto lease = lease_mgr_->validate_lease(request->inode_id(), request->client_id());
    if (lease.hasError()) {
        fill_status(response->mutable_status(), lease.error().code(), lease.error().message());
        return;
    }

    if (request->has_header() && !request->header().request_id().empty()) {
        auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
        if (existing.hasError()) {
            fill_status(response->mutable_status(), existing.error().code(), existing.error().message());
            return;
        }
        if (existing.value().has_value()) {
            uint32_t status_code = 0;
            uint64_t target_inode_id = 0;
            std::string status_message;
            std::string response_json;
            if (!parse_response_status(existing.value().value(),
                                       "allocate_block",
                                       &status_code,
                                       &status_message,
                                       &target_inode_id,
                                       &response_json)) {
                fill_status(response->mutable_status(),
                            static_cast<uint32_t>(ErrorCode::kInternalError),
                            "invalid allocate_block idempotency payload");
                return;
            }
            fill_status(response->mutable_status(), status_code, status_message);
            if (status_code == 0) {
                if (!parse_located_block(response_json, response->mutable_block())) {
                    auto rebuilt = LocatedBlock{};
                    if (!rebuild_allocate_block_from_metadata(metadata_store_,
                                                              request->inode_id(),
                                                              request->block_index(),
                                                              &rebuilt)) {
                        fill_status(response->mutable_status(),
                                    static_cast<uint32_t>(ErrorCode::kInternalError),
                                    "failed to rebuild allocate_block response");
                        return;
                    }
                    fill_located_block(response->mutable_block(), rebuilt);
                }
                *response->mutable_block()->mutable_block_token() =
                    issue_block_token(response->block().block_id(),
                                      response->block().generation_stamp(),
                                      request->inode_id(),
                                      request->block_index(),
                                      kBlockTokenPermissionWrite);
            }
            return;
        }
    }

    uint32_t replication =
        request->replication() > 0 ? request->replication() : kDefaultReplication;

    auto txn = metadata_store_->begin_transaction();
    if (txn.hasError()) {
        fill_status(response->mutable_status(), txn.error().code(), txn.error().message());
        return;
    }

    auto result = block_mgr_->allocate_block_in_transaction(
        request->inode_id(), request->block_index(), replication);
    if (result.hasError()) {
        if (request->has_header() && !request->header().request_id().empty()) {
            auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
            if (existing.hasValue() && existing.value().has_value()) {
                uint32_t status_code = 0;
                uint64_t target_inode_id = 0;
                std::string status_message;
                std::string response_json;
                if (parse_response_status(existing.value().value(),
                                          "allocate_block",
                                          &status_code,
                                          &status_message,
                                          &target_inode_id,
                                          &response_json)) {
                    fill_status(response->mutable_status(), status_code, status_message);
                    if (status_code == 0) {
                        if (!parse_located_block(response_json, response->mutable_block())) {
                            auto rebuilt = LocatedBlock{};
                            if (!rebuild_allocate_block_from_metadata(metadata_store_,
                                                                      request->inode_id(),
                                                                      request->block_index(),
                                                                      &rebuilt)) {
                                fill_status(response->mutable_status(),
                                            static_cast<uint32_t>(ErrorCode::kInternalError),
                                            "failed to rebuild allocate_block response");
                                return;
                            }
                            fill_located_block(response->mutable_block(), rebuilt);
                        }
                        *response->mutable_block()->mutable_block_token() =
                            issue_block_token(response->block().block_id(),
                                              response->block().generation_stamp(),
                                              request->inode_id(),
                                              request->block_index(),
                                              all_data_plane_permissions());
                    }
                    return;
                }
            }
        }
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    if (request->has_header() && !request->header().request_id().empty()) {
        auto payload = make_oplog_payload(request->header(),
                                          "allocate_block",
                                          request->inode_id(),
                                          0,
                                          "",
                                          located_block_to_json(result.value()));
        auto log = write_oplog("allocate_block", request->inode_id(), request->header(), payload);
        if (log.hasError()) {
            auto existing = metadata_store_->get_oplog_by_request_id(request->header().request_id());
            if (existing.hasValue() && existing.value().has_value()) {
                uint32_t status_code = 0;
                uint64_t target_inode_id = 0;
                std::string status_message;
                std::string response_json;
                if (parse_response_status(existing.value().value(),
                                          "allocate_block",
                                          &status_code,
                                          &status_message,
                                          &target_inode_id,
                                          &response_json)) {
                    fill_status(response->mutable_status(), status_code, status_message);
                    if (status_code == 0) {
                        if (!parse_located_block(response_json, response->mutable_block())) {
                            auto rebuilt = LocatedBlock{};
                            if (!rebuild_allocate_block_from_metadata(metadata_store_,
                                                                      request->inode_id(),
                                                                      request->block_index(),
                                                                      &rebuilt)) {
                                fill_status(response->mutable_status(),
                                            static_cast<uint32_t>(ErrorCode::kInternalError),
                                            "failed to rebuild allocate_block response");
                                return;
                            }
                            fill_located_block(response->mutable_block(), rebuilt);
                        }
                        *response->mutable_block()->mutable_block_token() =
                            issue_block_token(response->block().block_id(),
                                              response->block().generation_stamp(),
                                              request->inode_id(),
                                              request->block_index(),
                                              all_data_plane_permissions());
                    }
                    return;
                }
            }
            fill_status(response->mutable_status(), log.error().code(), log.error().message());
            return;
        }
    }

    auto commit = txn.value()->commit();
    if (commit.hasError()) {
        fill_status(response->mutable_status(), commit.error().code(), commit.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    fill_located_block(response->mutable_block(), result.value());
}

void NameNodeServiceImpl::GetLocatedBlocks(google::protobuf::RpcController* /*controller*/,
                                           const protocol::GetLocatedBlocksRequest* request,
                                           protocol::GetLocatedBlocksResponse* response,
                                           google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = block_mgr_->get_located_blocks(request->inode_id());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    for (const auto& lb : result.value()) {
        fill_located_block(response->add_blocks(), lb);
    }
}

// Lease Operations
void NameNodeServiceImpl::RenewLease(google::protobuf::RpcController* /*controller*/,
                                     const protocol::RenewLeaseRequest* request,
                                     protocol::RenewLeaseResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = lease_mgr_->renew_lease(request->inode_id(), request->client_id());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }
    fill_status(response->mutable_status(), 0);
}

// DataNodeProtocolServiceImpl
void DataNodeProtocolServiceImpl::fill_status(protocol::StatusProto* proto,
                                              uint32_t code,
                                              std::string_view msg) {
    set_status(proto, code, msg);
}

protocol::BlockTokenProto DataNodeProtocolServiceImpl::issue_block_token(uint64_t block_id,
                                                                           uint64_t generation_stamp,
                                                                           uint64_t inode_id,
                                                                           uint32_t block_index,
                                                                           uint32_t permissions) const {
    return pl::minidfs::issue_block_token(block_id,
                                          generation_stamp,
                                          inode_id,
                                          block_index,
                                          permissions,
                                          block_token_ttl_ms_,
                                          block_token_secret_);
}

DataNodeProtocolServiceImpl::DataNodeProtocolServiceImpl(DataNodeManager* dn_mgr,
                                                         BlockManager* block_mgr,
                                                         NameNodeMaintenance* maintenance,
                                                         std::string block_token_secret,
                                                         uint64_t block_token_ttl_ms)
    : dn_mgr_(dn_mgr),
      block_mgr_(block_mgr),
      maintenance_(maintenance),
      block_token_secret_(std::move(block_token_secret)),
      block_token_ttl_ms_(block_token_ttl_ms) {}

void DataNodeProtocolServiceImpl::RegisterDataNode(google::protobuf::RpcController* /*controller*/,
                                                   const protocol::RegisterDataNodeRequest* request,
                                                   protocol::RegisterDataNodeResponse* response,
                                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = dn_mgr_->register_datanode(request->uuid(),
                                             request->hostname(),
                                             request->ip(),
                                             request->rpc_port(),
                                             request->data_port(),
                                             request->rack(),
                                             request->capacity_bytes());

    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    response->set_datanode_id(result.value());
}

void DataNodeProtocolServiceImpl::Heartbeat(google::protobuf::RpcController* /*controller*/,
                                            const protocol::HeartbeatRequest* request,
                                            protocol::HeartbeatResponse* response,
                                            google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    auto result = dn_mgr_->handle_heartbeat(request->datanode_id(),
                                            request->capacity_bytes(),
                                            request->used_bytes(),
                                            request->free_bytes());
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);

    auto deletions = block_mgr_->get_blocks_to_delete(request->datanode_id());
    if (deletions.hasError()) {
        fill_status(
            response->mutable_status(), deletions.error().code(), deletions.error().message());
        return;
    }
    for (const auto& block : deletions.value()) {
        auto* command = response->add_commands();
        command->set_type(protocol::DataNodeCommand::DELETE);
        command->set_block_id(block.block_id);
        command->set_generation_stamp(block.generation_stamp);
    }

    for (const auto& task : maintenance_->take_replication_tasks(request->datanode_id())) {
        auto target = dn_mgr_->get_datanode(task.target_datanode);
        if (target.hasError() || target.value().state != DataNodeState::kLive) {
            continue;
        }
        auto* command = response->add_commands();
        command->set_type(protocol::DataNodeCommand::REPLICATE);
        command->set_block_id(task.block_id);
        command->set_generation_stamp(task.generation_stamp);
        command->set_inode_id(task.inode_id);
        command->set_block_index(task.block_index);
        command->set_target_host(target.value().ip.empty() ? target.value().hostname
                                                           : target.value().ip);
        command->set_target_port(target.value().data_port);

        auto token = issue_block_token(task.block_id,
                                       task.generation_stamp,
                                       task.inode_id,
                                       task.block_index,
                                       kBlockTokenPermissionTransfer);
        auto* cmd_token = command->mutable_block_token();
        cmd_token->CopyFrom(token);
    }
}

void DataNodeProtocolServiceImpl::BlockReport(google::protobuf::RpcController* /*controller*/,
                                              const protocol::BlockReportRequest* request,
                                              protocol::BlockReportResponse* response,
                                              google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    std::vector<ReportedBlock> reported;
    reported.reserve(request->blocks_size());
    for (const auto& block : request->blocks()) {
        reported.push_back({
            .block_id = block.block_id(),
            .generation_stamp = block.generation_stamp(),
            .length = block.length(),
        });
    }

    auto reconcile = block_mgr_->reconcile_block_report(
        request->datanode_id(), reported, request->full_report());
    if (reconcile.hasError()) {
        fill_status(
            response->mutable_status(), reconcile.error().code(), reconcile.error().message());
        return;
    }

    auto deletions = block_mgr_->get_blocks_to_delete(request->datanode_id());
    if (deletions.hasError()) {
        fill_status(
            response->mutable_status(), deletions.error().code(), deletions.error().message());
        return;
    }

    fill_status(response->mutable_status(), 0);
    for (const auto& block : deletions.value()) {
        auto* deletion = response->add_blocks_to_delete();
        deletion->set_block_id(block.block_id);
        deletion->set_generation_stamp(block.generation_stamp);
    }
}

void DataNodeProtocolServiceImpl::CommitBlock(google::protobuf::RpcController* /*controller*/,
                                              const protocol::CommitBlockRequest* request,
                                              protocol::CommitBlockResponse* response,
                                              google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    std::vector<uint64_t> finalized_datanodes;
    finalized_datanodes.reserve(request->finalized_datanode_ids_size() + 1);
    for (uint64_t datanode_id : request->finalized_datanode_ids()) {
        finalized_datanodes.push_back(datanode_id);
    }
    if (finalized_datanodes.empty() && request->datanode_id() != 0) {
        finalized_datanodes.push_back(request->datanode_id());
    }

    auto result = block_mgr_->commit_block(
        request->block_id(), request->length(), request->generation_stamp(), finalized_datanodes);
    if (result.hasError()) {
        fill_status(response->mutable_status(), result.error().code(), result.error().message());
        return;
    }
    fill_status(response->mutable_status(), 0);
}

} // namespace pl::minidfs
