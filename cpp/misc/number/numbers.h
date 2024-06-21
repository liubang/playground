// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#pragma once

#include <cstdint>

namespace pl {

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

inline bool is_power_of_two(uint64_t n) {
    // -n 实际上就是n按位取反然后加1的结果
    // -n = ~n + 1
    //  8     01000
    // -8     10111 + 1
    //        11000   // 消除了末尾的1，保留了原数字最后一位1
    return (n != 0) && ((n & -n) == n);
}

} // namespace pl
