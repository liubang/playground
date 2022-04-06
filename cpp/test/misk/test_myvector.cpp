#include <cassert>
#include <iostream>
#include <vector>

namespace test::misk {

template <typename T>
class MyVector {
 public:
  MyVector(const std::size_t n) : vec_(n) {}
  MyVector(const std::size_t n, const T initvalues) : vec_(n, initvalues) {}

  std::size_t size() const { return vec_.size(); }

  T operator[](const std::size_t i) const {
    assert(i < size());
    return vec_[i];
  }

  T& operator[](const std::size_t i) {
    assert(i < size());
    return vec_[i];
  }

 private:
  std::vector<T> vec_;
};

template <typename T>
MyVector<T> operator+(const MyVector<T>& a, const MyVector<T>& b) {
  assert(a.size() == b.size());
  MyVector<T> result(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    result[i] = a[i] + b[i];
  }
  return result;
}

template <typename T>
MyVector<T> operator*(const MyVector<T>& a, const MyVector<T>& b) {
  assert(a.size() == b.size());
  MyVector<T> result(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    result[i] = a[i] * b[i];
  }
  return result;
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const MyVector<T>& vec) {
  std::cout << '\n';
  for (std::size_t i = 0; i < vec.size(); ++i) {
    os << vec[i] << ' ';
  }
  os << '\n';
  return os;
}

}  // namespace test::misk

int main(int argc, char* argv[]) {
  test::misk::MyVector<double> x(10, 5.4);
  test::misk::MyVector<double> y(10, 10.3);
  auto ret = x + x + y * y;
  std::cout << ret << std::endl;
  return 0;
}
