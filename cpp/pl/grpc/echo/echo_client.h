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

#pragma once

#include <chrono>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "proto/echo/echo.grpc.pb.h"
#include "proto/echo/stream.grpc.pb.h"

namespace pl {

// Returns current timestamp in microseconds since epoch. All RTT measurements
// use client-local time (not server time) to avoid clock-skew errors.
inline int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class EchoServiceClient {
public:
    explicit EchoServiceClient(std::shared_ptr<::grpc::Channel> channel)
        : stub_(::pl::grpc::proto::EchoService::NewStub(channel)),
          stream_stub_(::pl::grpc::proto::StreamService::NewStub(channel)) {}

    // --- Unary Echo ---
    void DoEcho(const std::string& message) {
        ::grpc::ClientContext context;
        int64_t sent_us = NowUs();

        ::pl::grpc::proto::EchoRequest req;
        req.set_message(message);
        req.set_timestamp_us(sent_us);
        (*req.mutable_headers())["client"] = "cpp";

        ::pl::grpc::proto::EchoResponse resp;
        ::grpc::Status status = stub_->Echo(&context, req, &resp);
        if (status.ok()) {
            int64_t rtt_us = NowUs() - sent_us; // client-local RTT, no clock skew
            std::cout << "[Echo] response: " << resp.message() << " | rtt_us=" << rtt_us
                      << " | server=" << resp.server_id() << std::endl;
        } else {
            std::cout << "[Echo] RPC failed: " << status.error_message() << std::endl;
        }
    }

    // --- Server Streaming ---
    void DoServerStream(const std::string& pattern, int32_t max_responses) {
        ::grpc::ClientContext context;
        ::pl::grpc::proto::ServerStreamRequest req;
        req.set_pattern(pattern);
        req.set_max_responses(max_responses);

        std::unique_ptr<::grpc::ClientReader<::pl::grpc::proto::StreamItem>> reader(
            stream_stub_->ServerStream(&context, req));
        ::pl::grpc::proto::StreamItem item;
        std::cout << "[ServerStream] receiving items (pattern='" << pattern
                  << "', limit=" << max_responses << "):" << std::endl;
        while (reader->Read(&item)) {
            std::cout << "  [" << item.index() << "] " << item.content() << std::endl;
        }
        ::grpc::Status status = reader->Finish();
        if (!status.ok()) {
            std::cout << "[ServerStream] RPC failed: " << status.error_message() << std::endl;
        }
    }

    // --- Client Streaming ---
    void DoClientStream(const std::vector<std::string>& messages) {
        ::grpc::ClientContext context;
        ::pl::grpc::proto::EchoSummary summary;
        std::unique_ptr<::grpc::ClientWriter<::pl::grpc::proto::EchoRequest>> writer(
            stream_stub_->ClientStream(&context, &summary));

        for (const auto& msg : messages) {
            ::pl::grpc::proto::EchoRequest req;
            req.set_message(msg);
            if (!writer->Write(req)) {
                break;
            }
        }
        writer->WritesDone();
        ::grpc::Status status = writer->Finish();
        if (status.ok()) {
            std::cout << "[ClientStream] summary: count=" << summary.message_count()
                      << " | server=" << summary.server_id() << std::endl;
            for (int i = 0; i < summary.messages_size(); ++i) {
                std::cout << "  [" << i << "] " << summary.messages(i) << std::endl;
            }
        } else {
            std::cout << "[ClientStream] RPC failed: " << status.error_message() << std::endl;
        }
    }

    // --- Bidirectional Chat ---
    void DoChat(const std::vector<std::string>& messages_to_send) {
        ::grpc::ClientContext context;
        auto stream = stream_stub_->Chat(&context);

        // Writer thread
        std::thread writer([&stream, &messages_to_send]() {
            for (const auto& content : messages_to_send) {
                ::pl::grpc::proto::ChatMessage msg;
                msg.set_sender("cpp-client");
                msg.set_content(content);
                msg.set_timestamp_us(NowUs());
                if (!stream->Write(msg)) {
                    break; // stream broken, stop writing
                }
            }
            stream->WritesDone();
        });

        // Reader
        ::pl::grpc::proto::ChatMessage response;
        std::cout << "[Chat] round-trip:" << std::endl;
        while (stream->Read(&response)) {
            std::cout << "  " << response.sender() << " → " << response.content() << std::endl;
        }
        writer.join();
        ::grpc::Status status = stream->Finish();
        if (!status.ok()) {
            std::cout << "[Chat] RPC failed: " << status.error_message() << std::endl;
        }
    }

    // --- Health Check ---
    void DoHealthCheck(const std::string& service = "") {
        ::grpc::ClientContext context;
        ::pl::grpc::proto::HealthRequest req;
        req.set_service(service);
        ::pl::grpc::proto::HealthResponse resp;
        ::grpc::Status status = stub_->HealthCheck(&context, req, &resp);
        if (status.ok()) {
            std::cout << "[HealthCheck] status="
                      << ::pl::grpc::proto::HealthResponse_Status_Name(resp.status())
                      << " | server=" << resp.server_id() << " | version=" << resp.version()
                      << " | uptime=" << resp.uptime_seconds() << "s" << std::endl;
        } else {
            std::cout << "[HealthCheck] RPC failed: " << status.error_message() << std::endl;
        }
    }

private:
    std::unique_ptr<::pl::grpc::proto::EchoService::Stub> stub_;
    std::unique_ptr<::pl::grpc::proto::StreamService::Stub> stream_stub_;
};

} // namespace pl
