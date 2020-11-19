#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
  std::vector<int> v;
  std::cout << "size: " << v.size() << std::endl;
  std::cout << "capacity: " << v.capacity() << std::endl;

  v.push_back(1);
  v.push_back(2);
  v.push_back(3);
  v.push_back(4);
  v.push_back(5);
  std::cout << "size: " << v.size() << std::endl;
  std::cout << "capacity: " << v.capacity() << std::endl;

  v.clear();
  std::cout << "size: " << v.size() << std::endl;
  std::cout << "capacity: " << v.capacity() << std::endl;

  v.shrink_to_fit();
  std::cout << "size: " << v.size() << std::endl;
  std::cout << "capacity: " << v.capacity() << std::endl;

  return 0;
}
