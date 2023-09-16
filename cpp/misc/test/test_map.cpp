//=====================================================================
//
// test_map.cpp -
//
// Created by liubang on 2023/06/12 11:20
// Last Modified: 2023/06/12 11:20
//
//=====================================================================
#include <iostream>
#include <map>
#include <string>

auto main(int argc, char *argv[]) -> int {
    {
        std::map<std::string, std::string> mmp;
        mmp["A"] = "A";
        mmp["E"] = "E";
        mmp["D"] = "D";
        mmp["F"] = "F";
        mmp["H"] = "H";

        for (auto &[name, nn] : mmp) {
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

        for (auto &[name, nn] : mmp) {
            std::cout << name << " ==> " << nn << "\n";
        }

        // 这里的upper返回的是idx的upper
        auto itr = mmp.upper_bound("G");
        // 由于前面是从大到小排序，所以这里执行后的结果为: F
        std::cout << "the upper_bound(G) is : " << itr->second << "\n";
    }

    return 0;
}
