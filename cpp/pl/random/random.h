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

#include <ctime>
#include <random>
#include <string>

namespace pl {

std::string random_string(size_t length) {
    static const std::string characters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::random_device rd;
    static std::mt19937 generator(rd());
    static std::uniform_int_distribution<> distribution(0, characters.size() - 1);

    std::string rand_str;
    rand_str.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        rand_str += characters[distribution(generator)];
    }

    return rand_str;
}

std::vector<uint8_t> random_bytes(size_t size) {
    static std::random_device rd;
    static std::mt19937 generator(rd());
    static std::uniform_int_distribution<uint16_t> distribution(0, 255);

    std::vector<uint8_t> result;
    result.reserve(size);

    for (size_t i = 0; i < size; ++i) {
        result.push_back(static_cast<uint8_t>(distribution(generator)));
    }

    return result;
}

} // namespace pl
