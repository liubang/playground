/**
 * 2. 形参为通用引用类型
 *
 * 当expr是一个左值的时候，T 和 param 都推导为左值引用类型，应用了引用折叠规则
 * 当expr是一个右值的时候，T 推导为值类型，param 推导为右值引用类型
 *
 * // param is a universal reference
 * template <typename T>
 * void f(T &&param);
 *
 * // param is a rvalue reference
 * template <typename T>
 * void f(const T &&param);
 *
 * // X is a universal reference
 * auto &&X = expr;
 *
 * // Y is a rvalue reference
 * const auto &&Y = expr;
 *
 * 注意通用引用的两种代码形式，不能有任何的修饰符，哪怕是const也不行
 * 通用引用往往与完美转发一起使用，更严格说应该叫转发引用
 *
 */

#include <iostream>
#include <type_traits>

template<typename T> void f(T &&param) {

}
