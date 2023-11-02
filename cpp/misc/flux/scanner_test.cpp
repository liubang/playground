//=====================================================================
//
// scanner_test.cpp -
//
// Created by liubang on 2023/11/02 09:54
// Last Modified: 2023/11/02 09:54
//
//=====================================================================
#include <iostream>
#include <memory>

#include "scanner.h"

int main(int argc, char* argv[]) {
    std::string flux = R"(
    from(bucket:"telegraf/autogen")
        |> range(start:-1h)
        |> filter(fn:(r) =>
            r._measurement == "😄🫵👌" and
            r.cpu == "cpu-total"
        )
        |> aggregateWindow(every: 1m, fn: mean)
    )";

    std::unique_ptr<pl::Scanner> scanner = std::make_unique<pl::Scanner>(flux.data(), flux.size());

    for (;;) {
        auto token = scanner->scan();
        if (token->tok == pl::TokenType::Eof) {
            break;
        }
        std::cout << (*token) << "\n";
    }

    return 0;
}
