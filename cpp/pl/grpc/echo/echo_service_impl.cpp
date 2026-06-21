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
// Created: 2026/06/21 00:00

#include "echo_service_impl.h"

#include <chrono>
#include <regex>

namespace pl {

EchoServiceImpl::EchoServiceImpl(std::string server_id)
    : server_id_(std::move(server_id)), start_time_(std::chrono::steady_clock::now()) {}

::grpc::Status EchoServiceImpl::Echo(::grpc::ServerContext* context,
                                     const ::pl::grpc::proto::EchoRequest* request,
                                     ::pl::grpc::proto::EchoResponse* response) {
    std::ignore = context;
    response->set_message(request->message());
    response->set_original_timestamp(request->timestamp_us());
    response->set_server_timestamp(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    response->set_server_id(server_id_);
    return ::grpc::Status::OK;
}

::grpc::Status EchoServiceImpl::ServerStream(
    ::grpc::ServerContext* context,
    const ::pl::grpc::proto::ServerStreamRequest* request,
    ::grpc::ServerWriter<::pl::grpc::proto::StreamItem>* writer) {
    std::ignore = context;

    // Build a canned list of items to stream
    static const std::vector<std::string> kItems = {
        "Alpha",
        "Bravo",
        "Charlie",
        "Delta",
        "Echo",
        "Foxtrot",
        "Golf",
        "Hotel",
        "India",
        "Juliet",
    };

    std::regex pattern;
    bool has_pattern = !request->pattern().empty();
    try {
        if (has_pattern) {
            pattern = std::regex(request->pattern(), std::regex::icase);
        }
    } catch (const std::regex_error&) {
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "Invalid regex pattern");
    }

    int32_t count = 0;
    int32_t limit = request->max_responses();
    if (limit <= 0) {
        limit = static_cast<int32_t>(kItems.size());
    }

    for (int i = 0; i < static_cast<int>(kItems.size()) && count < limit; ++i) {
        if (has_pattern && !std::regex_search(kItems[i], pattern)) {
            continue;
        }
        ::pl::grpc::proto::StreamItem item;
        item.set_index(i);
        item.set_content(kItems[i]);
        if (!writer->Write(item)) {
            break; // client disconnected
        }
        ++count;
    }
    return ::grpc::Status::OK;
}

::grpc::Status EchoServiceImpl::ClientStream(
    ::grpc::ServerContext* context,
    ::grpc::ServerReader<::pl::grpc::proto::EchoRequest>* reader,
    ::pl::grpc::proto::EchoSummary* summary) {
    std::ignore = context;

    ::pl::grpc::proto::EchoRequest request;
    int32_t count = 0;
    while (reader->Read(&request)) {
        summary->add_messages(request.message());
        ++count;
    }
    summary->set_message_count(count);
    summary->set_server_timestamp(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    summary->set_server_id(server_id_);
    return ::grpc::Status::OK;
}

::grpc::Status EchoServiceImpl::Chat(
    ::grpc::ServerContext* context,
    ::grpc::ServerReaderWriter<::pl::grpc::proto::ChatMessage, ::pl::grpc::proto::ChatMessage>*
        stream) {
    std::ignore = context;
    ::pl::grpc::proto::ChatMessage msg;
    while (stream->Read(&msg)) {
        stream->Write(msg);
    }
    return ::grpc::Status::OK;
}

::grpc::Status EchoServiceImpl::HealthCheck(::grpc::ServerContext* context,
                                            const ::pl::grpc::proto::HealthRequest* request,
                                            ::pl::grpc::proto::HealthResponse* response) {
    std::ignore = context;
    std::ignore = request;
    response->set_status(::pl::grpc::proto::HealthResponse::SERVING);
    response->set_server_id(server_id_);
    response->set_version("1.0.0");
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_);
    response->set_uptime_seconds(static_cast<int64_t>(uptime.count()));
    return ::grpc::Status::OK;
}

} // namespace pl
