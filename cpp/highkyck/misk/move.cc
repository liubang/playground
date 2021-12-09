#include <iostream>
#include <string>

std::string concat(std::string& x, const std::string& y) { return x.append(y); }

std::string concat(std::string& x, std::string&& y) {
  if (y.capacity() - y.size() >= x.size()) {
    y.insert(0, x);
    x.swap(y);
    return x;
  }
  return x.append(y);
}

template <typename T, typename U>
std::string& concat(std::string& x, T&& t, U&& u) {
  return concat(x, std::string(std::forward<T>(t), std::forward<U>(u)));
}

[&](std::string& x, auto&& t, auto&& u) {
  return concat(
      x, std::string(static_cast<decltype(t)>(t), static_cast<decltype(u)>(u)))
}

int main(int argc, char* argv[]) {
  std::string str;
  std::cout << concat(str, "hello", 3) << std::endl;
  return 0;
}
