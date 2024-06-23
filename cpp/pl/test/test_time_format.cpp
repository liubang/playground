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

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

std::string formatTimestamp(const std::chrono::system_clock::time_point& timestamp,
                            const std::string& format) {
    std::time_t t = std::chrono::system_clock::to_time_t(timestamp);
    std::tm* tm = std::localtime(&t);

    std::ostringstream oss;
    oss << std::put_time(tm, format.c_str());

    return oss.str();
}

int main() {
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();

    std::string formattedTimestamp = formatTimestamp(timestamp, "%Y-%m-%d %H:%M:%S");
    std::cout << "Formatted timestamp: " << formattedTimestamp << std::endl;

    return 0;
}
