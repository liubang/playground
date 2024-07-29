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

#include "cpp/pl/log/logger.h"

#include <arpa/inet.h>
#include <cstdio>
#include <fcntl.h>
#include <fmt/format.h>
#include <memory>
#include <netdb.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <sys/event.h>
#elif defined(__LINUX__)
#include <sys/epoll.h>
#else
#error "unsupported operation system"
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace pl {

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

    [[nodiscard]] SocketAddr get_addr() const { return {curr->ai_addr, curr->ai_addrlen}; }

    [[nodiscard]] int create_socket() const {
        int fd = ::socket(curr->ai_family, curr->ai_socktype, curr->ai_protocol);
        if (fd == -1) {
            LOG(ERROR) << "socket: " << ::strerror(errno);
            throw;
        }
        return fd;
    }

    [[nodiscard]] int create_socket_and_bind() const {
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
        int ret = ::getaddrinfo(name.c_str(), service.c_str(), nullptr, &head);
        if (ret != 0) {
            LOG(ERROR) << "getaddrinfo: " << ::gai_strerror(ret);
            throw;
        }
    }

    AddrResolvedEntry get_first_entry() { return {head}; }
};

class TcpServer {
public:
    void init(const std::string& host, const std::string& service);

    void start();

protected:
    virtual void on_accept(int connect_id) = 0;

protected:
    std::unique_ptr<AddrResolver> resolver_;
};

} // namespace pl
