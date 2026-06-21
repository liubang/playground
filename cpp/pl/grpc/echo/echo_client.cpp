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

#include "echo_client.h"

#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"

ABSL_FLAG(std::string, target, "localhost:50051", "Server address to connect to");

int main(int argc, char* argv[]) {
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();

    auto channel =
        grpc::CreateChannel(absl::GetFlag(FLAGS_target), grpc::InsecureChannelCredentials());
    pl::EchoServiceClient client(channel);

    std::cout << "============ HealthCheck ============" << std::endl;
    client.DoHealthCheck();

    std::cout << "============ Unary Echo =============" << std::endl;
    client.DoEcho("Hello from C++ client!");

    std::cout << "============ Server Stream ==========" << std::endl;
    client.DoServerStream("[aeiou].*", 5);

    std::cout << "============ Client Stream ==========" << std::endl;
    client.DoClientStream({"msg-1", "msg-2", "msg-3"});

    std::cout << "============ Bidi Chat ==============" << std::endl;
    client.DoChat({"First message", "Second message", "Third message", "Fourth message"});

    return 0;
}
