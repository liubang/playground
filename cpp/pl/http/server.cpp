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

#include <arpa/inet.h>
#include <cstdio>
#include <fcntl.h>
#include <fmt/format.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "cpp/pl/scope/scope.h"

int main(int argc, char* argv[]) {
    struct addrinfo* addrinfo;
    int ret = ::getaddrinfo("127.0.0.1", "8080", nullptr, &addrinfo);
    if (ret != 0) {
        fmt::println("error: {}", ::gai_strerror(ret));
        return 1;
    }

    SCOPE_EXIT { ::freeaddrinfo(addrinfo); };

    fmt::println("{}", addrinfo->ai_family);   // AF_INET = 2 or AF_INET6 = 10
    fmt::println("{}", addrinfo->ai_socktype); // SOCK_STREAM = 1 or SOCK_DGRAM = 2
    fmt::println("{}", addrinfo->ai_protocol); // IPPROTO_TCP = 6 or IPPROTO_UDP = 17

    int fd = ::socket(addrinfo->ai_family, addrinfo->ai_socktype, addrinfo->ai_protocol);
    if (fd == -1) {
        fmt::println("socket: {}", std::strerror(errno));
        return 1;
    }

    return 0;
}
