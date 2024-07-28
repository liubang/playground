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

#include "cpp/pl/http/tcp.h"

#include <thread>
#include <vector>

namespace pl {

void TcpServer::init(const std::string& host, const std::string& service) {
    resolver_ = std::make_unique<AddrResolver>();
    resolver_->resolve(host, service);
}

std::vector<std::thread> pool;

void TcpServer::start() {
    auto entry = resolver_->get_first_entry();
    int sock_fd = entry.create_socket_and_bind();
    for (;;) {
        SocketAddrStorage peer_addr;
        int conn_id = ::accept(sock_fd, &peer_addr.addr, &peer_addr.addr_len);
        pool.emplace_back([this, conn_id] {
            this->on_accept(conn_id);
        });
    }

    for (auto& t : pool) {
        t.join();
    }
}

} // namespace pl
