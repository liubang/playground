#include <iostream>

#include "cpp/tools/bits.h"

int main(int argc, char *argv[]) {
    uint64_t i = 26;
    uint64_t j = pl::Endian::swap(i);
    std::cout << j << std::endl;
    return 0;
}
