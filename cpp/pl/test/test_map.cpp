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

#include <iostream>
#include <map>
#include <string>

auto main(int argc, char* argv[]) -> int {
    {
        std::map<std::string, std::string> mmp;
        mmp["A"] = "A";
        mmp["E"] = "E";
        mmp["D"] = "D";
        mmp["F"] = "F";
        mmp["H"] = "H";

        for (auto& [name, nn] : mmp) {
            std::cout << name << " ==> " << nn << "\n";
        }

        auto itr = mmp.upper_bound("G");
        std::cout << "the upper_bound(G) is : " << itr->second << "\n";
    }

    {
        // 按照key从大到小的排序
        std::map<std::string, std::string, std::greater<>> mmp;
        mmp["A"] = "A";
        mmp["E"] = "E";
        mmp["D"] = "D";
        mmp["F"] = "F";
        mmp["H"] = "H";

        for (auto& [name, nn] : mmp) {
            std::cout << name << " ==> " << nn << "\n";
        }

        // 这里的upper返回的是idx的upper
        auto itr = mmp.upper_bound("G");
        // 由于前面是从大到小排序，所以这里执行后的结果为: F
        std::cout << "the upper_bound(G) is : " << itr->second << "\n";
    }

    return 0;
}
