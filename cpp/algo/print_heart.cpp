#include <cmath>
#include <iostream>

int main(int argc, char* argv[]) {
    auto f = [](int i, int j) {
        double x = ((j * 2) / 15.0 - 3) / 2;
        double y = (i * 2) / 15.0 - 2;
        // clang-format off
        return std::pow(std::pow(x, 2) + std::pow(y, 2) - 1, 3) +
               std::pow(x, 2) * std::pow(y, 3) < 0;
        // clang-format on
    };

    for (int i = 5; i < 25; ++i) {
        for (int j = 0; j < 50; ++j) {
            std::cout << (f(i, j) ? '*' : ' ');
        }
        std::cout << std::endl;
    }

    return 0;
}
