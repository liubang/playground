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
#include <thread>

namespace pl {
class EchoServiceClient {
public:
    EchoServiceClient(std::shared_ptr<::grpc::Channel> channel)
        : stub_(::pl::grpc::proto::Echo::NewStub(channel)) {}

    void Echo(const std::string& message) {
        ::grpc::ClientContext context;
        ::pl::grpc::proto::EchoRequest req;
        req.set_message(message);
        ::pl::grpc::proto::EchoResponse resp;
        ::grpc::Status status = stub_->Echo(&context, req, &resp);
        if (!status.ok()) {
            std::cout << "Echo rpc failed." << std::endl;
        } else {
            std::cout << "Echo rpc response: " << resp.message() << std::endl;
        }
    }

    void Chat() {
        ::grpc::ClientContext context;
        std::shared_ptr<::grpc::ClientReaderWriter<::pl::grpc::proto::EchoRequest,
                                                   ::pl::grpc::proto::EchoResponse>>
            stream(stub_->Chat(&context));

        std::thread writer([stream]() {
            std::vector<std::string> messages = {
                "First message",
                "Second message",
                "Third message",
                "Forth message",
            };

            for (const auto& message : messages) {
                std::cout << "Sending message " << message << std::endl;
                ::pl::grpc::proto::EchoRequest request;
                request.set_message(message);
                stream->Write(request);
            }

            stream->WritesDone();
        });

        pl::grpc::proto::EchoResponse response;
        while (stream->Read(&response)) {
            std::cout << "Got message " << response.message() << std::endl;
        }
        writer.join();
        ::grpc::Status status = stream->Finish();
        if (!status.ok()) {
            std::cout << "Chat rpc failed." << std::endl;
        }
    }

private:
    std::unique_ptr<pl::grpc::proto::Echo::Stub> stub_;
};
} // namespace pl
