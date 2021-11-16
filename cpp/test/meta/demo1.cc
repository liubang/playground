#include "demo1.h"

#include <iostream>
#include <string>


constexpr int foo = meta::fun<10>;

int main(int argc, char* argv[])
{
    // unsigned int a = 10;
    meta::Fun_<int>::type a = 10;
    meta::Fun_t<int>      b = 10;

    meta::Fun_<std::string>::type c = "ok";
    meta::Fun_t<std::string>      d = "ok";


    std::cout << foo << std::endl;
    return 0;
}
