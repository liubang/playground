// 判断整数是不是2的幂次

#include <cstdint>
#include <iostream>

/*
bool is_power_of_two(uint64_t n) {
  // 8      01000
  // 12     01100
  // 13     01101
  // 15     01111
  // 16     10000
  //
  // 不难发现，二进制只有1个bit为1的数字是2的幂次方

  // 依赖gcc扩展
  return __builtin_popcountll(n) == 1;
}
*/

bool is_power_of_two(uint64_t n)
{
  // -n 实际上就是n按位取反然后加1的结果
  // -n = ~n + 1
  //  8     01000
  // -8     10111 + 1
  //        11000   // 消除了末尾的1，保留了原数字最后一位1
  return (n != 0) && ((n & -n) == n);
}
