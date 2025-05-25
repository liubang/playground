// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/ascii_table/ascii_table.h"

#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << "=== Basic Usage ===\n";
    pl::ASCIITable table;
    // 设置表头
    auto header_result = table.setHeader(std::vector<std::string>{"Name", "Age", "City", "Salary"});
    if (!header_result) {
        std::cerr << "Failed to set header\n";
        return 0;
    }

    // 添加数据行
    table.addRow("John Doe", 25, "New York", "$50,000");
    table.addRow("Alice Smith", 30, "London", "$60,000");
    table.addRow("Bob Johnson", 28, "Tokyo", "$55,000");

    // 设置样式
    table.setColumnStyle(1, pl::ascii_table::CellStyle{pl::ascii_table::Alignment::CENTER,
                                                       pl::ascii_table::Color::BLUE});
    table.setColumnStyle(3, pl::ascii_table::CellStyle{pl::ascii_table::Alignment::RIGHT,
                                                       pl::ascii_table::Color::GREEN});

    table.print();
    return 0;
}
