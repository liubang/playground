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

#include <grpcpp/grpcpp.h>
#include <regex>
#include <string>

#include "proto/echo/echo.grpc.pb.h"

namespace pl {

class EchoServiceImpl : public ::pl::grpc::proto::EchoService::Service {
public:
    explicit EchoServiceImpl(std::string server_id);

    ::grpc::Status Echo(::grpc::ServerContext* context,
                        const ::pl::grpc::proto::EchoRequest* request,
                        ::pl::grpc::proto::EchoResponse* response) override;

    ::grpc::Status ServerStream(
        ::grpc::ServerContext* context,
        const ::pl::grpc::proto::ServerStreamRequest* request,
        ::grpc::ServerWriter<::pl::grpc::proto::StreamItem>* writer) override;

    ::grpc::Status ClientStream(::grpc::ServerContext* context,
                                ::grpc::ServerReader<::pl::grpc::proto::EchoRequest>* reader,
                                ::pl::grpc::proto::EchoSummary* summary) override;

    ::grpc::Status Chat(
        ::grpc::ServerContext* context,
        ::grpc::ServerReaderWriter<::pl::grpc::proto::ChatMessage, ::pl::grpc::proto::ChatMessage>*
            stream) override;

    ::grpc::Status HealthCheck(::grpc::ServerContext* context,
                               const ::pl::grpc::proto::HealthRequest* request,
                               ::pl::grpc::proto::HealthResponse* response) override;

private:
    std::string server_id_;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace pl
