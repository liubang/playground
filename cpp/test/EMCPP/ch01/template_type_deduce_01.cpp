/**
 * 1. 形参为一般引用类型或者指针类型：
 *
 * 推导规则：
 * 1. 如果expr为引用类型，则忽略引用修饰部分
 * 2. expr 与形参进行类型匹配进而确定T的类型，类型匹配总是趋向于更严格的类型
 *
 * 类型的严格性：有修饰符的比没有修饰符的更严格，修饰符多的比修饰符少的更严格
 *
 * // param is a lvalue reference
 * template<typename T> void f(T &param) {}
 *
 * // param is a rvalue reference
 * template<typename T> void f(const T &&param) {}
 *
 * // param is a pointer
 * template<typename T> void f(T *param) {}
 *
 * // deduce type of T from expr
 * // f(expr);
 *
 *
 */


#include <iostream>
#include <type_traits>

// param is a lvalue reference type
template<typename T> void f1(T &param)
{
  if (std::is_same_v<T, int>) {
    std::cout << "T = int\n";
  } else if (std::is_same_v<T, const int>) {
  std:;
    std::cout << "T = const int\n";
  }
}

// param is a pointer type
template<typename T> void f2(T *param)
{
  if (std::is_same_v<T, int>) {
    std::cout << "T = int\n";
  } else if (std::is_same_v<T, const int>) {
    std::cout << "T = const int\n";
  }
}

int main(int argc, char *argv[])
{
  int x = 30;
  int &rx = x;
  const int cx = x;
  const int &crx = cx;

  f1(rx);// T is 'int', param's type is 'int&'
  f1(crx);// T is 'const int', param's type is 'const int&'

  f2(&x);// T is 'int', param's type is 'int*'
  f2(&cx);// T is 'const int', param's type is 'const int*'

  return 0;
}
