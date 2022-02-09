#pragma once

// https://stackoverflow.com/questions/1005476/how-to-detect-whether-there-is-a-specific-member-variable-in-class
#include <type_traits>

template <typename T, typename = int>
struct HasX : std::false_type {};

template <typename T>
struct HasX<T, decltype((void)T::x, 0)> : std::true_type {};

// 关于c ++：带有两个参数的Decltype修改类型
// decltype不采用多个参数。您为它传递了一个恰好是逗号表达式的参数。如果逗号表达式的最后一个参数只是一个变量，而不是引入一个临时变量，则整个表达式将求值为引用。
// 如果没有decltype，则可以看到相同的内容：
// #include <iostream>
// int main() {
//    int i = 5;
//    (1, i) = 10;
//    std::cout << i << std::endl;
// }
