// Copyright (c) 2024 The Authors. All rights reserved.
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

#ifdef __linux__
#include "echo_service.h"

#include <brpc/controller.h>

void pl::brpc::EchoServiceImpl::Echo(::google::protobuf::RpcController* cntl_base,
                                     const ::pl::brpc::EchoRequest* request,
                                     ::pl::brpc::EchoResponse* response,
                                     ::google::protobuf::Closure* done) {
    ::brpc::ClosureGuard done_guard(done);
    [[__maybe_unused__]] auto* cntl = static_cast<::brpc::Controller*>(cntl_base);
    response->set_message(request->message());
}

#endif // __linux__
