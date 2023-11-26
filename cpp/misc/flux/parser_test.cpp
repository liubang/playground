//=====================================================================
//
// parser_test.cpp -
//
// Created by liubang on 2023/11/09 16:22
// Last Modified: 2023/11/09 16:22
//
//=====================================================================
#include <iostream>
#include <memory>

#include "parser.h"

int main(int argc, char* argv[]) {
    std::string flux = R"(
    import "array"
    import "math"
    import "influxdata/influxdb/sample"

    from(bucket:"telegraf/autogen")
        |> range(start:-1h)
        |> filter(fn:(r) =>
            r._measurement == "cpu" and
            r.cpu == "cpu-total"
        )
        |> aggregateWindow(every: 1m, fn: mean)
    )";
    // std::string flux = R"(
    // from(bucket:"telegraf/autogen")
    //     |> range(start:-1h)
    // )";

    pl::Parser parser(flux);
    auto file = parser.parse_file("");

    std::cout << __FILE_NAME__ << ":" << __LITTLE_ENDIAN << " ==> " << file->name << "\n";
    std::cout << __FILE_NAME__ << ":" << __LITTLE_ENDIAN << " ==> " << file->body.size() << "\n";

    for (const auto& stmt : file->body) {
        std::cout << static_cast<int>(stmt->type) << "\n";
    }

    return 0;
}
