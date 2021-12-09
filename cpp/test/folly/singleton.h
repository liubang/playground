#pragma once

#include <memory>
#include <string>

namespace foo {
class Foo {
 public:
  explicit Foo(std::string name, int32_t age) : name_(name), age_(age) {}
  const std::string& getName() const { return name_; }
  const int32_t& getAge() const { return age_; }

  static std::shared_ptr<Foo> getInstance();

 private:
  std::string name_;
  int32_t age_;
};
}  // namespace foo
