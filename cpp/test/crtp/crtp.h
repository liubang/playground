#pragma once

#include <iostream>

namespace test {
namespace crtp {

template <typename Drived>
class Base {
 public:
  void interface() { static_cast<Drived*>(this)->impl(); }

 private:
  Base() = default;
  friend Drived;
};

class Drived1 : public Base<Drived1> {
 public:
  void impl() { std::cout << "Drived1::impl" << std::endl; }
};

// error: call to implicitly-deleted default constructor of
// 'test::crtp::Drived2'
/*
class Drived2 : public Base<Drived1> {
 public:
  void impl() { std::cout << "Drived2::impl" << std::endl; }
};
*/

}  // namespace crtp
}  // namespace test
