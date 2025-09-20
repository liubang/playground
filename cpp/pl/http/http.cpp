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

#include "cpp/pl/http/http.h"

#include "cpp/pl/scope/scope.h"

namespace pl {

void HttpServer::on_accept(int connect_id) {
    SCOPE_EXIT { ::close(connect_id); };
    char buf[1024];
    int ret = ::read(connect_id, buf, sizeof(buf));
    if (ret == -1) {
        LOG(WARN) << "read: " << ::strerror(errno);
        return;
    }
    auto str = std::string_view(buf, ret);
    LOG(INFO) << "got: " << str;
    std::string_view response = R"(
HTTP/1.1 200 ok

<h1>hello world</h1>
    )";
    ret = ::write(connect_id, response.data(), response.size());
    if (ret == -1) {
        LOG(WARN) << "write: " << ::strerror(errno);
    }
}

} // namespace pl
