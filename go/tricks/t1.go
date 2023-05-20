// Copyright (c) 2022 The Authors. All rights reserved.
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
// Created: 2022/08/09 12:45

package tricks

// a, b, c 的默认类型是 rune (rune 是 int32 的别名)，且 a, b, c 被当作 slice 的下标索引
// 由于 a, b, c 的 ascii 分别为 97, 98, 99, 所以下面的数组初始化等价于
// ```go
//
//	m := [...]int {
//	  97: 1,
//	  98: 2,
//	  99: 3,
//	}
//
// m 的最大下标为 99, 故 m 的长度为 100
// ```
func SliceTricks1() int {
	m := [...]int{
		'a': 1,
		'b': 2,
		'c': 3,
	}
	return len(m)
}
