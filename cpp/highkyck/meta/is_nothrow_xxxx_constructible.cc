#include <iostream>
#include <string>
#include <type_traits>

struct A
{};

struct B
{
  B(const B&) {}
};

struct C
{
  C(const C&) noexcept {}
};

struct D
{
  int a;
};

struct E
{
  std::string s;
};

struct F
{
  F(const F&) = default;
};

struct G
{
  G(const G&) = default;
  std::string s;
};

struct H
{
  H(const H&) noexcept {}
  std::string s;
};

int main(int argc, char* argv[])
{
  std::cout << std::boolalpha;
  std::cout << "int:" << std::is_nothrow_copy_constructible<int>::value << std::endl;
  std::cout << "std::string:" << std::is_nothrow_copy_constructible<std::string>::value
            << std::endl;
  std::cout << "A:" << std::is_nothrow_copy_constructible<A>::value << std::endl;
  std::cout << "B:" << std::is_nothrow_copy_constructible<B>::value << std::endl;
  std::cout << "C:" << std::is_nothrow_copy_constructible<C>::value << std::endl;
  std::cout << "D:" << std::is_nothrow_copy_constructible<D>::value << std::endl;
  std::cout << "E:" << std::is_nothrow_copy_constructible<E>::value << std::endl;
  std::cout << "F:" << std::is_nothrow_copy_constructible<F>::value << std::endl;
  std::cout << "G:" << std::is_nothrow_copy_constructible<G>::value << std::endl;
  std::cout << "H:" << std::is_nothrow_copy_constructible<H>::value << std::endl;

  return 0;
}

/*
int:true
std::string:false
A:true
B:false
C:true
D:true
E:false
F:true
G:false
H:true
*/
