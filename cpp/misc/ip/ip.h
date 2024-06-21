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

#pragma once

#include "cpp/misc/scope/scope.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace pl {

inline std::optional<std::string> getLocalIp() {
    static constexpr std::string_view REMOTE_ADDRESS = "10.255.255.255";

    struct sockaddr_in remote_server;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return std::nullopt;
    }

    memset(&remote_server, 0, sizeof(remote_server));
    remote_server.sin_family = AF_INET;
    remote_server.sin_addr.s_addr = inet_addr(REMOTE_ADDRESS.data());
    remote_server.sin_port = htons(22);
    int err = ::connect(sock, reinterpret_cast<const struct sockaddr*>(&remote_server),
                        sizeof(remote_server));
    if (err < 0) {
        return std::nullopt;
    }

    SCOPE_EXIT { ::close(sock); };
    struct sockaddr_in local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    err = getsockname(sock, reinterpret_cast<struct sockaddr*>(&local_addr), &local_addr_len);
    char buffer[INET_ADDRSTRLEN];
    std::string local_ip;
    const char* p = inet_ntop(AF_INET, &local_addr.sin_addr, buffer, INET_ADDRSTRLEN);
    if (p != nullptr) {
        local_ip = std::string(buffer);
    }

    return local_ip;
}

} // namespace pl
