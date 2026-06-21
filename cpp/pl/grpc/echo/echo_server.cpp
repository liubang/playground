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

#include <grpcpp/security/server_credentials.h>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "echo_service_impl.h"

ABSL_FLAG(std::string, port, "50051", "Server port to listen on");
ABSL_FLAG(std::string, server_id, "cpp-server", "Unique server identifier");

void RunServer() {
    std::string server_address = "0.0.0.0:" + absl::GetFlag(FLAGS_port);
    pl::EchoServiceImpl service(absl::GetFlag(FLAGS_server_id));

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "[C++ EchoServer] Listening on " << server_address
              << " (id: " << absl::GetFlag(FLAGS_server_id) << ")" << std::endl;
    server->Wait();
}

int main(int argc, char* argv[]) {
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();
    RunServer();
    return 0;
}
