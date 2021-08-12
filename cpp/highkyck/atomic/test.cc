#include <atomic>
#include <iostream>

int main(int argc, char* argv[])
{
  std::atomic<bool> at;
  at = false;

  bool exp = false;
  bool ret = at.compare_exchange_strong(exp, true);
  std::cout << "ret:" << ret << ", at:" << at << std::endl;

  at = false;
  exp = true;
  ret = at.compare_exchange_strong(exp, false);
  std::cout << "ret:" << ret << ", at:" << at << std::endl;
  return 0;
}
