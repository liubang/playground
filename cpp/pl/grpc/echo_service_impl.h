// Copyright (c) 2025 The Authors. All rights reserved.
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

#pragma once

#include "cpp/pl/grpc/proto/echo.grpc.pb.h"

#include <grpcpp/grpcpp.h>

namespace pl {

class EchoServiceImpl : public ::pl::grpc::proto::Echo::Service {
public:
    EchoServiceImpl() = default;
    ~EchoServiceImpl() override = default;

    ::grpc::Status Echo(::grpc::ServerContext* context,
                        const ::pl::grpc::proto::EchoRequest* request,
                        ::pl::grpc::proto::EchoResponse* response) override;
};

} // namespace pl
