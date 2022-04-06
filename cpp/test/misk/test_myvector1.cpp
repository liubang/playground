#include <cassert>
#include <iostream>
#include <vector>

namespace test::misk {

template <typename T, typename Cont = std::vector<T>>
class MyVector {
 public:
  MyVector(const std::size_t n) : vec_(n) {}
  MyVector(const std::size_t n, const T initvalues) : vec_(n, initvalues) {}

  MyVector(const Cont& other) : vec_(other) {}

  template <typename T2, typename R2>
  MyVector& operator=(const MyVector<T2, R2>& other) {
    assert(size() == other.size());
    for (std::size_t i = 0; i < size(); ++i) vec_[i] = other[i];
    return *this;
  }

  std::size_t size() const { return vec_.size(); }
  T operator[](const std::size_t i) const { return vec_[i]; }
  T& operator[](const std::size_t i) { return vec_[i]; }
  const Cont& data() const { return vec_; }
  Cont& data() { return vec_; }

 private:
  Cont vec_;
};

template <typename T, typename Op1, typename Op2>
class MyVectorAdd {
 public:
  MyVectorAdd(const Op1& a, const Op2& b) : op1_(a), op2_(b) {}

  T operator[](const std::size_t i) const { return op1_[i] + op2_[i]; }
  std::size_t size() const { return op1_.size(); }

 private:
  const Op1& op1_;
  const Op2& op2_;
};

template <typename T, typename Op1, typename Op2>
class MyVectorMul {
 public:
  MyVectorMul(const Op1& a, const Op2& b) : op1_(a), op2_(b) {}

  T operator[](const std::size_t i) const { return op1_[i] * op2_[i]; }
  std::size_t size() const { return op1_.size(); }

 private:
  const Op1& op1_;
  const Op2& op2_;
};

template <typename T, typename R1, typename R2>
MyVector<T, MyVectorAdd<T, R1, R2>> operator+(const MyVector<T, R1>& a,
                                              const MyVector<T, R2>& b) {
  return MyVector<T, MyVectorAdd<T, R1, R2>>(
      MyVectorAdd<T, R1, R2>(a.data(), b.data()));
}

template <typename T, typename R1, typename R2>
MyVector<T, MyVectorMul<T, R1, R2>> operator*(const MyVector<T, R1>& a,
                                              const MyVector<T, R2>& b) {
  return MyVector<T, MyVectorMul<T, R1, R2>>(
      MyVectorMul<T, R1, R2>(a.data(), b.data()));
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const MyVector<T>& vec) {
  os << '\n';
  for (std::size_t i = 0; i < vec.size(); ++i) os << vec[i] << ' ';
  os << '\n';
  return os;
}

}  // namespace test::misk

int main(int argc, char* argv[]) {
  test::misk::MyVector<double> x(10, 5.4);
  test::misk::MyVector<double> y(10, 10.3);
  test::misk::MyVector<double> result(10);
  result = x + x + y * y;
  std::cout << result << std::endl;
  return 0;
}
