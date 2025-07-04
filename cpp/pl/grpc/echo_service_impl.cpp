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

#include "echo_service_impl.h"

namespace pl {

::grpc::Status EchoServiceImpl::Echo(::grpc::ServerContext* context,
                                     const ::pl::grpc::proto::EchoRequest* request,
                                     ::pl::grpc::proto::EchoResponse* response) {
    std::ignore = context;
    response->set_message(request->message());
    return ::grpc::Status::OK;
}

::grpc::Status EchoServiceImpl::Chat(
    ::grpc::ServerContext* context,
    ::grpc::ServerReaderWriter<::pl::grpc::proto::EchoResponse, ::pl::grpc::proto::EchoRequest>*
        stream) {
    std::ignore = context;
    ::pl::grpc::proto::EchoRequest request;
    ::pl::grpc::proto::EchoResponse response;
    while (stream->Read(&request)) {
        std::cout << "read stream" << std::endl;
        response.set_message(request.message());
        std::cout << "write stream" << std::endl;
        stream->Write(response);
    }
    return ::grpc::Status::OK;
}

} // namespace pl
