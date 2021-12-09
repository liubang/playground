#pragma once
#include <cstddef>

namespace highkyck {
namespace meta {

// 1 ParamType是一个指针或引用，但不是通用引用
template <typename T>
void f1(T& param) {}

static void test1() {
  int x = 27;         // x is int
  const int cx = x;   // cx is const int
  const int& rx = x;  // rx is reference of const int

  f1(x);   // T is int, and param is int&
  f1(cx);  // T is const int, and param is const int&
  f1(rx);  // T is const int, and param is const int&

  // 在第三个例子中，即使rx是一个引用，T也会被推导成一个非引用类型，
  // 这是应为rx的引用性(reference-ness)在类型推导中会被忽略
}

template <typename T>
void f11(const T& param) {}

static void test11() {
  int x = 27;         // x is int
  const int cx = x;   // cx is const int
  const int& rx = x;  // rx is reference of const int

  f11(x);   // T is int, and param is const int&
  f11(cx);  // T is int, and param is const int&
  f11(rx);  // T is int, and param is const int&
}

template <typename T>
void f12(T* param) {}

static void test12() {
  int x = 27;          // x is int
  const int* px = &x;  // px is pointer of int

  f12(&x);  // T is int, and param is int*
  f12(px);  // T is const int, and param is const int*

  // 指针在类型推导中也被忽略了，所以即使传的是px，T也被推导成了const
  // int，而不是const int*
}

// 2 ParamType 是一个通用引用
template <typename T>
void f2(T&& param) {}

static void test2() {
  int x = 27;         // x is int
  const int cx = x;   // x is const int
  const int& rx = x;  // x is reference of const int

  f2(x);   // x is lvalue, T is const int&, and param is int&
  f2(cx);  // cx is lvalue, T is const int&, and param is const int&
  f2(rx);  // rx is lvalue, T is const int&, and param is const int&
  f2(27);  // 27 is rvalue, T is int, and param is int&&
}

// 3 ParamType既不是指针也不是引用
// 这意味着，无论传递什么param都会成为它的一份拷贝：
//      1. 如果实参是一个引用，则忽略引用部分
//      2.
//      如果忽略引用之后，实参还是一个const，那就再忽略const，如果它还是volatile，那么也忽略
template <typename T>
void f3(T param) {}

static void test3() {
  int x = 27;         // x is int
  const int cx = x;   // x is const int
  const int& rx = x;  // x is reference of const int

  f3(x);   // T and param both are int
  f3(cx);  // T and param both are int
  f3(rx);  // T and param both are int

  const char* const ptr = "hello world";

  f3(ptr);  // param is const char*
}

// calculate array size
template <typename T, std::size_t N>
constexpr std::size_t array_size(T (&)[N]) noexcept {
  return N;
}

// 总结：
//      1. 在模板类型推导时，有引用的实参会被视为无引用，他们的引用会被忽略
//      2. 对于通用引用的推导，左值实参会被特殊对待
//      3.
//      对于传值类型推导，const和volatile实参会被认为是non-const和non-volatile的
//      4.
//      在模板类型推导时，数组名和函数名实参会退化成指针，除非他们被用于初始化引用

}  // namespace meta
}  // namespace highkyck
