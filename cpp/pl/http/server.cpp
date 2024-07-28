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
#include <thread>
#include <unistd.h>

#include "cpp/pl/log/logger.h"
#include "cpp/pl/scope/scope.h"
#include <vector>

struct SocketAddr {
    struct sockaddr* addr;
    socklen_t addr_len;
};

struct SocketAddrStorage {
    union {
        struct sockaddr addr;
        struct sockaddr_storage addr_storage;
    };

    socklen_t addr_len = sizeof(struct sockaddr_storage);

    operator SocketAddr() { return {&addr, addr_len}; }
};

struct AddrResolvedEntry {
    struct addrinfo* curr = nullptr;

    SocketAddr get_addr() const { return {curr->ai_addr, curr->ai_addrlen}; }

    int create_socket() const {
        int fd = ::socket(curr->ai_family, curr->ai_socktype, curr->ai_protocol);
        if (fd == -1) {
            LOG(ERROR) << "socket: " << ::strerror(errno);
            throw;
        }
        return fd;
    }

    int create_socket_and_bind() const {
        int socket_fd = create_socket();
        SocketAddr server_addr = get_addr();
        int ret = ::bind(socket_fd, server_addr.addr, server_addr.addr_len);
        if (ret == -1) {
            LOG(ERROR) << "socket: " << ::strerror(errno);
            throw;
        }

        ret = ::listen(socket_fd, SOMAXCONN);
        if (ret == -1) {
            LOG(ERROR) << "listen: " << ::strerror(errno);
            throw;
        }

        return socket_fd;
    }

    bool next_entry() {
        curr = curr->ai_next;
        return curr == nullptr;
    }
};

struct AddrResolver {
    struct addrinfo* head = nullptr;

    AddrResolver() = default;
    AddrResolver(AddrResolver&& that) noexcept : head(that.head) { that.head = nullptr; }

    ~AddrResolver() {
        if (head != nullptr) {
            ::freeaddrinfo(head);
        }
    }

    void resolve(const std::string& name, const std::string& service) {
        int ret = ::getaddrinfo(name.c_str(), service.c_str(), NULL, &head);
        if (ret != 0) {
            LOG(ERROR) << "getaddrinfo: " << ::gai_strerror(ret);
            throw;
        }
    }

    AddrResolvedEntry get_first_entry() { return {head}; }
};

std::vector<std::thread> pool;

int main(int argc, char* argv[]) {
    AddrResolver resolver;
    resolver.resolve("127.0.0.1", "8090");
    auto entry = resolver.get_first_entry();
    int socket_fd = entry.create_socket_and_bind();
    LOG(INFO) << "listen 127.0.0.1:8090";
    for (;;) {
        SocketAddrStorage peer_addr;
        int conn_id = ::accept(socket_fd, &peer_addr.addr, &peer_addr.addr_len);
        pool.emplace_back([conn_id] {
            SCOPE_EXIT { ::close(conn_id); };
            char buf[1024];
            int ret = ::read(conn_id, buf, sizeof(buf));
            if (ret == -1) {
                LOG(WARN) << "read: " << ::strerror(errno);
                return;
            }
            auto str = std::string_view(buf, ret);
            LOG(INFO) << "got: " << str;
            std::string_view response = R"(
HTTP/1.1 200 OK

<h1>hello world</h1>
            )";
            ret = ::write(conn_id, response.data(), response.size());
            if (ret == -1) {
                LOG(WARN) << "write: " << ::strerror(errno);
            }
        });
    }

    for (auto& t : pool) {
        t.join();
    }

    return 0;
}
