//=====================================================================
//
// test_time_format.cpp -
//
// Created by liubang on 2023/06/13 15:03
// Last Modified: 2023/06/13 15:03
//
//=====================================================================

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
